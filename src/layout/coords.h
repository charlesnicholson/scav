#ifndef SCAV_LAYOUT_COORDS_H_INCLUDED
#define SCAV_LAYOUT_COORDS_H_INCLUDED

// Brandes & Kopf cross-axis coordinate assignment (GD 2001) with the
// corrections from the 2020 erratum, arXiv:2008.01252. Internal POD in, one
// coordinate per node out; no chart, no profile, no font.

#include <cstdint>
#include <vector>

namespace scav {

// A proper layered graph: every edge joins consecutive layers. `extent` is
// what each node occupies along the cross axis, so the result is centres and
// two adjacent nodes end up at least `sep` apart edge to edge.
struct CoordGraph {
  // One segment between consecutive layers. `inner` marks a segment whose
  // both ends are dummies, which is the one a type-1 conflict protects.
  struct Edge {
    uint32_t from, to;
    uint32_t inner;
  };

  std::vector<int32_t> extent;                // indexed by node
  std::vector<std::vector<uint32_t>> layers;  // layer -> nodes, in order
  std::vector<Edge> edges;
  int32_t sep{ 0 };
};

// One centre per node, translated so the topmost node's leading edge is zero.
// A node in no layer gets zero.
std::vector<int32_t> cross_coordinates(CoordGraph const &g);

}  // namespace scav

#endif  // SCAV_LAYOUT_COORDS_H_INCLUDED
