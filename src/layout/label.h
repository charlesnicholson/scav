#ifndef SCAV_LAYOUT_LABEL_H_INCLUDED
#define SCAV_LAYOUT_LABEL_H_INCLUDED

// Path boxes onto the finished routes: a fixed-length leader from a point on
// the label's own polyline to one of the eight points of its rectangle, the
// anchor slid along the route, the feasible candidate least at risk of reading
// as somebody else's (11.9.4).

#include "layout/geom.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

// Fills `out` parallel to `s.path_box`; returns the boxes that found no
// feasible candidate and took the centred placement instead.
uint32_t place_labels(Chart const &c,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      std::vector<scav_span> const &route,
                      std::vector<scav_point> const &points,
                      scav_profile const &p,
                      std::vector<scav_rect> &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_LABEL_H_INCLUDED
