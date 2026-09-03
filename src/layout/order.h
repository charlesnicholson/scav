#ifndef SCAV_LAYOUT_ORDER_H_INCLUDED
#define SCAV_LAYOUT_ORDER_H_INCLUDED

// Phase 1: one layered graph per submachine, ranked along the layering axis
// and ordered within each rank. Internal POD; nothing here crosses the ABI.

#include "layout/decompose.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

#include <cstdint>
#include <vector>

namespace scav {

// What a node of one frame's layered graph stands for. `Boundary` is a
// hierarchical port sitting on the frame's own enclosing border, which is
// where a route leaves or arrives from outside; `Bend` is a dummy the chain of
// a multi-rank edge passes through.
enum class OrderKind : uint32_t { State, Boundary, Bend };

// `subject` is a StateId ordinal for `State` and an index into
// `SplitGraph::segments` for the other two: a boundary node is one-to-one with
// the segment that reaches it, since consecutive crossings always change frame.
struct OrderNode {
  OrderKind kind;
  uint32_t subject;
  uint32_t rank;  // layer index within the frame, 0-based, increasing in +x
  uint32_t pos;   // position within the rank, 0-based, increasing in +y
  constexpr bool operator==(OrderNode const &) const = default;
};

// Always between adjacent ranks: a longer span was chained through `Bend`
// nodes before this edge was emitted. `reversed` marks an edge whose authored
// direction is dst to src, flipped to make the frame acyclic.
struct OrderEdge {
  uint32_t src, dst;  // -> nodes
  uint32_t segment;   // -> SplitGraph::segments
  uint32_t reversed;
  constexpr bool operator==(OrderEdge const &) const = default;
};

struct SubmachineOrders {
  std::vector<OrderNode> nodes;     // contiguous per submachine
  std::vector<OrderEdge> edges;     // contiguous per submachine
  std::vector<Span> sub_nodes;      // parallel to submachines -> nodes
  std::vector<Span> sub_edges;      // parallel to submachines -> edges
  std::vector<uint32_t> sub_ranks;  // parallel to submachines; layer count

  // The extra width each rank boundary must carry beyond `rank_sep`, one row
  // per boundary, so `len` is one less than the frame's rank count.
  std::vector<Span> sub_gaps;  // parallel to submachines -> gaps
  std::vector<int32_t> gaps;

  std::vector<uint32_t> state_node;  // parallel to states -> nodes; INVALID if dead
  std::vector<uint32_t> seg_node;    // parallel to segments -> its boundary node
};

// Pure in `(Chart, SplitGraph, Spaces, Profile)`: ranks by longest path,
// multi-rank edges chained through bends, then `sweep_count` median sweeps
// keeping the ordering with the fewest crossings. Reads no extent except a
// path box's own, so it runs before anything is sized.
SubmachineOrders phase1_order(Chart const &c,
                              SplitGraph const &g,
                              scav_spaces const &s,
                              scav_profile const &p);

// The crossings between two adjacent ranks, by inversion counting over the
// edges' endpoint positions. Exposed because it is what the ordering
// minimizes and what a test measures it against.
uint64_t rank_crossings(std::vector<uint32_t> const &south_positions);

}  // namespace scav

#endif  // SCAV_LAYOUT_ORDER_H_INCLUDED
