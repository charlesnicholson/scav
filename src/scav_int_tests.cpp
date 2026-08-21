// The division primitives against every sign combination, and the helpers
// against their boundaries -- the table a platform cannot bend.

#include "scav_int.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>

namespace {

using namespace scav;

}  // namespace

TEST_CASE("int: floor_div rounds toward negative infinity for every sign pair") {
  CHECK(floor_div(7, 2) == 3);
  CHECK(floor_div(-7, 2) == -4);
  CHECK(floor_div(7, -2) == -4);
  CHECK(floor_div(-7, -2) == 3);
  CHECK(floor_div(6, 2) == 3);
  CHECK(floor_div(-6, 2) == -3);
  CHECK(floor_div(0, 5) == 0);
  CHECK(floor_div(INT64_C(-9'000'000'000), INT64_C(4)) == INT64_C(-2'250'000'000));
  CHECK(floor_div(INT32_MIN, 1) == INT32_MIN);
}

TEST_CASE("int: floor_mod result carries the divisor's sign") {
  CHECK(floor_mod(7, 3) == 1);
  CHECK(floor_mod(-7, 3) == 2);
  CHECK(floor_mod(7, -3) == -2);
  CHECK(floor_mod(-7, -3) == -1);
  CHECK(floor_mod(6, 3) == 0);
  CHECK(floor_mod(-6, 3) == 0);
  // The identity that makes the pair a pair.
  for (int32_t a : { -7, -1, 0, 1, 7 }) {
    for (int32_t b : { -3, -2, 2, 3 }) {
      CHECK((floor_div(a, b) * b) + floor_mod(a, b) == a);
    }
  }
}

TEST_CASE("int: ceil_div rounds toward positive infinity, signed and unsigned") {
  CHECK(ceil_div(7, 2) == 4);
  CHECK(ceil_div(-7, 2) == -3);
  CHECK(ceil_div(7, -2) == -3);
  CHECK(ceil_div(-7, -2) == 4);
  CHECK(ceil_div(6, 2) == 3);
  CHECK(ceil_div(0, 5) == 0);
  CHECK(ceil_div(7U, 2U) == 4U);
  CHECK(ceil_div(UINT64_C(0xFFFF'FFFF'FFFF'FFFF), UINT64_C(2))
        == UINT64_C(0x8000'0000'0000'0000));
}

TEST_CASE("int: isqrt is the floor square root across the whole domain") {
  CHECK(isqrt(0) == 0);
  CHECK(isqrt(1) == 1);
  CHECK(isqrt(3) == 1);
  CHECK(isqrt(4) == 2);
  CHECK(isqrt(24) == 4);
  CHECK(isqrt(25) == 5);
  CHECK(isqrt(26) == 5);
  // Around a large perfect square, and the top of the domain: floor(sqrt(2^64
  // - 1)) is 2^32 - 1.
  constexpr uint64_t BIG{ UINT64_C(4'294'967'295) };
  CHECK(isqrt(BIG * BIG) == BIG);
  CHECK(isqrt((BIG * BIG) - 1) == BIG - 1);
  CHECK(isqrt(UINT64_C(0xFFFF'FFFF'FFFF'FFFF)) == BIG);
  static_assert(isqrt(152'399'025) == 12'345);
}

TEST_CASE("int: ilog2 is bit_width minus one") {
  CHECK(ilog2(1U) == 0);
  CHECK(ilog2(2U) == 1);
  CHECK(ilog2(3U) == 1);
  CHECK(ilog2(4U) == 2);
  CHECK(ilog2(UINT64_C(1) << 63U) == 63);
  CHECK(ilog2((UINT64_C(1) << 63U) - 1) == 62);
}

TEST_CASE("int: ratio_less compares by cross-multiplication, never dividing") {
  CHECK(ratio_less(1, 3, 1, 2));
  CHECK(!ratio_less(1, 2, 1, 3));
  CHECK(!ratio_less(2, 4, 1, 2));  // equal ratios are not less either way
  CHECK(!ratio_less(1, 2, 2, 4));
  CHECK(ratio_less(-1, 2, 1, 2));
  CHECK(ratio_less(-3, 2, -1, 2));
  // Distinct ratios a double would conflate: (2^53 + 1) / 2^53 vs 1 / 1.
  constexpr int64_t P53{ INT64_C(1) << 53U };
  CHECK(ratio_less(1, 1, P53 + 1, P53));
}
