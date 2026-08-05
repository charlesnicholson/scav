#ifndef SCAV_TYPES_H_INCLUDED
#define SCAV_TYPES_H_INCLUDED

// The POD spellings every scav library and the C ABI share. No functions: an
// entry point belongs to the header of the library that owns it.

// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Only char, unsigned char and std::byte may alias an object representation;
// uint8_t need not be one of them, so byte inspection through it is UB where it
// is not. Not std::byte either: no arithmetic operators, and the C ABI needs
// unsigned char.
// NOLINTNEXTLINE(modernize-use-using)
typedef unsigned char scav_byte;

// 0 = ok; negative = error enum.
// NOLINTNEXTLINE(modernize-use-using)
typedef int32_t scav_result;

// StrRef and Span both: an offset and a length into a separately-exposed flat
// array. The only variable-length member any ABI type is allowed.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct {
  uint32_t off, len;
} scav_span;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SCAV_TYPES_H_INCLUDED
