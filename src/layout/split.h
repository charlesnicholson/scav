#ifndef SCAV_LAYOUT_SPLIT_H_INCLUDED
#define SCAV_LAYOUT_SPLIT_H_INCLUDED

// Phase 0: split every transition at each boundary it crosses, so each segment
// is local to one submachine frame. Internal POD; nothing here crosses the ABI.

#include "scav/scav_core.h"

#include <cstdint>
#include <vector>

namespace scav {

// A hierarchical port, one per (transition, crossed boundary). A state border
// names the state; a concurrent separator names the submachine instead.
struct SplitPort {
  StateId state;      // INVALID for a separator port
  SubmachineId sub;   // INVALID for a state-border port
  TransId trans;
  uint32_t crossing;  // this port's ordinal along its transition's route
  constexpr bool operator==(SplitPort const &) const = default;
};

// One route piece between two ports, or between an endpoint state and a port.
// INVALID port means the transition's own src (first) or dst (last) state.
struct SplitSegment {
  TransId trans;
  uint32_t ordinal;    // (trans, ordinal) is the stable key
  SubmachineId frame;  // the submachine this segment routes in
  uint32_t src_port;
  uint32_t dst_port;
  uint32_t separator;  // 1 = the channel between two concurrent submachines
  constexpr bool operator==(SplitSegment const &) const = default;
};

struct SplitGraph {
  std::vector<SplitPort> ports;        // route order within each transition
  std::vector<SplitSegment> segments;  // contiguous per transition
  std::vector<Span> trans_segments;    // parallel to transitions; -> segments
  std::vector<uint32_t> trans_crossings;  // boundaries crossed, kind-adjusted
  std::vector<uint32_t> state_crossings;  // edges through each state's border
  std::vector<uint32_t> state_depth;      // enclosing state borders above each state
};

// A pure function of the model: no-route transitions (internal or local
// self-transitions) get an empty span, tombstones are skipped.
SplitGraph phase0_split(Chart const &c);

}  // namespace scav

#endif  // SCAV_LAYOUT_SPLIT_H_INCLUDED
