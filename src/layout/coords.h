#ifndef SCAV_LAYOUT_COORDS_H_INCLUDED
#define SCAV_LAYOUT_COORDS_H_INCLUDED

// Brandes & Kopf cross-axis coordinate assignment (GD 2001) with the 2020 erratum's
// corrections (arXiv:2008.01252). Maps a POD layered graph to one centre per node.

#include "scav_pod_vector.h"

#include <cstdint>
#include <vector>

namespace scav {

// A proper layered graph: every edge joins consecutive layers.
struct CoordGraph {
  struct Edge {
    uint32_t from, to;                 // `from` in the earlier layer
    uint32_t inner;                    // nonzero when both ends are dummies
    int32_t from_at{ 0 }, to_at{ 0 };  // meeting point, from each end's centre
    uint32_t weak{ 0 };                // tried after the strong medians
  };

  PodVector<int32_t> extent;                // cross-axis size, indexed by node
  std::vector<PodVector<uint32_t>> layers;  // layer -> nodes, in order
  PodVector<Edge> edges;
  int32_t sep{ 0 };  // least edge-to-edge gap between neighbours in a layer
};

// One centre per node, translated so the least leading edge is zero; 0 for a node in
// no layer.
void cross_coordinates(CoordGraph const &g, PodVector<int32_t> &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_COORDS_H_INCLUDED
