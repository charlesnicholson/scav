#ifndef SCAV_LAYOUT_GEOM_H_INCLUDED
#define SCAV_LAYOUT_GEOM_H_INCLUDED

// The rect and point predicates the phases share. Both containment tests are
// strict: rects touching do not overlap, and a point on a border is not inside.

#include "scav/scav_types.h"
#include "scav_int.h"

namespace scav {

constexpr bool same(scav_point a, scav_point b) { return (a.x == b.x) && (a.y == b.y); }

constexpr bool overlaps(scav_rect const &a, scav_rect const &b) {
  return (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) && (a.y < (b.y + b.h)) &&
         (b.y < (a.y + a.h));
}

constexpr bool inside(scav_point p, scav_rect const &r) {
  return (p.x > r.x) && (p.x < (r.x + r.w)) && (p.y > r.y) && (p.y < (r.y + r.h));
}

// A segment's bounding box: zero thickness when axis-aligned, so `overlaps`
// reads a run along a border as touching rather than overlapping.
constexpr scav_rect span_rect(scav_point a, scav_point b) {
  int32_t const x{ (a.x < b.x) ? a.x : b.x };
  int32_t const y{ (a.y < b.y) ? a.y : b.y };
  return { .x = x,
           .y = y,
           .w = ((a.x < b.x) ? b.x : a.x) - x,
           .h = ((a.y < b.y) ? b.y : a.y) - y };
}

// The Chebyshev gap between two rects: the larger of the two axes'
// separations, and zero on the axis they overlap or touch on.
constexpr int32_t chebyshev_gap(scav_rect const &a, scav_rect const &b) {
  int32_t const dx{ imax(imax(b.x - (a.x + a.w), a.x - (b.x + b.w)), 0) };
  int32_t const dy{ imax(imax(b.y - (a.y + a.h), a.y - (b.y + b.h)), 0) };
  return imax(dx, dy);
}

}  // namespace scav

#endif  // SCAV_LAYOUT_GEOM_H_INCLUDED
