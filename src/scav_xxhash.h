#ifndef SCAV_XXHASH_H_INCLUDED
#define SCAV_XXHASH_H_INCLUDED

// xxHash32, the reference algorithm byte for byte, stable across runs and
// toolchains. A private primitive; chart_structural_hash is the API.

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>

namespace scav {

// Lane reads are assembled from bytes rather than cast, so a big-endian host
// produces the same digest. `bytes` may be null when `len` is zero.
uint32_t xxhash32(scav_byte const *bytes, size_t len, uint32_t seed);

}  // namespace scav

#endif  // SCAV_XXHASH_H_INCLUDED
