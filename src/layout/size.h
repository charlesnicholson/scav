#ifndef SCAV_LAYOUT_SIZE_H_INCLUDED
#define SCAV_LAYOUT_SIZE_H_INCLUDED

// Phase 2: extents composed bottom-up by the box formula, frame-local
// positions from the ranks and the cross-axis assignment, then one descent
// that makes every position root-absolute.

#include "layout/decompose.h"
#include "layout/order.h"
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

// False on an extent that would leave the coordinate domain, with one
// diagnostic per offending entity and `out` left partly written.
bool phase2_size(Chart const &c,
                 SplitGraph const &g,
                 SubmachineOrders const &o,
                 scav_spaces const &s,
                 scav_profile const &p,
                 SizedLayout &out,
                 std::vector<Diagnostic> &diags);

}  // namespace scav

#endif  // SCAV_LAYOUT_SIZE_H_INCLUDED
