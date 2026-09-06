#ifndef SCAV_LAYOUT_NUDGE_H_INCLUDED
#define SCAV_LAYOUT_NUDGE_H_INCLUDED

// A lane is a run of collinear overlapping interior segments across the nets of
// one frame; its members spread onto integer offsets, one per bundle.

#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

struct NudgeStats {
  uint32_t lanes{ 0 };
  uint32_t spread{ 0 };  // lanes of those that had the room to take an offset
  uint32_t moved{ 0 };
  uint32_t bundles{ 0 };  // members of a lane that run as one net and move as one
  uint32_t refused{ 0 };  // bundles of those a member's own checks stopped
};

// `nets` are spans into `points`, rewritten in place. `region` bounds every
// point and `bounds` is the frame's own box, which no displacement may cross.
void nudge_lanes(scav_rect const &region,
                 scav_rect const &bounds,
                 std::vector<scav_rect> const &obstacles,
                 int32_t gap,
                 int32_t clear,
                 std::vector<scav_span> const &nets,
                 std::vector<scav_point> &points,
                 NudgeStats &stats);

}  // namespace scav

#endif  // SCAV_LAYOUT_NUDGE_H_INCLUDED
