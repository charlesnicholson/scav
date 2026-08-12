// Ids, the no-iteration lookup map, and interning. Compiled against
// scavcore_testable.

#include "core/model/interner.h"
#include "core/model/lookup_map.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scav;

std::vector<std::string> pool_strings(StringPool const &pool,
                                      std::vector<StrRef> const &refs) {
  std::vector<std::string> out;
  out.reserve(refs.size());
  for (StrRef const r : refs) { out.emplace_back(string_pool_view(pool, r)); }
  return out;
}

// Interns each input and hands back the pool, the refs, and the pool bytes as one
// string to compare against.
struct Interned {
  StringPool pool;
  std::vector<StrRef> refs;
  std::string bytes;
};

Interned intern_all(std::vector<std::string_view> const &inputs) {
  Interner in;
  Interned out;
  out.refs.reserve(inputs.size());
  for (std::string_view const s : inputs) { out.refs.push_back(intern_bytes(in, s)); }
  out.pool = std::move(in.pool);
  out.bytes.assign(reinterpret_cast<char const *>(out.pool.bytes.data()),
                   out.pool.bytes.size());
  return out;
}

}  // namespace

TEST_CASE("ids: distinct types compare by value") {
  CHECK(DocId{ 3 } == DocId{ 3 });
  CHECK(DocId{ 3 } != DocId{ 4 });
  CHECK(StmtId{ 0 } == StmtId{ 0 });
  CHECK(StmtId{ 1 } != StmtId{ 0 });
  CHECK(str_ref(2, 5) == str_ref(2, 5));
  CHECK(str_ref(2, 5) != str_ref(2, 6));
  CHECK(make_span(0, 0) == make_span(0, 0));
  CHECK(make_span(1, 0) != make_span(0, 0));
}

TEST_CASE("ids: INVALID is the all-ones sentinel") {
  CHECK(INVALID == 0xFFFFFFFFU);
  CHECK(INVALID + 1U == 0U);  // unsigned wrap is defined, so this is not UB
}

TEST_CASE("lookup_map: find returns nullptr rather than an iterator") {
  LookupMap<uint32_t, uint32_t> m;
  CHECK(m.empty());
  CHECK(m.find(7U) == nullptr);
  CHECK(m.insert(7U, 70U));
  CHECK(m.size() == 1);
  REQUIRE(m.find(7U) != nullptr);
  CHECK(*m.find(7U) == 70U);
  CHECK(m.contains(7U));
  CHECK_FALSE(m.contains(8U));
}

TEST_CASE("lookup_map: insert does not overwrite an existing key") {
  LookupMap<uint32_t, uint32_t> m;
  CHECK(m.insert(1U, 10U));
  CHECK_FALSE(m.insert(1U, 99U));
  CHECK(*m.find(1U) == 10U);
}

TEST_CASE("lookup_map: clear and reserve") {
  LookupMap<uint32_t, uint32_t> m;
  m.reserve(64);
  for (uint32_t i = 0; i < 64; ++i) { CHECK(m.insert(i, i * 2)); }
  CHECK(m.size() == 64);
  CHECK(*m.find(63U) == 126U);
  m.clear();
  CHECK(m.empty());
  CHECK(m.find(0U) == nullptr);
}

TEST_CASE("lookup_map: a string map probes with a view and does not allocate a key") {
  StringLookupMap<uint32_t> m;
  CHECK(m.insert(std::string{ "alpha" }, 1));
  std::string_view const probe{ "alpha" };
  REQUIRE(m.find(probe) != nullptr);
  CHECK(*m.find(probe) == 1);
  CHECK(m.find(std::string_view{ "beta" }) == nullptr);
}

TEST_CASE("lookup_map: a mutable find writes through") {
  LookupMap<uint32_t, uint32_t> m;
  CHECK(m.insert(1U, 10U));
  *m.find(1U) = 11U;
  CHECK(*m.find(1U) == 11U);
}

TEST_CASE("narrow: round-trips what fits and refuses what does not") {
  // we ban narrowing without a range check and names one helper for it.
  // This is the boundary between the caller's size_t and the model's uint32.
  uint32_t out{ 0xDEAD };
  CHECK(narrow<uint32_t>(size_t{ 0 }, out));
  CHECK(out == 0);
  CHECK(narrow<uint32_t>(size_t{ 0xFFFFFFFF }, out));
  CHECK(out == 0xFFFFFFFFU);

  if constexpr (sizeof(size_t) > 4) {
    out = 0xDEAD;
    CHECK_FALSE(narrow<uint32_t>(size_t{ 0x100000000ULL }, out));
    // Left alone on refusal, so a caller that ignores the bool gets its own
    // value back rather than a truncated one.
    CHECK(out == 0xDEAD);
    CHECK_FALSE(narrow<uint32_t>(SIZE_MAX, out));
  }

  uint8_t small{ 0 };
  CHECK(narrow<uint8_t>(255U, small));
  CHECK(small == 255);
  CHECK_FALSE(narrow<uint8_t>(256U, small));
  CHECK(small == 255);
}

TEST_CASE("narrow_clamp: clamps rather than wrapping") {
  // A short read is a bug you can find; a wrapped one is a 4-gigabyte read.
  CHECK(narrow_clamp<uint32_t>(size_t{ 7 }) == 7U);
  CHECK(narrow_clamp<uint32_t>(size_t{ 0xFFFFFFFF }) == 0xFFFFFFFFU);
  if constexpr (sizeof(size_t) > 4) {
    CHECK(narrow_clamp<uint32_t>(size_t{ 0x100000000ULL }) == 0xFFFFFFFFU);
    CHECK(narrow_clamp<uint32_t>(SIZE_MAX) == 0xFFFFFFFFU);
  }
  CHECK(narrow_clamp<uint8_t>(300U) == 255U);
}

TEST_CASE("intern: the empty string is a zero ref and never enters the pool") {
  Interner in;
  CHECK(intern_bytes(in, std::string_view{}) == str_ref(0, 0));
  CHECK(in.pool.bytes.empty());
  CHECK(in.unique.empty());

  Interned const r{ intern_all({ "" }) };
  CHECK(r.pool.bytes.empty());
  CHECK(r.refs[0] == str_ref(0, 0));
  CHECK(string_pool_view(r.pool, r.refs[0]).empty());
}

TEST_CASE("intern: the same bytes return the same ref") {
  // The property worth having: StrRef equality is string equality, so comparing
  // two names is comparing two pairs of uint32.
  Interner in;
  StrRef const a{ intern_bytes(in, "hello") };
  StrRef const b{ intern_bytes(in, "hello") };
  CHECK(a == b);
  CHECK(in.unique.size() == 1);
  CHECK(in.pool.bytes.size() == 5);
  CHECK(string_pool_view(in.pool, a) == "hello");
}

TEST_CASE("intern: an embedded NUL is an ordinary byte") {
  Interner in;
  std::string const with_nul{ std::string("a\0b", 3) };
  StrRef const a{ intern_bytes(in, with_nul) };
  StrRef const b{ intern_bytes(in, "ab") };
  CHECK(a != b);
  CHECK(a.len == 3);
  CHECK(in.unique.size() == 2);
}

TEST_CASE("intern: the pool is first-encounter order") {
  // Not sorted. Nothing depends on the pool's byte layout, so nothing pays to
  // canonicalize it; whoever needs a canonical order can sort at that point.
  Interned const r{ intern_all({ "zebra", "apple", "mango" }) };
  CHECK(r.bytes == "zebraapplemango");
  CHECK(pool_strings(r.pool, r.refs) ==
        std::vector<std::string>{ "zebra", "apple", "mango" });
}

TEST_CASE("intern: duplicates are stored once, wherever they recur") {
  Interned const r{ intern_all({ "one", "two", "one", "two", "one" }) };
  CHECK(r.bytes == "onetwo");
  CHECK(r.refs[0] == r.refs[2]);
  CHECK(r.refs[0] == r.refs[4]);
  CHECK(r.refs[1] == r.refs[3]);
}

TEST_CASE("intern: a string that is a prefix of another is still its own entry") {
  // No suffix sharing: `ab` does not alias the first two bytes of `abc`. Spans
  // would allow it, and deliberately nothing tries.
  Interned const r{ intern_all({ "abc", "ab", "abcd", "a" }) };
  CHECK(r.bytes == "abcababcda");
  CHECK(pool_strings(r.pool, r.refs) ==
        std::vector<std::string>{ "abc", "ab", "abcd", "a" });
}

TEST_CASE("intern: high bytes are ordinary bytes") {
  Interned const r{ intern_all({ "\xc3\xa9", "z", "A" }) };
  CHECK(pool_strings(r.pool, r.refs) == std::vector<std::string>{ "\xc3\xa9", "z", "A" });
  CHECK(r.bytes == "\xc3\xa9zA");
}

TEST_CASE("intern: a large distinct set round-trips") {
  std::vector<std::string> owned;
  std::vector<std::string_view> inputs;
  owned.reserve(500);
  for (uint32_t i = 0; i < 500; ++i) {
    owned.push_back("name_" + std::to_string((i * 337U) % 500U));
  }
  inputs.reserve(owned.size());
  for (std::string const &s : owned) { inputs.emplace_back(s); }

  Interned const r{ intern_all(inputs) };
  for (uint32_t i = 0; i < owned.size(); ++i) {
    CHECK(string_pool_view(r.pool, r.refs[i]) == owned[i]);
  }

  // Distinct and deduped, so the pool is the inputs concatenated in order.
  std::string expected;
  for (std::string const &s : owned) { expected += s; }
  CHECK(r.bytes == expected);
}
