#ifndef SCAV_LAYOUT_ORDER_H_INCLUDED
#define SCAV_LAYOUT_ORDER_H_INCLUDED

// Phase 1: one layered graph per submachine, ranked along the layering axis
// and ordered within each rank. Internal POD; nothing here crosses the ABI.

#include "layout/decompose.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include <array>
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
  // Parallel to submachines: 1 where the ranks run down (+y) rather than
  // across (+x), from the pins (11.10g).
  std::vector<uint8_t> sub_down;
  // Parallel to submachines: 0 under the row's fold rule, else 1 + a fold pin's mode.
  std::vector<uint8_t> sub_fold;
  std::vector<uint32_t> sub_fold_cut;  // parallel to submachines: a fold pin's `layer`

  // The extra width each rank boundary must carry beyond `rank_sep`, one row
  // per boundary, so `len` is one less than the frame's rank count.
  std::vector<Span> sub_gaps;  // parallel to submachines -> gaps, labels
  std::vector<int32_t> gaps;
  // Parallel to `gaps`: the part of each a label charged, without the lanes. Sizing folds
  // by `gaps` and places by `labels` and the lanes its alignment turns.
  std::vector<int32_t> labels;

  std::vector<uint32_t> state_node;  // parallel to states -> nodes; INVALID if dead
  std::vector<uint32_t> seg_node;    // parallel to segments -> its boundary node

  // The port a boundary node stands for, so a slot can go on the crossed state's
  // border. INVALID when it is an endpoint on an inner face, not a crossing (11.14).
  std::vector<uint32_t> seg_port;

  // Parallel to segments: 1 where the boundary node sits on the leading cross border, 2 on
  // the trailing one, 0 on its rank's border. On a cross border it shares its mate's rank.
  std::vector<uint8_t> seg_cross;
  // Parallel to segments: 1 where a side pin chose the boundary node's border.
  std::vector<uint8_t> seg_sided;

  // Per segment, 1 where it lies on a cycle of its frame's graph before any turn.
  std::vector<uint8_t> seg_cyclic;
};

// Ranks by longest path, long edges chained through bends, then `sweep_count` median
// sweeps; sharded by submachine and merged in submachine order.

// `SearchPins` is `scav_layout.h`'s. **A pin is a re-derivation, not an edit**:
// ranks feed the boundary charges, the chaining of multi-rank edges, the
// buckets and the crossing sweeps, so a moved state changes all four and
// pinning re-runs them rather than patching the answer. Undoing a move is
// running with the pins one held before it. A `ChainCut` is the same shape one
// step later: it drops a segment's bends, so the buckets and the sweeps are
// re-run over a graph that no longer holds them.

SubmachineOrders order_submachines(Chart const &c,
                                   SplitGraph const &g,
                                   scav_spaces const &s,
                                   scav_profile const &p,
                                   uint32_t threads = 0,
                                   SearchPins const &pins = {});

// Crossings between two adjacent ranks by inversion counting. Exposed because
// it is what the ordering minimizes and what a test measures against.
uint64_t rank_crossings(std::vector<uint32_t> const &south_positions);

// The lowest submachine holding both ends, and per end the state it holds directly:
// INVALID for an end enclosing the other. `frame` is INVALID for two regions of one state.
struct CommonAncestor {
  SubmachineId frame{ INVALID };
  std::array<StateId, 2> child{ StateId{ INVALID }, StateId{ INVALID } };
};

CommonAncestor lowest_common_ancestor(Chart const &c, StateId src, StateId dst);

// The segment of `t` in its lowest common ancestor, which carries its label, or the middle
// one where no submachine holds both ends; INVALID for no route.
uint32_t label_segment(Chart const &c, SplitGraph const &g, uint32_t t);

}  // namespace scav

#endif  // SCAV_LAYOUT_ORDER_H_INCLUDED
