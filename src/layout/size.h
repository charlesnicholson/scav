#ifndef SCAV_LAYOUT_SIZE_H_INCLUDED
#define SCAV_LAYOUT_SIZE_H_INCLUDED

// Phase 2: extents composed bottom-up by the box formula, frame-local
// positions from the ranks and the cross-axis assignment, then one descent
// that makes every position root-absolute.

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/pack.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

#include <cstdint>
#include <vector>

namespace scav {

// Everything the geometry columns need except the routes, all root-absolute.
// Tombstones stay all-zero.
struct SizedLayout {
  std::vector<scav_rect> state, before, after;  // parallel to states
  std::vector<scav_rect> sub;                   // parallel to submachines
  std::vector<scav_point> node;                 // parallel to the orders' nodes
  scav_rect chart{};
};

// A desired aspect ratio as a pair, in the profile's own `[1, 1024]` bounds so
// the packer's products stay where it proved them. `num` 0 is no ratio at all.
struct FrameDar {
  int32_t num{ 0 }, den{ 0 };
};

// Which ratio every packing inside a state's interior aims at: the profile's
// one ratio at every depth, or the aspect of the hole the state leaves between
// its two text bands, which is the rect its submachines are packed into
// (11.4, 11.10). The hole is only knowable once the state is sized, so
// `OwnerHole` sizes twice -- once at the profile's ratio to find the holes,
// then again against them.
enum class DarSource : uint32_t { Profile, OwnerHole };

// False on an extent that would leave the coordinate domain, with one
// diagnostic per offending entity and `out` left partly written. `dar` and
// `compaction` default to the row-0 tuple, which is the pipeline as it ran
// before the portfolio existed.
bool size_layout(Chart const &c,
                 SplitGraph const &g,
                 SubmachineOrders const &o,
                 scav_spaces const &s,
                 scav_profile const &p,
                 SizedLayout &out,
                 std::vector<Diagnostic> &diags,
                 DarSource dar = DarSource::Profile,
                 Compaction compaction = Compaction::Off);

}  // namespace scav

#endif  // SCAV_LAYOUT_SIZE_H_INCLUDED
