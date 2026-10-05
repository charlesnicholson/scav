#ifndef SCAV_LAYOUT_NUDGE_H_INCLUDED
#define SCAV_LAYOUT_NUDGE_H_INCLUDED

// A lane is a connected set of parallel interior segments, linked when under `gap` apart
// and overlapping along the axis; each of its bundles takes one integer offset.

#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

// `nets` span `points`, rewritten in place; net `n` stays inside `region` and `bounds[n]`.
// Net `n < n_keep` keeps its first leg over `keep[n].src` long and its last over `.dst`.
// A moved leg keeps `clear` off every obstacle and `band` inside `bounds[n]` and inside
// every obstacle it passes through, and runs within `band` of no border it was not near.
void nudge_lanes(scav_rect const &region,
                 std::vector<scav_rect> const &bounds,
                 std::vector<scav_rect> const &obstacles,
                 int32_t gap,
                 int32_t clear,
                 int32_t band,
                 std::vector<scav_span> const &nets,
                 std::vector<scav_point> &points,
                 scav_path_clear const *keep = nullptr,
                 uint32_t n_keep = 0);

}  // namespace scav

#endif  // SCAV_LAYOUT_NUDGE_H_INCLUDED
