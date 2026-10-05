#ifndef SCAV_RND_H_INCLUDED
#define SCAV_RND_H_INCLUDED

// Position-addressed randomness: each value is a pure function of its coordinate.

#include <cstdint>

namespace scav {

// The splitmix64 finalizer with the published constants; multiplies wrap mod 2^64.
constexpr uint64_t splitmix64(uint64_t z) {
  z ^= z >> 30U;
  z *= UINT64_C(0xBF58'476D'1CE4'E5B9);
  z ^= z >> 27U;
  z *= UINT64_C(0x94D0'49BB'1331'11EB);
  z ^= z >> 31U;
  return z;
}

// Three splitmix64 rounds over the coordinate. With any three arguments fixed, the
// fourth maps one to one onto the output.
constexpr uint64_t rnd(uint64_t seed, uint32_t phase, uint32_t item, uint32_t step) {
  uint64_t const seeded{ splitmix64(seed) };
  uint64_t const placed{ splitmix64(
      seeded + ((static_cast<uint64_t>(phase) << 32U) | static_cast<uint64_t>(item))) };
  return splitmix64(placed + static_cast<uint64_t>(step));
}

}  // namespace scav

#endif  // SCAV_RND_H_INCLUDED
