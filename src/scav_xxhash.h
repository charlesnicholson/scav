#ifndef SCAV_XXHASH_H_INCLUDED
#define SCAV_XXHASH_H_INCLUDED

// xxHash32, bit-identical to the reference, stable across runs and toolchains.
// Internal; the libraries' public hashes and digests are built on it.

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>

namespace scav {

// Reads lanes little-endian byte by byte, so every host gives the same digest.
// `bytes` may be null when `len` is zero.
uint32_t xxhash32(scav_byte const *bytes, size_t len, uint32_t seed);

}  // namespace scav

#endif  // SCAV_XXHASH_H_INCLUDED
