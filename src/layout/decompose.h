#ifndef SCAV_LAYOUT_DECOMPOSE_H_INCLUDED
#define SCAV_LAYOUT_DECOMPOSE_H_INCLUDED

// Splits every transition at each boundary it crosses; each segment lies in one
// submachine frame. Internal POD, outside the ABI.

#include "scav/scav_core.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

// A hierarchical port, one per (transition, crossed boundary): a state border, or the
// separator between two concurrent submachines.
struct SplitPort {
  StateId state;     // INVALID for a separator port
  SubmachineId sub;  // INVALID for a state-border port
  TransId trans;
  uint32_t crossing;  // this port's ordinal along its transition's route
  constexpr bool operator==(SplitPort const &) const = default;
};

// A route piece between two ports; an INVALID port is the transition's src or dst state.
// With `_inner` set, that state encloses `frame` and the route meets its inner face.
struct SplitSegment {
  TransId trans;
  uint32_t ordinal;    // (trans, ordinal) is the stable key
  SubmachineId frame;  // the submachine this segment routes in
  uint32_t src_port;
  uint32_t dst_port;
  uint32_t separator;  // 1 = the channel between two concurrent submachines
  uint32_t src_inner;  // 0/1; 1 only when src_port is INVALID
  uint32_t dst_inner;  // 0/1; 1 only when dst_port is INVALID
  constexpr bool operator==(SplitSegment const &) const = default;
};

// Per end, src then dst, `child` is its outermost ancestor-or-self not enclosing the other
// end (INVALID for none); a self-transition's is its state twice.
struct CommonAncestor {
  StateId state{ INVALID };       // the innermost state that is or encloses both ends
  SubmachineId frame{ INVALID };  // holds `child`; INVALID for two regions of one state
  std::array<StateId, 2> child{ StateId{ INVALID }, StateId{ INVALID } };
};

struct SplitGraph {
  std::vector<SplitPort> ports;              // route order within each transition
  std::vector<SplitSegment> segments;        // contiguous per transition
  std::vector<Span> trans_segments;          // parallel to transitions; -> segments
  std::vector<uint32_t> state_depth;         // enclosing state borders above each state
  std::vector<uint32_t> trans_label;         // parallel to transitions: `label_segment`
  std::vector<CommonAncestor> trans_common;  // parallel to transitions
  uint32_t serial{ 0 };  // a `memo_serial` naming the graph and its chart; 0 if hand-built
};

// A pure function of the model; a dead transition or one with a dead end has no segments.
SplitGraph decompose(Chart const &c);

// The state enclosing `s`; INVALID for a child of a document root and for
// INVALID itself.
StateId enclosing_state(Chart const &c, StateId s);

// True when `ancestor` is `of` or encloses it. The climb stops after one step per state.
bool ancestor_or_self(Chart const &c, StateId ancestor, StateId of);

// True for a live, non-external self-transition on a live normal state: a loop drawn in
// that state's loop room.
bool inner_loop(Chart const &c, uint32_t t);

}  // namespace scav

#endif  // SCAV_LAYOUT_DECOMPOSE_H_INCLUDED
