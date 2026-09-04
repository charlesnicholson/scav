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

// `Boundary` is a hierarchical port on the frame's enclosing border; `Bend` is a
// dummy the chain of a multi-rank edge passes through.
enum class OrderKind : uint32_t { State, Boundary, Bend };

// `subject` is a StateId for `State`, else an index into `SplitGraph::segments`:
// consecutive crossings change frame, so a boundary node has exactly one.
struct OrderNode {
  OrderKind kind;
  uint32_t subject;
  uint32_t rank;  // layer index within the frame, 0-based, increasing in +x
  uint32_t pos;   // position within the rank, 0-based, increasing in +y
  constexpr bool operator==(OrderNode const &) const = default;
};

// Always between adjacent ranks; longer spans were chained through `Bend` first.
// `reversed` marks an edge flipped dst-to-src to make the frame acyclic.
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

  // The port a boundary node stands for, so a slot can go on the crossed state's
  // border. INVALID when it is an endpoint on an inner face, not a crossing (11.14).
  std::vector<uint32_t> seg_port;
};

// Ranks by longest path, multi-rank edges chained through bends, then
// `sweep_count` median sweeps keeping the fewest crossings. Reads no extent but
// a path box's, so it runs before anything is sized.
SubmachineOrders phase1_order(Chart const &c,
                              SplitGraph const &g,
                              scav_spaces const &s,
                              scav_profile const &p);

// Crossings between two adjacent ranks by inversion counting. Exposed because
// it is what the ordering minimizes and what a test measures against.
uint64_t rank_crossings(std::vector<uint32_t> const &south_positions);

}  // namespace scav

#endif  // SCAV_LAYOUT_ORDER_H_INCLUDED
