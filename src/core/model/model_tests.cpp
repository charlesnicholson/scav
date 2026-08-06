// Ids, the no-iteration lookup map, the vendored stable sort, and two-pass
// interning. Compiled against scavcore_testable.

#include "core/model/interner.h"
#include "core/model/lookup_map.h"
#include "core/model/sort.h"
#include "scav/scav_ids.h"
#include "scav/scav_string_pool.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <string>
#include <string_view>
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

// Interns each input, finalizes, and returns the pool bytes as one string plus
// the remapped refs. The whole of two-pass interning in one call.
struct Interned {
  StringPool pool;
  std::vector<StrRef> refs;
  std::string bytes;
};

Interned intern_all(std::vector<std::string_view> const &inputs) {
  Interner in;
  std::vector<StrRef> staged;
  staged.reserve(inputs.size());
  for (std::string_view const s : inputs) { staged.push_back(intern_bytes(in, s)); }

  Interned out;
  StringRemap r;
  intern_finalize(in, out.pool, r);
  out.refs.reserve(staged.size());
  for (StrRef const s : staged) { out.refs.push_back(intern_remap(r, s)); }
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

TEST_CASE("sort: empty and single-element inputs are untouched") {
  std::vector<uint32_t> empty;
  stable_sort_by(empty, [](uint32_t a, uint32_t b) { return a < b; });
  CHECK(empty.empty());

  std::vector<uint32_t> one{ 5 };
  stable_sort_by(one, [](uint32_t a, uint32_t b) { return a < b; });
  CHECK(one == std::vector<uint32_t>{ 5 });
}

TEST_CASE("sort: orders by the functor") {
  std::vector<uint32_t> v{ 9, 3, 7, 1, 8, 2, 6, 4, 5, 0 };
  stable_sort_by(v, [](uint32_t a, uint32_t b) { return a < b; });
  CHECK(v == std::vector<uint32_t>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });

  stable_sort_by(v, [](uint32_t a, uint32_t b) { return a > b; });
  CHECK(v == std::vector<uint32_t>{ 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 });
}

TEST_CASE("sort: equal keys keep their input order") {
  // Every element compares equal on the key, so a stable sort is the identity
  // and an unstable one is free to shuffle. That is the whole property.
  struct Item {
    uint32_t key, tag;
  };
  std::vector<Item> v;
  v.reserve(64);
  for (uint32_t i = 0; i < 64; ++i) { v.push_back({ .key = i % 4U, .tag = i }); }
  stable_sort_by(v, [](Item a, Item b) { return a.key < b.key; });

  for (uint32_t bucket = 0; bucket < 4; ++bucket) {
    uint32_t previous{ 0 };
    bool first{ true };
    for (Item const &it : v) {
      if (it.key != bucket) { continue; }
      if (!first) { CHECK(it.tag > previous); }
      previous = it.tag;
      first = false;
    }
  }
}

TEST_CASE("sort: every length up to 33 sorts, including the odd merge passes") {
  // Bottom-up merging leaves the result in the scratch buffer after an odd
  // number of passes, so lengths on both sides of each power of two matter.
  for (uint32_t n = 0; n <= 33; ++n) {
    std::vector<uint32_t> v;
    v.reserve(n);
    for (uint32_t i = 0; i < n; ++i) { v.push_back((n - i) * 7U % 101U); }
    std::vector<uint32_t> expected{ v };
    stable_sort_by(v, [](uint32_t a, uint32_t b) { return a < b; });
    for (uint32_t i = 1; i < n; ++i) { CHECK(v[i - 1] <= v[i]); }
    CHECK(v.size() == expected.size());
  }
}

TEST_CASE("sort: an already-sorted and a reversed input both land sorted") {
  std::vector<uint32_t> ascending;
  std::vector<uint32_t> descending;
  ascending.reserve(100);
  descending.reserve(100);
  for (uint32_t i = 0; i < 100; ++i) {
    ascending.push_back(i);
    descending.push_back(99 - i);
  }
  auto const less = [](uint32_t a, uint32_t b) { return a < b; };
  stable_sort_by(ascending, less);
  stable_sort_by(descending, less);
  CHECK(ascending == descending);
}

TEST_CASE("compare_bytes: unsigned, byte-wise, shorter prefix first") {
  auto const cmp = [](std::string_view a, std::string_view b) {
    return string_compare_bytes(reinterpret_cast<scav_byte const *>(a.data()),
                                static_cast<uint32_t>(a.size()),
                                reinterpret_cast<scav_byte const *>(b.data()),
                                static_cast<uint32_t>(b.size()));
  };
  CHECK(cmp("a", "a") == 0);
  CHECK(cmp("a", "b") < 0);
  CHECK(cmp("b", "a") > 0);
  CHECK(cmp("ab", "abc") < 0);
  CHECK(cmp("abc", "ab") > 0);
  CHECK(cmp("", "a") < 0);
  CHECK(cmp("", "") == 0);
  // 0x80 is negative as a signed char, which is exactly the bug the unsigned
  // comparison exists to avoid.
  CHECK(cmp("\x7f", "\x80") < 0);
  CHECK(cmp("\xff", "\x01") > 0);
}

TEST_CASE("intern: the empty string is a zero ref and never enters the pool") {
  Interner in;
  CHECK(intern_bytes(in, std::string_view{}) == str_ref(0, 0));
  CHECK(in.staging.empty());
  CHECK(in.unique.empty());

  Interned const r{ intern_all({ "" }) };
  CHECK(r.pool.bytes.empty());
  CHECK(r.refs[0] == str_ref(0, 0));
  CHECK(string_pool_view(r.pool, r.refs[0]).empty());
}

TEST_CASE("intern: the same bytes return the same staging ref") {
  Interner in;
  StrRef const a{ intern_bytes(in, "hello") };
  StrRef const b{ intern_bytes(in, "hello") };
  CHECK(a == b);
  CHECK(in.unique.size() == 1);
  CHECK(in.staging.size() == 5);
  CHECK(intern_staged_view(in, a) == "hello");
  CHECK(intern_staged_view(in, str_ref(0, 0)).empty());
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

TEST_CASE("intern: the finalized pool is sorted by bytes, not by encounter order") {
  Interned const r{ intern_all({ "zebra", "apple", "mango" }) };
  CHECK(r.bytes == "applemangozebra");
  CHECK(pool_strings(r.pool, r.refs) ==
        std::vector<std::string>{ "zebra", "apple", "mango" });
}

TEST_CASE("intern: two encounter orders produce byte-identical pools") {
  // This is the whole reason interning is two-pass. Canonical ordering is by
  // name bytes, so two producers building the same model must emit the same
  // pool no matter who typed what first.
  Interned const forward{ intern_all({ "delta", "alpha", "charlie", "bravo" }) };
  Interned const backward{ intern_all({ "bravo", "charlie", "alpha", "delta" }) };
  CHECK(forward.bytes == backward.bytes);
  CHECK(forward.bytes == "alphabravocharliedelta");

  // And each ref still names the string it was interned from.
  CHECK(pool_strings(forward.pool, forward.refs) ==
        std::vector<std::string>{ "delta", "alpha", "charlie", "bravo" });
  CHECK(pool_strings(backward.pool, backward.refs) ==
        std::vector<std::string>{ "bravo", "charlie", "alpha", "delta" });
}

TEST_CASE("intern: duplicates are stored once") {
  Interned const r{ intern_all({ "one", "two", "one", "two", "one" }) };
  CHECK(r.bytes == "onetwo");
  CHECK(r.refs[0] == r.refs[2]);
  CHECK(r.refs[0] == r.refs[4]);
  CHECK(r.refs[1] == r.refs[3]);
}

TEST_CASE("intern: a prefix sorts before the string that extends it") {
  Interned const r{ intern_all({ "abc", "ab", "abcd", "a" }) };
  CHECK(r.bytes == "aababcabcd");
  CHECK(pool_strings(r.pool, r.refs) ==
        std::vector<std::string>{ "abc", "ab", "abcd", "a" });
}

TEST_CASE("intern: high bytes sort above ASCII") {
  Interned const r{ intern_all({ "\xc3\xa9", "z", "A" }) };
  CHECK(pool_strings(r.pool, r.refs) == std::vector<std::string>{ "\xc3\xa9", "z", "A" });
  CHECK(r.bytes == "Az\xc3\xa9");
}

TEST_CASE("intern: finalizing an empty interner yields an empty pool") {
  Interner in;
  StringPool pool;
  StringRemap r;
  intern_finalize(in, pool, r);
  CHECK(pool.bytes.empty());
  CHECK(r.staging.empty());
  CHECK(intern_remap(r, str_ref(0, 0)) == str_ref(0, 0));
}

TEST_CASE("intern: remapping a ref from another interner degrades to empty") {
  // Reading another string's bytes would be a silent wrong answer; the empty
  // string is at least visibly wrong.
  Interned const r{ intern_all({ "alpha" }) };
  Interner other;
  intern_bytes(other, "beta");
  StringPool other_pool;
  StringRemap other_remap;
  intern_finalize(other, other_pool, other_remap);
  CHECK(intern_remap(other_remap, str_ref(999, 4)) == str_ref(0, 0));
}

TEST_CASE("intern: a large distinct set round-trips through the remap") {
  std::vector<std::string> owned;
  std::vector<std::string_view> inputs;
  owned.reserve(500);
  for (uint32_t i = 0; i < 500; ++i) {
    // Interleaved so first-encounter order is nothing like byte order.
    owned.push_back("name_" + std::to_string((i * 337U) % 500U));
  }
  inputs.reserve(owned.size());
  for (std::string const &s : owned) { inputs.emplace_back(s); }

  Interned const r{ intern_all(inputs) };
  for (uint32_t i = 0; i < owned.size(); ++i) {
    CHECK(string_pool_view(r.pool, r.refs[i]) == owned[i]);
  }

  // Sorted and deduped, so the pool is exactly the concatenation in byte order.
  std::vector<std::string> sorted{ owned };
  stable_sort_by(sorted, [](std::string const &a, std::string const &b) { return a < b; });
  std::string expected;
  for (std::string const &s : sorted) { expected += s; }
  CHECK(r.bytes == expected);
}
