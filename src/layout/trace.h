#ifndef SCAV_LAYOUT_TRACE_H_INCLUDED
#define SCAV_LAYOUT_TRACE_H_INCLUDED

// A decision trace for debugging layout itself (11.16): not a geometry column,
// not hashed, not serialized with a chart, consumed by no builder.

#include "scav/scav_core.h"

#include <cstdint>
#include <vector>

namespace scav {

enum class TraceKind : uint16_t {
  None = 0,
  RankAssigned,    // longest-path gave a state its rank
  RankPinned,      // 11.10a's placement move overrode one
  EdgeReversed,    // cycle-breaking flipped a segment
  EdgeChained,     // a multi-rank segment got a bend node at a rank
  NodePlaced,      // phase 2 fixed a node's cross coordinate
  SpacingInflated, // 11.6's retry widened the chart
  NetPlanned,      // a segment became a net with seats and waypoints
  NetWaypoint,     // one waypoint of the net last planned
  SeatMoved,       // one of 11.5's seating passes moved an attachment
  LaneAssigned,    // nudging put a run in a corridor lane
  RouteDegraded,   // a transition fell back to a straight line
  CandidateScored, // one row of 11.10's table, or one Level 1 move
};

// Which of 11.5's seating passes moved a seat; `SeatMoved.pass`.
enum class SeatPass : uint16_t { Attach, Reface, Align, Spread, Separate, Nudge };

// Why a Level 1 move was not taken; `CandidateScored.verdict`.
enum class MoveVerdict : uint16_t { Taken, NotViable, Inflated, NotBetter };

// Payloads are POD and name entities by id, never by node index -- an index is
// an artefact of how a frame was built and chaining appends bends to it. Named
// rather than anonymous because a nested anonymous type is a C++ extension.
struct TraceRank { uint32_t state, rank; };
struct TraceChain { uint32_t seg, rank, index, count; };
struct TraceSeg { uint32_t seg; };
struct TracePlace { uint32_t state, seg; int32_t x, y; };  // a bend sets seg, not state
struct TraceInflate { int32_t node_sep, rank_sep; };
struct TraceNet { uint32_t seg, trans, waypoints; int32_t sx, sy, dx, dy; };
struct TracePoint { int32_t x, y; };
struct TraceSeat { uint32_t net, end; int32_t from_x, from_y, to_x, to_y; };
struct TraceLane { uint32_t net, lane; int32_t at; };
struct TraceScore { uint32_t row, state, rank; int32_t t0; int64_t t2; };

struct TraceEvent {
  TraceKind kind{ TraceKind::None };
  uint16_t pass{ 0 };         // SeatPass / MoveVerdict, else 0
  uint32_t frame{ INVALID };  // stamped by the sink unless the site names one
  union {
    TraceRank rank;
    TraceChain chain;
    TraceSeg seg;
    TracePlace place;
    TraceInflate inflate;
    TraceNet net;
    TracePoint point;
    TraceSeat seat;
    TraceLane lane;
    TraceScore score;
  };
};

// The sink the phases append to. Null everywhere but a traced run, so an emit
// off the hot path is a load and a branch.
struct LayoutTrace {
  std::vector<TraceEvent> events;
  uint32_t frame{ INVALID };  // stamped onto every event; phases set it once
};

// Thread-local so no phase signature carries a sink it never uses. A traced run
// forces `threads = 1` (11.16), which is what keeps event order a function of
// the algorithm rather than of the scheduler.
LayoutTrace *trace_sink();
void trace_sink_set(LayoutTrace *t);

inline void trace_emit(TraceEvent e) {
  LayoutTrace *const t{ trace_sink() };
  if (t == nullptr) { return; }
  if (e.frame == INVALID) { e.frame = t->frame; }  // an explicit frame wins
  t->events.push_back(e);
}

// Scopes the frame stamp, so a phase that recurses restores its caller's.
struct TraceFrame {
  LayoutTrace *const t{ trace_sink() };
  uint32_t const was{ (t != nullptr) ? t->frame : INVALID };
  explicit TraceFrame(SubmachineId f) {
    if (t != nullptr) { t->frame = f.v; }
  }
  ~TraceFrame() {
    if (t != nullptr) { t->frame = was; }
  }
  TraceFrame(TraceFrame const &) = delete;
  TraceFrame &operator=(TraceFrame const &) = delete;
};

// One line per event, in order. The reader is `tools/trace.py`.
void trace_to_json(LayoutTrace const &t, Chart const &c, std::vector<char> &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_TRACE_H_INCLUDED
