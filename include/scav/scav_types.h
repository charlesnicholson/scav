#ifndef SCAV_TYPES_H_INCLUDED
#define SCAV_TYPES_H_INCLUDED

// POD types shared by every scav library and the C ABI. Entry points live in
// the owning library's header.

// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A byte that may alias any object and supports arithmetic.
// NOLINTNEXTLINE(modernize-use-using)
typedef unsigned char scav_byte;

// 0 = ok; negative = error enum.
// NOLINTNEXTLINE(modernize-use-using)
typedef int32_t scav_result;

// The C form of StrRef and Span; the only variable-length member in an ABI type.
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

using Coord = int32_t;  // grid units of 1/16 point
using Wide = int64_t;   // widen to Wide before multiplying coordinates

// Symmetric domain, so negation, abs, and the RTL x-mirror stay in range on
// every value.
inline constexpr int32_t COORD_MAX{ (INT32_C(1) << 19) - 1 };
inline constexpr int32_t COORD_MIN{ -COORD_MAX };

}  // namespace scav
#endif

#endif  // SCAV_TYPES_H_INCLUDED
