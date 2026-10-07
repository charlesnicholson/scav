#ifndef SCAV_LAYOUT_TRACE_H_INCLUDED
#define SCAV_LAYOUT_TRACE_H_INCLUDED

// A decision trace for debugging layout, kept out of the geometry columns, the layout
// hash and chart serialization.

#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

enum class TraceKind : uint16_t {
  None = 0,
  RankAssigned,       // ranking gave a state its rank
  RankPinned,         // a rank pin overrode a state's rank
  EdgeReversed,       // cycle-breaking or a reversal pin flipped a segment's edge
  EdgeChained,        // a multi-rank segment got a bend node at a rank
  NodePlaced,         // phase 2 placed a node, root-absolute
  SpacingInflated,    // a spacing-inflation retry widened the chart and was kept
  NetPlanned,         // a segment became a net with seats and waypoints
  NetWaypoint,        // one waypoint of the net last planned
  SeatMoved,          // a seating pass moved an attachment
  LaneAssigned,       // nudging put a run in a corridor lane
  RouteDegraded,      // a transition fell back to a straight line
  CandidateScored,    // a Level 1 move was scored; `pass` is its verdict
  CandidateTerms,     // the Tier-2 shares of the score in the event before
  FoldCut,            // a folded rank run started a piece here, or was refused
  PseudostateSeated,  // an initial or final was set beside the state it joins
  PortTurned,         // a port was turned onto another border
  PortAttached,       // a segment meets a composite at its port, off its centre
  ColumnCentred,      // a state moved along its column onto a joined state's centre
  PiecePacked,        // where the packing put one piece of a component's rank run
  GapCharged,         // a rank boundary was charged width beyond rank_sep
  FoldPinned,         // a fold pin decided a frame's fold; `pass` is its mode
  BoundaryCarried,    // a fold cut took a boundary node into its neighbour's piece
  LoopFaced,      // an outer self-loop took its box's least-used face; `leg` is the net
  PortWalled,     // every face a port could take is lined; the port stays
  LaneFound,      // nudging found a lane of two or more segments
  BundleRefused,  // a check left a nudged bundle in place
  RouteWalled,    // a net crossed the walls that enclose one of its ends
  LabelCentred,   // a label found no seat beside its route and was centred on it
  RouteReseated,  // a net routed only on the grid without clearance bumpers
  RouteCrossed,   // a net found no way clear of its `RouteNet::apart` net
  RowSearched,    // a Level 2 row's Level 1 search converged; `pass` is its `RowPass`
  RowRepeated,    // a row drew an earlier row's drawing, so it is not kicked
  KickScored,     // a kick was searched to convergence; `pass` is its `KickVerdict`
  KickTaken,      // a row took a kick search's result; `pass` is its `KickHow`
};

inline constexpr uint32_t TRACE_KINDS{ static_cast<uint32_t>(TraceKind::KickTaken) + 1U };

// The kind's name in the JSON and the stream header; "none" for any other value.
char const *trace_kind_name(TraceKind k);

// What a rank boundary's charge is for; `GapCharged.pass`. `Held` adds nothing: the
// boundary's charge already covers the label.
enum class GapCause : uint16_t { Label, Lanes, Held };

// What seating an initial or final pseudostate did; `PseudostateSeated.pass`.
enum class SeatHow : uint16_t { Moved, Levelled, Declined };

// Which seating pass moved a seat; `SeatMoved.pass`.
enum class SeatPass : uint16_t {
  Attach,
  Reface,
  Align,
  Spread,
  Separate,
  Nudge,
  Loop,
  Occupied,
  Order,
  Slide
};

// A Level 1 move's outcome; `CandidateScored.pass`.
enum class MoveVerdict : uint16_t { Taken, NotViable, Inflated, NotBetter, Degraded };

// Which of a row's searches converged; `RowSearched.pass`. `Refold` adds fold moves.
enum class RowPass : uint16_t { First, Refold };

// A kick's converged cost against its row's incumbent; `KickScored.pass`.
enum class KickVerdict : uint16_t { Improves, NotBetter, NotViable };

// What a row took; `KickTaken.pass`. `Together` is each frame's best kick at once,
// `Stacked` one more frame's best on the round's pick, `Settled` the unscoped search
// after.
enum class KickHow : uint16_t { Single, Together, Stacked, Settled };

// Payloads are POD and name entities by id, never by frame node index.
struct TraceRank {
  uint32_t state, rank;
};
struct TraceChain {
  uint32_t seg, rank, index, count;
};
struct TraceSeg {
  uint32_t seg;
};
// A fold cut before frame rank `rank`; `refused` is set where the cut would split a
// pseudostate or boundary node from the node it joins.
struct TraceFold {
  uint32_t rank, refused;
  int32_t carried;  // leading-edge label room of the piece the cut starts
};
// `side` is the border the port ends on: 0 left, 1 right, 2 top, 3 bottom.
struct TracePort {
  uint32_t seg, trans, leg, side;
};
// `seg` is the boundary node's segment and `rank` the frame rank its piece starts at.
struct TraceCarry {
  uint32_t seg, rank;
};
// `rank` is the frame rank the piece starts at, the rect is frame-local as packed, and
// `carried` is its leading-edge label room.
struct TracePiece {
  uint32_t rank;
  int32_t x, y, w, h, carried;
};
// `boundary` is the frame's rank boundary after rank `boundary`; `seg` is the segment
// whose edge charged `width` there.
struct TraceGap {
  uint32_t boundary, seg;
  int32_t width;
};
// `by` is the port's offset across the ranks from the state's centre (`PortAttached`), or
// the state's move along its column (`ColumnCentred`, whose `seg` is INVALID).
struct TraceShift {
  uint32_t state, seg;
  int32_t by;
};
struct TracePlace {
  uint32_t state, seg;
  int32_t x, y;
};  // a bend sets seg, not state
struct TraceInflate {
  int32_t node_sep, rank_sep;
};
struct TraceNet {
  uint32_t seg, trans, waypoints;
  int32_t sx, sy, dx, dy;
};
struct TracePoint {
  int32_t x, y;
};
struct TraceSeat {
  uint32_t net, end;
  int32_t from_x, from_y, to_x, to_y;
};
struct TraceLane {
  uint32_t net, lane;
  int32_t at;
};
struct TraceLaneFound {
  uint32_t horizontal;  // 1 for a lane of horizontal segments
  int32_t at;           // the lane's lowest cross coordinate
  uint32_t members, bundles;
  uint32_t merged;     // bundles of two or more members
  uint32_t reordered;  // 1 when the vote order differs from the key order
  uint32_t spread;     // 1 when the room gives some member a nonzero offset
};
// `net` is the bundle's first member's, `lane` its slot as in `LaneAssigned`, and `to`
// the position that member was refused.
struct TraceBundle {
  uint32_t net, lane, members;
  int32_t to;
};
// `move` is a Level 1 move's `TRACE_MOVE_*`, and `end` (0 src, 1 dst) the end a face or
// side move changed. `row` is the Level 2 row, INVALID for a Level 1 move.
struct TraceScore {
  uint32_t row, state, rank, trans, leg;
  uint16_t move, end;
  uint32_t face;
  int32_t t0;
  int64_t t2;
};
inline constexpr uint16_t TRACE_MOVE_RANK{ 0 };
inline constexpr uint16_t TRACE_MOVE_CUT{ 1 };
inline constexpr uint16_t TRACE_MOVE_REVERSE{ 2 };
inline constexpr uint16_t TRACE_MOVE_FACE{ 3 };    // `face` holds the face
inline constexpr uint16_t TRACE_MOVE_SIDE{ 4 };    // `face` holds the side
inline constexpr uint16_t TRACE_MOVE_FOLD{ 5 };    // `rank` holds the cut's layer
inline constexpr uint16_t TRACE_MOVE_ORIENT{ 6 };  // a kick only
inline constexpr uint16_t TRACE_MOVE_LOOP{ 7 };    // `face` and `end` hold the placement
// Each Tier-2 term's share of the scored sum in basis points, in CostTerms order.
struct TraceTerms {
  std::array<int32_t, TIER2_TERMS> share;
};
// A whole search's result for Level 2 row `row`. A kick names its `TRACE_MOVE_*` in
// `move`, a reversal its segment in `trans` and `leg`; `framed_t0` and `framed` are the
// cost its frame's own search reached. `of` is the earlier row a repeated row draws.
struct TraceSearch {
  uint32_t row, of;
  uint16_t move;
  uint32_t trans, leg;
  int32_t t0, framed_t0;
  int64_t t2, framed;
};

struct TraceEvent {
  TraceKind kind{ TraceKind::None };
  uint16_t pass{ 0 };         // SeatPass / SeatHow / MoveVerdict / GapCause / Fold, else 0
  uint32_t frame{ INVALID };  // stamped by the sink unless the site sets one
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
    TraceLaneFound found;
    TraceBundle bundle;
    TraceScore score;
    TraceTerms terms;
    TraceSearch search;
    TraceFold fold;
    TracePort port;
    TraceShift shift;
    TracePiece piece;
    TraceGap gap;
    TraceCarry carry;
  };
};

// The sink the phases append to during a traced run: events are encoded into `chunk`,
// which goes to `sink` once it holds about TRACE_CHUNK bytes.
struct LayoutTrace {
  TraceChunk sink{ nullptr };
  void *ctx{ nullptr };
  std::vector<uint8_t> chunk;
  uint64_t count{ 0 };        // events encoded
  uint32_t frame{ INVALID };  // stamped onto each event that sets no frame
  bool ok{ true };            // `sink` took every chunk
};

// Encodes `e` into `t.chunk`, handing a full chunk to `t.sink`.
void trace_put(LayoutTrace &t, TraceEvent const &e);

// This thread's sink, null outside a traced run. A traced run forces `threads = 1`.
LayoutTrace *trace_sink();
void trace_sink_set(LayoutTrace *t);

inline void trace_emit(TraceEvent e) {
  LayoutTrace *const t{ trace_sink() };
  if (t == nullptr) { return; }
  if (e.frame == INVALID) { e.frame = t->frame; }
  trace_put(*t, e);
}

// This thread's sink for the search outline alone: the row and kick events, recorded with
// no other sink set, so every phase runs as untraced.
LayoutTrace *trace_outline();
void trace_outline_set(LayoutTrace *t);

// Emits a row or kick event to the outline sink, else to `trace_sink()`.
inline void trace_outline_emit(TraceEvent const &e) {
  LayoutTrace *const t{ trace_outline() };
  if (t == nullptr) {
    trace_emit(e);
    return;
  }
  trace_put(*t, e);
}

// Sets the sink's frame stamp for its scope and restores the previous one on exit.
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

// Appends `e` as one line's JSON object with ordinal `i`, naming states from a decoded
// header's `names` and `name_end`; the caller writes the separators and newlines.
void trace_event_json(TraceEvent const &e,
                      uint64_t i,
                      std::vector<char> const &names,
                      std::vector<uint32_t> const &name_end,
                      std::vector<char> &out);

inline constexpr uint32_t TRACE_MOVES{ 8 };  // `TRACE_MOVE_*` values

// Per `TRACE_MOVE_*`: moves offered, answered by the candidate memo, taken, culled
// unoffered as changing nothing, and left unscored by don't-look bits in a taking round.
struct SearchStats {
  std::array<uint64_t, TRACE_MOVES> offered{}, deduped{}, taken{}, culled{}, skipped{};
  std::array<uint64_t, TRACE_MOVES> pruned{};   // route bound reached the incumbent
  std::array<uint64_t, TRACE_MOVES> stopped{};  // routed, then Tier 2 reached it
  uint64_t drawn{ 0 };                          // moves the memo answered by their drawing
  uint64_t faced{ 0 };       // moves whose facing pass the memo answered
  uint64_t searches{ 0 };    // Level 1 searches run
  uint64_t recalled{ 0 };    // Level 1 searches the search memo answered
  uint64_t memo_bytes{ 0 };  // the most a layout's candidate memo held
  uint64_t aliased{ 0 };     // rows laid out and searched as an earlier row
  uint64_t deferred{ 0 };    // moves rescored after another thread routed their drawing
  uint64_t relaid{ 0 };      // bounds laid out afresh to set a don't-look bit
};

// One JSON object: the totals, then per move kind its offered, deduped, pruned, stopped,
// taken, culled and skipped counts.
void search_stats_to_json(SearchStats const &st, std::vector<char> &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_TRACE_H_INCLUDED
