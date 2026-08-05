#ifndef SCAV_TOY_H_INCLUDED
#define SCAV_TOY_H_INCLUDED

// Bootstrap scaffolding, deliberately nothing more: one library with one function,
// so the harness is proven before any scav code is measured against it.

#include "scav/types.h"

// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// FNV-1a, deliberately *not* scav's hash: a real one would invite treating this
// golden as a determinism artifact.

// uint32_t, not size_t: size_t is 32-bit on wasm32 and unsigned wrap is defined,
// so the same expression yields a different correct answer per platform.
uint64_t scav_toy_checksum(scav_byte const *bytes, uint32_t len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SCAV_TOY_H_INCLUDED
