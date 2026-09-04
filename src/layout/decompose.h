#ifndef SCAV_LAYOUT_DECOMPOSE_H_INCLUDED
#define SCAV_LAYOUT_DECOMPOSE_H_INCLUDED

// Splits every transition at each boundary it crosses, so each segment is
// local to one submachine frame. Internal POD; nothing here crosses the ABI.

#include "scav/scav_core.h"

#include <cstdint>
#include <vector>

namespace scav {

// A hierarchical port, one per (transition, crossed boundary). A state border
// names the state; a concurrent separator names the submachine instead.
struct SplitPort {
  StateId state;     // INVALID for a separator port
  SubmachineId sub;  // INVALID for a state-border port
  TransId trans;
  uint32_t crossing;  // this port's ordinal along its transition's route
  constexpr bool operator==(SplitPort const &) const = default;
};

// One route piece between two ports, or between an endpoint state and a port.
// INVALID port means the transition's own src (first) or dst (last) state.
// An `_inner` flag on such an end says that endpoint state encloses `frame`,
// so the route meets the state's inner face rather than its box centre; the
// border is not crossed there, so the end carries no port.
struct SplitSegment {
  TransId trans;
  uint32_t ordinal;    // (trans, ordinal) is the stable key
  SubmachineId frame;  // the submachine this segment routes in
  uint32_t src_port;
  uint32_t dst_port;
  uint32_t separator;  // 1 = the channel between two concurrent submachines
  uint32_t src_inner;  // 0/1, only ever set when src_port is INVALID
  uint32_t dst_inner;  // 0/1, only ever set when dst_port is INVALID
  constexpr bool operator==(SplitSegment const &) const = default;
};

struct SplitGraph {
  std::vector<SplitPort> ports;           // route order within each transition
  std::vector<SplitSegment> segments;     // contiguous per transition
  std::vector<Span> trans_segments;       // parallel to transitions; -> segments
  std::vector<uint32_t> state_crossings;  // edges through each state's border
  std::vector<uint32_t> state_depth;      // enclosing state borders above each state
};

// A pure function of the model: no-route transitions (internal or local
// self-transitions) get an empty span, tombstones are skipped.
SplitGraph decompose(Chart const &c);

// The state enclosing `s`; INVALID for a child of a document root and for
// INVALID itself.
StateId enclosing_state(Chart const &c, StateId s);

// `ancestor` is `of` itself or encloses it. The climb stops after one step per
// state, so containment that has been corrupted into a cycle answers false
// rather than spinning.
bool ancestor_or_self(Chart const &c, StateId ancestor, StateId of);

}  // namespace scav

#endif  // SCAV_LAYOUT_DECOMPOSE_H_INCLUDED
