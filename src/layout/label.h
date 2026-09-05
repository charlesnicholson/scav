#ifndef SCAV_LAYOUT_LABEL_H_INCLUDED
#define SCAV_LAYOUT_LABEL_H_INCLUDED

// Path boxes onto the finished routes: one strip either side of each leg,
// slid along it, the feasible candidate nearest where centring would have put it.

#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"
#include "scav_int.h"

#include <cstdint>
#include <vector>

namespace scav {

// A segment's bounding box, so `overlaps` against it is the strict interior
// test. Zero thickness on an axis-aligned segment; a diagonal keeps its box.
constexpr scav_rect span_rect(scav_point a, scav_point b) {
  return { .x = imin(a.x, b.x),
           .y = imin(a.y, b.y),
           .w = imax(a.x, b.x) - imin(a.x, b.x),
           .h = imax(a.y, b.y) - imin(a.y, b.y) };
}

// Fills `out` parallel to `s.path_box`; returns the boxes that found no
// feasible candidate and took the centred placement instead.
uint32_t place_labels(Chart const &c,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      std::vector<scav_span> const &route,
                      std::vector<scav_point> const &points,
                      std::vector<scav_rect> &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_LABEL_H_INCLUDED
