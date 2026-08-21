#ifndef SCAV_TYPES_H_INCLUDED
#define SCAV_TYPES_H_INCLUDED

// The POD spellings every scav library and the C ABI share. No functions: an
// entry point belongs to the header of the library that owns it.

// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// uint8_t need not alias an object representation, and std::byte has no
// arithmetic. NOLINTNEXTLINE(modernize-use-using)
typedef unsigned char scav_byte;

// 0 = ok; negative = error enum.
// NOLINTNEXTLINE(modernize-use-using)
typedef int32_t scav_result;

// StrRef and Span both, and the only variable-length member an ABI type holds.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct {
  uint32_t off, len;
} scav_span;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SCAV_TYPES_H_INCLUDED
