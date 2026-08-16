// Rotation goes through `<bit>`: `__builtin_rotl` is not standard C++ and a
// hand-rolled shift pair is UB at a rotation of zero.

#include "scav_xxhash.h"

#include "scav/scav_types.h"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace scav {

namespace {

constexpr uint32_t PRIME1{ 0x9E37'79B1U };
constexpr uint32_t PRIME2{ 0x85EB'CA77U };
constexpr uint32_t PRIME3{ 0xC2B2'AE3DU };
constexpr uint32_t PRIME4{ 0x27D4'EB2FU };
constexpr uint32_t PRIME5{ 0x1656'67B1U };

uint32_t lane(scav_byte const *at) {
  return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8U) |
         (static_cast<uint32_t>(at[2]) << 16U) | (static_cast<uint32_t>(at[3]) << 24U);
}

uint32_t round32(uint32_t acc, uint32_t input) {
  return std::rotl(acc + (input * PRIME2), 13) * PRIME1;
}

}  // namespace

uint32_t xxhash32(scav_byte const *bytes, size_t len, uint32_t seed) {
  scav_byte const *at{ bytes };
  scav_byte const *const end{ bytes + len };
  uint32_t h{ 0 };

  if (len >= 16) {
    uint32_t a1{ seed + PRIME1 + PRIME2 };
    uint32_t a2{ seed + PRIME2 };
    uint32_t a3{ seed };
    uint32_t a4{ seed - PRIME1 };
    for (; (end - at) >= 16; at += 16) {
      a1 = round32(a1, lane(at));
      a2 = round32(a2, lane(at + 4));
      a3 = round32(a3, lane(at + 8));
      a4 = round32(a4, lane(at + 12));
    }
    h = std::rotl(a1, 1) + std::rotl(a2, 7) + std::rotl(a3, 12) + std::rotl(a4, 18);
  } else {
    h = seed + PRIME5;
  }

  // Mixing the length in is what separates two inputs that differ only in
  // trailing structure. Truncating past 4 GiB is the reference behaviour.
  h += static_cast<uint32_t>(len & 0xFFFF'FFFFU);

  for (; (end - at) >= 4; at += 4) { h = std::rotl(h + (lane(at) * PRIME3), 17) * PRIME4; }
  for (; at != end; ++at) {
    h = std::rotl(h + (static_cast<uint32_t>(*at) * PRIME5), 11) * PRIME1;
  }

  h ^= h >> 15U;
  h *= PRIME2;
  h ^= h >> 13U;
  h *= PRIME3;
  h ^= h >> 16U;
  return h;
}

}  // namespace scav
