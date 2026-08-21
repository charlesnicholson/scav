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

// The geometry vocabulary: integer grid units, root-absolute, rects half-open.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct {
  int32_t x, y;
} scav_point;

// NOLINTNEXTLINE(modernize-use-using)
typedef struct {
  int32_t w, h;
} scav_extent;

// NOLINTNEXTLINE(modernize-use-using)
typedef struct {
  int32_t x, y, w, h;
} scav_rect;

#ifdef __cplusplus
}  // extern "C"

namespace scav {

// Coordinates are int32 grid units of 1/16 point; intermediates widen to Wide
// before multiplying.
using Coord = int32_t;
using Wide = int64_t;

// Symmetric domain, so negation, abs, and the RTL x-mirror stay in range on
// every value.
inline constexpr int32_t COORD_MAX{ (INT32_C(1) << 19) - 1 };
inline constexpr int32_t COORD_MIN{ -COORD_MAX };

}  // namespace scav
#endif

#endif  // SCAV_TYPES_H_INCLUDED
