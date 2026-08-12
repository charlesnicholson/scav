#include "scav/scav_toy.h"
#include "scav_internal.h"

#include <cstdint>

namespace {

// FNV-1a, 64-bit.
constexpr uint64_t OFFSET_BASIS{ 0xcbf29ce484222325ULL };
constexpr uint64_t PRIME{ 0x100000001b3ULL };

}  // namespace

SCAV_INTERNAL_BEGIN

// One FNV-1a step. Declared before defined: under SCAV_TESTING it has external
// linkage, and -Wmissing-declarations wants the prototype.
uint64_t scav_toy_fold(uint64_t acc, scav_byte b);

uint64_t scav_toy_fold(uint64_t acc, scav_byte b) {
  return (acc ^ static_cast<uint64_t>(b)) * PRIME;
}

SCAV_INTERNAL_END

uint64_t scav_toy_checksum(scav_byte const *bytes, uint32_t len) {
  // A null pointer with a zero length is the empty sequence, not an error.
  uint64_t acc{ OFFSET_BASIS };
  for (uint32_t i = 0; i < len; ++i) { acc = scav_toy_fold(acc, bytes[i]); }
  return acc;
}
