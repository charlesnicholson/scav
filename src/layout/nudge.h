#ifndef SCAV_LAYOUT_NUDGE_H_INCLUDED
#define SCAV_LAYOUT_NUDGE_H_INCLUDED

// 11.5's nudging stage. Two routes that reach the same lane draw as one polyline
// fanning out at its ends, and nothing in the router separates them: a lane is
// free for every net that wants it. This spreads the members of a lane onto
// integer offsets either side of it.

#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

struct NudgeStats {
  uint32_t lanes{ 0 };   // lanes found with more than one net in them
  uint32_t spread{ 0 };  // ... of those, the ones that had room to spread
  uint32_t moved{ 0 };   // segments actually displaced
};

// `nets` are spans into `points`, which this rewrites in place. Only a segment
// with a neighbour at each end moves: an end segment is anchored on a box border
// and sliding it along that face is a different move (11.5).
//
// A displacement is taken only when it is known good -- the moved segment and
// the two neighbours it drags may not touch an obstacle grown by `clear` that
// they did not already touch, may not leave `region`, and may not collapse a
// neighbour to nothing. A lane with no room keeps its members
// stacked, because a diagram that scores well and overlaps a box is worth less
// than one that scores badly and does not.
void nudge_lanes(scav_rect const &region,
                 std::vector<scav_rect> const &obstacles,
                 int32_t gap,
                 int32_t clear,
                 std::vector<scav_span> const &nets,
                 std::vector<scav_point> &points,
                 NudgeStats &stats);

}  // namespace scav

#endif  // SCAV_LAYOUT_NUDGE_H_INCLUDED
