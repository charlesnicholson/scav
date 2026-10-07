#ifndef SCAV_LAYOUT_LABEL_H_INCLUDED
#define SCAV_LAYOUT_LABEL_H_INCLUDED

// Places path boxes on finished routes, each joined to its route by a fixed-length leader.
// The feasible candidate least crowded by foreign legs, then nearest the anchor, wins.

#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"
#include "scav_vector.h"

#include <cstdint>

namespace scav {

// How a box's candidates are searched; all return the same boxes. `Pruned` skips those
// that cannot win; `Memoized` is `Pruned` behind a per-thread table of placed boxes.
enum class LabelSearch : uint32_t {
  Pruned,
  Memoized,
#ifdef SCAV_TESTING
  Exhaustive,  // keys and tests every candidate; the reference for the other two
#endif
};

// Fills `out` parallel to `s.path_box`; returns how many boxes have no feasible
// candidate and sit centred on their anchor.
uint32_t place_labels(Chart const &c,
                      SplitGraph const &g,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      Vector<scav_span> const &route,
                      Vector<scav_point> const &points,
                      scav_profile const &p,
                      Vector<scav_rect> &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_LABEL_H_INCLUDED
