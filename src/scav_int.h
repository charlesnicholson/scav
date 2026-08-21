#ifndef SCAV_INT_H_INCLUDED
#define SCAV_INT_H_INCLUDED

// Integer stand-ins for <cmath> and raw signed division: division rounding
// one documented way for every sign, floor sqrt/log2, ratio comparison.

#include <bit>
#include <cstdint>
#include <type_traits>

namespace scav {

// The only division primitives: floor_div/floor_mod round toward negative
// infinity, ceil_div toward positive. b nonzero; INT_MIN / -1 is the caller's.

template <typename T>
constexpr T floor_div(T a, T b) {
  static_assert(std::is_integral_v<T> && std::is_signed_v<T>,
                "unsigned division already floors; call / directly");
  T const q{ static_cast<T>(a / b) };
  T const r{ static_cast<T>(a % b) };
  return static_cast<T>(q - (((r != 0) && ((r < 0) != (b < 0))) ? 1 : 0));
}

template <typename T>
constexpr T floor_mod(T a, T b) {
  static_assert(std::is_integral_v<T> && std::is_signed_v<T>,
                "unsigned modulo already floors; call % directly");
  T const r{ static_cast<T>(a % b) };
  return static_cast<T>(r + (((r != 0) && ((r < 0) != (b < 0))) ? b : 0));
}

template <typename T>
constexpr T ceil_div(T a, T b) {
  static_assert(std::is_integral_v<T>, "integers only");
  T const q{ static_cast<T>(a / b) };
  T const r{ static_cast<T>(a % b) };
  if constexpr (std::is_signed_v<T>) {
    return static_cast<T>(q + (((r != 0) && ((r < 0) == (b < 0))) ? 1 : 0));
  } else {
    return static_cast<T>(q + ((r != 0) ? 1 : 0));
  }
}

// Floor square root, digit by digit from the top bit pair, so no float and no
// libm can disagree about the last bit.
constexpr uint64_t isqrt(uint64_t x) {
  uint64_t root{ 0 };
  uint64_t bit{ UINT64_C(1) << 62 };
  while (bit > x) { bit >>= 2U; }
  while (bit != 0) {
    if (x >= (root + bit)) {
      x -= root + bit;
      root = (root >> 1U) + bit;
    } else {
      root >>= 1U;
    }
    bit >>= 2U;
  }
  return root;
}

// Floor log2. x must be nonzero.
template <typename T>
constexpr uint32_t ilog2(T x) {
  static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>, "unsigned only");
  return static_cast<uint32_t>(std::bit_width(x)) - 1U;
}

// a_num/a_den < b_num/b_den by cross-multiplication: denominators positive,
// products proven inside int64 by the caller's own bounds.
constexpr bool ratio_less(int64_t a_num, int64_t a_den, int64_t b_num, int64_t b_den) {
  return (a_num * b_den) < (b_num * a_den);
}

}  // namespace scav

#endif  // SCAV_INT_H_INCLUDED
