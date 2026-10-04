#ifndef SCAV_LAYOUT_NUDGE_H_INCLUDED
#define SCAV_LAYOUT_NUDGE_H_INCLUDED

// A lane is a connected set of parallel interior segments, linked when under `gap` apart
// and overlapping along the axis; each of its bundles takes one integer offset.

#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

struct NudgeStats {
  uint32_t lanes{ 0 };
  uint32_t spread{ 0 };  // lanes with room to give some member a nonzero offset
  uint32_t moved{ 0 };
  uint32_t bundles{ 0 };    // bundles of two or more members, which move as one
  uint32_t refused{ 0 };    // of `bundles`, those a member's check left in place
  uint32_t reordered{ 0 };  // lanes whose vote order differs from the key order
};

// `nets` span `points`, rewritten in place; net `n` stays inside `region` and `bounds[n]`.
// Net `n < n_keep` keeps its first leg over `keep[n].src` long and its last over `.dst`.
void nudge_lanes(scav_rect const &region,
                 std::vector<scav_rect> const &bounds,
                 std::vector<scav_rect> const &obstacles,
                 int32_t gap,
                 int32_t clear,
                 std::vector<scav_span> const &nets,
                 std::vector<scav_point> &points,
                 NudgeStats &stats,
                 scav_path_clear const *keep = nullptr,
                 uint32_t n_keep = 0);

}  // namespace scav

#endif  // SCAV_LAYOUT_NUDGE_H_INCLUDED
