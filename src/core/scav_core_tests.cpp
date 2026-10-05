// scav_core.h's own vocabulary: ids, spans, checked narrowing, and reading a
// string pool. The lexer, parser, and normalizer have their own suites.

#include "core/core_internal.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string_view>

namespace {

using namespace scav;

StrRef append(StringPool &pool, std::string_view text) {
  StrRef const ref{ str_ref(narrow_clamp<uint32_t>(pool.bytes.size()),
                            narrow_clamp<uint32_t>(text.size())) };
  pool.bytes.insert(pool.bytes.end(), text.begin(), text.end());
  return ref;
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
  CHECK(INVALID + 1U == 0U);  // unsigned wrap is defined
}

TEST_CASE("narrow: round-trips what fits and refuses what does not") {
  uint32_t out{ 0xDEAD };
  CHECK(narrow<uint32_t>(size_t{ 0 }, out));
  CHECK(out == 0);
  CHECK(narrow<uint32_t>(size_t{ 0xFFFFFFFF }, out));
  CHECK(out == 0xFFFFFFFFU);

  if constexpr (sizeof(size_t) > 4) {
    out = 0xDEAD;
    CHECK_FALSE(narrow<uint32_t>(size_t{ 0x100000000ULL }, out));
    // `out` is unchanged on refusal.
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
  CHECK(narrow_clamp<uint32_t>(size_t{ 7 }) == 7U);
  CHECK(narrow_clamp<uint32_t>(size_t{ 0xFFFFFFFF }) == 0xFFFFFFFFU);
  if constexpr (sizeof(size_t) > 4) {
    CHECK(narrow_clamp<uint32_t>(size_t{ 0x100000000ULL }) == 0xFFFFFFFFU);
    CHECK(narrow_clamp<uint32_t>(SIZE_MAX) == 0xFFFFFFFFU);
  }
  CHECK(narrow_clamp<uint8_t>(300U) == 255U);
}

TEST_CASE("string_pool: a ref reads back the bytes it spans") {
  StringPool pool;
  StrRef const a{ append(pool, "alpha") };
  StrRef const b{ append(pool, "beta") };
  CHECK(string_pool_view(pool, a) == "alpha");
  CHECK(string_pool_view(pool, b) == "beta");
  // The refs are adjacent: one byte past `a` is the first byte of `b`.
  CHECK(string_pool_view(pool, str_ref(a.off, a.len + 1)) == "alphab");
}

TEST_CASE("string_pool: a zero-length ref is empty without touching the pool") {
  StringPool const empty;
  CHECK(string_pool_view(empty, str_ref(0, 0)).empty());
  CHECK(string_pool_view(empty, StrRef{}).empty());
  // A zero-length ref reads empty whatever its offset.
  CHECK(string_pool_view(empty, str_ref(9999, 0)).empty());
}

TEST_CASE("string_pool: an embedded NUL is an ordinary byte") {
  // A NUL is an ordinary byte in a span; the pool appends no terminator.
  StringPool pool;
  StrRef const r{ append(pool, std::string_view{ "a\0b", 3 }) };
  CHECK(r.len == 3);
  CHECK(string_pool_view(pool, r) == std::string_view{ "a\0b", 3 });
  CHECK(pool.bytes.size() == 3);
}

TEST_CASE("string_pool: equal strings get their own bytes") {
  // The pool appends each string without deduplication; StrRef equality compares
  // spans, so the test compares views.
  StringPool pool;
  StrRef const a{ append(pool, "same") };
  StrRef const b{ append(pool, "same") };
  CHECK(a != b);
  CHECK(string_pool_view(pool, a) == string_pool_view(pool, b));
  CHECK(pool.bytes.size() == 8);
}

// Fails on purpose; skipped unless run by name. The build runs it and expects
// a non-zero exit.
TEST_CASE("core: deliberate failure" * doctest::skip()) {
  CHECK_MESSAGE(INVALID == 0, "this failure is intentional");
}
