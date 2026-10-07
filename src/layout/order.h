#ifndef SCAV_LAYOUT_ORDER_H_INCLUDED
#define SCAV_LAYOUT_ORDER_H_INCLUDED

// Phase 1: one layered graph per submachine, ranked along the layering axis and
// ordered within each rank. Internal POD, outside the ABI.

#include "layout/decompose.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_vector.h"

#include <cstdint>

namespace scav {

// `Boundary` is a hierarchical port on the frame's enclosing border; `Bend` is a
// dummy the chain of a multi-rank edge passes through.
enum class OrderKind : uint32_t { State, Boundary, Bend };

struct OrderNode {
  OrderKind kind;
  uint32_t subject;  // a StateId for `State`, else -> SplitGraph::segments
  uint32_t rank;     // layer index within the frame, 0-based, increasing in +x
  uint32_t pos;      // position within the rank, 0-based, increasing in +y
  constexpr bool operator==(OrderNode const &) const = default;
};

// A forward multi-rank edge is chained through `Bend` nodes into one-rank edges unless its
// segment is cut.
struct OrderEdge {
  uint32_t src, dst;  // -> nodes
  uint32_t segment;   // -> SplitGraph::segments
  uint32_t reversed;  // 1 = runs dst-to-src, against its authoring
  constexpr bool operator==(OrderEdge const &) const = default;
};

struct SubmachineOrders {
  Vector<OrderNode> nodes;     // contiguous per submachine
  Vector<OrderEdge> edges;     // contiguous per submachine
  Vector<Span> sub_nodes;      // parallel to submachines -> nodes
  Vector<Span> sub_edges;      // parallel to submachines -> edges
  Vector<uint32_t> sub_ranks;  // parallel to submachines; layer count
  // Parallel to submachines: 1 where an orient pin runs the ranks down (+y), else 0 (+x).
  Vector<uint8_t> sub_down;
  // Parallel to submachines: 0 under the row's fold rule, else 1 + a fold pin's mode.
  Vector<uint8_t> sub_fold;
  Vector<uint32_t> sub_fold_cut;  // parallel to submachines: a fold pin's `layer`
  // Parallel to states: 0 unpinned, else 1 + a loop pin's `face * 2 + end`.
  Vector<uint8_t> state_loop;

  // The extra width each rank boundary carries beyond `rank_sep`, one row per boundary;
  // `len` is the frame's rank count less one.
  Vector<Span> sub_gaps;  // parallel to submachines -> gaps, labels
  Vector<int32_t> gaps;
  // Parallel to `gaps`: the part of each gap a label charged, without the lanes.
  Vector<int32_t> labels;

  Vector<uint32_t> state_node;  // parallel to states -> nodes; INVALID if dead
  Vector<uint32_t> seg_node;    // parallel to segments -> its boundary node

  // Parallel to segments: the port its boundary node stands for; INVALID for an inner-face
  // endpoint or no boundary node.
  Vector<uint32_t> seg_port;

  // Parallel to segments: 1 where the boundary node sits on the leading cross border, 2 on
  // the trailing one, 0 on its rank's border. On a cross border it shares its mate's rank.
  Vector<uint8_t> seg_cross;
  // Parallel to segments: 1 where an end pin chose the boundary node's border.
  Vector<uint8_t> seg_sided;
  // Parallel to segments: the boundary node's face 0..3 as `scav_port_slot::side`; off a
  // cross border, the leading rank border if an edge leaves the node, else the trailing.
  Vector<uint8_t> seg_side;

  // Parallel to segments: 1 where it lies on a cycle of its frame's graph before any turn.
  Vector<uint8_t> seg_cyclic;
};

// Ranks each frame by longest path under `pins`, chains long edges through bends, then
// runs `sweep_count` median sweeps; sharded by submachine, merged in submachine order.
SubmachineOrders order_submachines(Chart const &c,
                                   SplitGraph const &g,
                                   scav_spaces const &s,
                                   scav_profile const &p,
                                   uint32_t threads = 0,
                                   SearchPins const &pins = {});

// The same into `o`, reusing its capacity.
void order_submachines(SubmachineOrders &o,
                       Chart const &c,
                       SplitGraph const &g,
                       scav_spaces const &s,
                       scav_profile const &p,
                       uint32_t threads,
                       SearchPins const &pins);

// Crossings between two adjacent ranks: inversions of the south positions in north order.
uint64_t rank_crossings(Vector<uint32_t> const &south_positions);

// The segment carrying `t`'s label: its first in the common frame, else the middle one;
// INVALID with no segments or for an inner loop.
uint32_t label_segment(Chart const &c, SplitGraph const &g, uint32_t t);

}  // namespace scav

#endif  // SCAV_LAYOUT_ORDER_H_INCLUDED
