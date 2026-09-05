#ifndef SCAV_LAYOUT_GEOM_H_INCLUDED
#define SCAV_LAYOUT_GEOM_H_INCLUDED

// The rect and point predicates the phases share. Both containment tests are
// strict: rects touching do not overlap, and a point on a border is not inside.

#include "scav/scav_types.h"

namespace scav {

constexpr bool same(scav_point a, scav_point b) { return (a.x == b.x) && (a.y == b.y); }

constexpr bool overlaps(scav_rect const &a, scav_rect const &b) {
  return (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) && (a.y < (b.y + b.h)) &&
         (b.y < (a.y + a.h));
}

constexpr bool inside(scav_point p, scav_rect const &r) {
  return (p.x > r.x) && (p.x < (r.x + r.w)) && (p.y > r.y) && (p.y < (r.y + r.h));
}

}  // namespace scav

#endif  // SCAV_LAYOUT_GEOM_H_INCLUDED
