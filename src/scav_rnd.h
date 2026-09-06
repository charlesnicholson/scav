#ifndef SCAV_RND_H_INCLUDED
#define SCAV_RND_H_INCLUDED

// Position-addressed randomness: the value at a coordinate rather than the next
// value of a stream, so nothing about it depends on who asks or in what order.

#include <cstdint>

namespace scav {

// The splitmix64 finalizer, constants as published. Unsigned multiplication
// wraps by definition, which is the mixing step.
constexpr uint64_t splitmix64(uint64_t z) {
  z ^= z >> 30U;
  z *= UINT64_C(0xBF58'476D'1CE4'E5B9);
  z ^= z >> 27U;
  z *= UINT64_C(0x94D0'49BB'1331'11EB);
  z ^= z >> 31U;
  return z;
}

// Each round is a bijection and each fold adds one argument's bits, so holding
// three arguments fixed leaves the fourth mapped one to one onto the output.
constexpr uint64_t rnd(uint64_t seed, uint32_t phase, uint32_t item, uint32_t step) {
  uint64_t const seeded{ splitmix64(seed) };
  uint64_t const placed{ splitmix64(
      seeded + ((static_cast<uint64_t>(phase) << 32U) | static_cast<uint64_t>(item))) };
  return splitmix64(placed + static_cast<uint64_t>(step));
}

}  // namespace scav

#endif  // SCAV_RND_H_INCLUDED
