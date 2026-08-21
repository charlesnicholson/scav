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

// One row of scav_layout_run's out-param, parallel to the path boxes; w and h
// may exceed what was requested. NOLINTNEXTLINE(modernize-use-using)
typedef scav_rect scav_placed;

#ifdef __cplusplus
}  // extern "C"

// The C++ spellings. One layout under two names: the model and layout say
// Point and Rect, the ABI says scav_point and scav_rect.
namespace scav {

using Point = ::scav_point;
using Extent = ::scav_extent;
using Rect = ::scav_rect;

// Coordinates are int32 grid units of 1/16 point; intermediates widen to Wide
// before multiplying.
using Coord = int32_t;
using Wide = int64_t;

// Symmetric domain, so negation, abs, and the RTL x-mirror stay in range on
// every value.
inline constexpr int32_t COORD_MAX{ (INT32_C(1) << 19) - 1 };
inline constexpr int32_t COORD_MIN{ -COORD_MAX };

}  // namespace scav

// Pinned: the binding generator reproduces these layouts from the JSON, so a
// platform where they drift must fail to compile rather than misread.
static_assert(sizeof(scav_span) == 8);
static_assert(sizeof(scav_point) == 8);
static_assert(sizeof(scav_extent) == 8);
static_assert(sizeof(scav_rect) == 16);
#endif

#endif  // SCAV_TYPES_H_INCLUDED
