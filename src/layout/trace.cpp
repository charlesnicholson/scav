// The trace sink and its JSON serialization.

#include "layout/trace.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace scav {

namespace {

thread_local LayoutTrace *g_sink{ nullptr };
thread_local LayoutTrace *g_outline{ nullptr };

// Level 1 move kinds by `TRACE_MOVE_*`.
constexpr std::array<char const *, TRACE_MOVES> MOVES{ "rank",   "cut",  "reverse",
                                                       "face",   "side", "fold",
                                                       "orient", "loop", "grow" };

char const *seat_pass_name(uint16_t p) {
  switch (static_cast<SeatPass>(p)) {
    case SeatPass::Attach: return "attach";
    case SeatPass::Reface: return "reface";
    case SeatPass::Align: return "align";
    case SeatPass::Spread: return "spread";
    case SeatPass::Separate: return "separate";
    case SeatPass::Nudge: return "nudge";
    case SeatPass::Loop: return "loop";
    case SeatPass::Occupied: return "occupied";
    case SeatPass::Order: return "order";
    case SeatPass::Slide: return "slide";
  }
  return "?";
}

char const *verdict_name(uint16_t v) {
  switch (static_cast<MoveVerdict>(v)) {
    case MoveVerdict::Taken: return "taken";
    case MoveVerdict::NotViable: return "not_viable";
    case MoveVerdict::Inflated: return "inflated";
    case MoveVerdict::NotBetter: return "not_better";
    case MoveVerdict::Degraded: return "degraded";
  }
  return "?";
}

char const *kick_verdict_name(uint16_t v) {
  switch (static_cast<KickVerdict>(v)) {
    case KickVerdict::Improves: return "improves";
    case KickVerdict::NotBetter: return "not_better";
    case KickVerdict::NotViable: return "not_viable";
  }
  return "?";
}

char const *kick_how_name(uint16_t v) {
  switch (static_cast<KickHow>(v)) {
    case KickHow::Single: return "single";
    case KickHow::Together: return "together";
    case KickHow::Stacked: return "stacked";
    case KickHow::Settled: return "settled";
  }
  return "?";
}

// State names by id, end to end: state `s`'s ends at `end[s]`.
struct Names {
  std::vector<char> const &text;
  std::vector<uint32_t> const &end;
};

// Appends JSON to `out`, one typed append per value shape: number, string, point, state.
struct Json {
  std::vector<char> &out;

  void raw(char const *s, size_t n) { vec_insert(out, out.end(), s, s + n); }
  void raw(char const *s) { raw(s, strlen(s)); }

  void num(int64_t v) {
    std::array<char, 24> buf{};
    size_t n{ 0 };
    uint64_t mag{ (v < 0) ? (~static_cast<uint64_t>(v) + 1U) : static_cast<uint64_t>(v) };
    do {
      buf[n++] = static_cast<char>('0' + (mag % 10U));
      mag /= 10U;
    } while (mag != 0U);
    if (v < 0) { vec_push_back(out, '-'); }
    while (n != 0) { vec_push_back(out, buf[--n]); }
  }

  void key(char const *k) {
    raw(",\"");
    raw(k);
    raw("\":");
  }
  void kv(char const *k, int64_t v) {
    key(k);
    num(v);
  }
  void ks(char const *k, char const *v) {
    key(k);
    raw("\"");
    raw(v);
    raw("\"");
  }
  void kxy(char const *k, int32_t x, int32_t y) {
    key(k);
    raw("[");
    num(x);
    raw(",");
    num(y);
    raw("]");
  }

  // Writes `state_id` and `state`: the name, or `#` and the id when nameless; `state` is
  // null for INVALID or out-of-range `v`.
  void kstate(Names const &states, uint32_t v) {
    if ((v == INVALID) || (v >= states.end.size())) {
      key("state");
      raw("null");
      return;
    }
    kv("state_id", v);
    key("state");
    uint32_t const from{ (v == 0) ? 0U : states.end[v - 1U] };
    raw("\"");
    if (from == states.end[v]) {
      raw("#");
      num(v);
    } else {
      raw(states.text.data() + from, states.end[v] - from);
    }
    raw("\"");
  }
};

}  // namespace

char const *trace_kind_name(TraceKind k) {
  switch (k) {
    case TraceKind::RankAssigned: return "rank_assigned";
    case TraceKind::RankPinned: return "rank_pinned";
    case TraceKind::EdgeReversed: return "edge_reversed";
    case TraceKind::EdgeChained: return "edge_chained";
    case TraceKind::NodePlaced: return "node_placed";
    case TraceKind::SpacingInflated: return "spacing_inflated";
    case TraceKind::NetPlanned: return "net_planned";
    case TraceKind::NetWaypoint: return "net_waypoint";
    case TraceKind::SeatMoved: return "seat_moved";
    case TraceKind::LaneAssigned: return "lane_assigned";
    case TraceKind::RouteDegraded: return "route_degraded";
    case TraceKind::CandidateScored: return "candidate_scored";
    case TraceKind::CandidateTerms: return "candidate_terms";
    case TraceKind::FoldCut: return "fold_cut";
    case TraceKind::PseudostateSeated: return "pseudostate_seated";
    case TraceKind::PortTurned: return "port_turned";
    case TraceKind::LoopFaced: return "loop_faced";
    case TraceKind::PortWalled: return "port_walled";
    case TraceKind::PortAttached: return "port_attached";
    case TraceKind::ColumnCentred: return "column_centred";
    case TraceKind::PiecePacked: return "piece_packed";
    case TraceKind::GapCharged: return "gap_charged";
    case TraceKind::FoldPinned: return "fold_pinned";
    case TraceKind::BoundaryCarried: return "boundary_carried";
    case TraceKind::LaneFound: return "lane_found";
    case TraceKind::BundleRefused: return "bundle_refused";
    case TraceKind::RouteWalled: return "route_walled";
    case TraceKind::LabelCentred: return "label_centred";
    case TraceKind::RouteReseated: return "route_reseated";
    case TraceKind::RouteCrossed: return "route_crossed";
    case TraceKind::RowSearched: return "row_searched";
    case TraceKind::RowRepeated: return "row_repeated";
    case TraceKind::KickScored: return "kick_scored";
    case TraceKind::KickTaken: return "kick_taken";
    case TraceKind::StateGrown: return "state_grown";
    case TraceKind::FaceSpread: return "face_spread";
    case TraceKind::None: break;
  }
  return "none";
}

LayoutTrace *trace_sink() { return g_sink; }
void trace_sink_set(LayoutTrace *t) { g_sink = t; }
LayoutTrace *trace_outline() { return g_outline; }
void trace_outline_set(LayoutTrace *t) { g_outline = t; }

SearchStats::SearchStats() = default;
static_assert(std::is_trivially_copyable_v<SearchStats>);

void search_stats_to_json(SearchStats const &st, std::vector<char> &out) {
  Json j{ out };
  j.raw("{\"searches\":");
  j.num(static_cast<int64_t>(st.searches));
  j.kv("recalled", static_cast<int64_t>(st.recalled));
  j.kv("drawn", static_cast<int64_t>(st.drawn));
  j.kv("faced", static_cast<int64_t>(st.faced));
  j.kv("memo_bytes", static_cast<int64_t>(st.memo_bytes));
  j.kv("aliased", static_cast<int64_t>(st.aliased));
  j.kv("deferred", static_cast<int64_t>(st.deferred));
  j.kv("relaid", static_cast<int64_t>(st.relaid));
  j.raw(",\"moves\":{");
  for (uint32_t k = 0; k < TRACE_MOVES; ++k) {
    j.raw((k == 0) ? "\"" : ",\"");
    j.raw(MOVES[k]);
    j.raw(R"(":{"offered":)");
    j.num(static_cast<int64_t>(st.offered[k]));
    j.kv("deduped", static_cast<int64_t>(st.deduped[k]));
    j.kv("pruned", static_cast<int64_t>(st.pruned[k]));
    j.kv("stopped", static_cast<int64_t>(st.stopped[k]));
    j.kv("taken", static_cast<int64_t>(st.taken[k]));
    j.kv("culled", static_cast<int64_t>(st.culled[k]));
    j.kv("skipped", static_cast<int64_t>(st.skipped[k]));
    j.raw("}");
  }
  j.raw("}}\n");
}

void trace_event_json(TraceEvent const &e,
                      uint64_t i,
                      std::vector<char> const &names,
                      std::vector<uint32_t> const &name_end,
                      std::vector<char> &out) {
  Names const states{ .text = names, .end = name_end };
  Json j{ out };
  j.raw("  {\"i\":");
  j.num(static_cast<int64_t>(i));
  j.ks("kind", trace_kind_name(e.kind));
  if (e.frame != INVALID) { j.kv("frame", e.frame); }
  switch (e.kind) {
    case TraceKind::RankAssigned:
    case TraceKind::RankPinned:
      j.kstate(states, e.rank.state);
      j.kv("rank", e.rank.rank);
      break;
    case TraceKind::EdgeReversed:
    case TraceKind::RouteDegraded:
    case TraceKind::RouteWalled:
    case TraceKind::LabelCentred:
    case TraceKind::RouteReseated:
    case TraceKind::RouteCrossed: j.kv("seg", e.seg.seg); break;
    case TraceKind::EdgeChained:
      j.kv("seg", e.chain.seg);
      j.kv("rank", e.chain.rank);
      j.kv("index", e.chain.index);
      j.kv("count", e.chain.count);
      break;
    case TraceKind::NodePlaced:
      if (e.place.state != INVALID) {
        j.kstate(states, e.place.state);
      } else {
        j.kv("bend_of_seg", e.place.seg);
      }
      j.kxy("at", e.place.x, e.place.y);
      break;
    case TraceKind::SpacingInflated:
      j.kv("node_sep", e.inflate.node_sep);
      j.kv("rank_sep", e.inflate.rank_sep);
      break;
    case TraceKind::NetPlanned:
      j.kv("seg", e.net.seg);
      j.kv("trans", e.net.trans);
      j.kv("waypoints", e.net.waypoints);
      j.kxy("src", e.net.sx, e.net.sy);
      j.kxy("dst", e.net.dx, e.net.dy);
      break;
    case TraceKind::NetWaypoint: j.kxy("at", e.point.x, e.point.y); break;
    case TraceKind::SeatMoved:
      j.ks("pass", seat_pass_name(e.pass));
      j.kv("net", e.seat.net);
      j.ks("end", (e.seat.end == 0) ? "src" : "dst");
      j.kxy("from", e.seat.from_x, e.seat.from_y);
      j.kxy("to", e.seat.to_x, e.seat.to_y);
      break;
    case TraceKind::LaneAssigned:
      j.kv("net", e.lane.net);
      j.kv("lane", e.lane.lane);
      j.kv("at", e.lane.at);
      break;
    case TraceKind::LaneFound:
      j.kv("horizontal", e.found.horizontal);
      j.kv("at", e.found.at);
      j.kv("members", e.found.members);
      j.kv("bundles", e.found.bundles);
      j.kv("merged", e.found.merged);
      j.kv("reordered", e.found.reordered);
      j.kv("spread", e.found.spread);
      break;
    case TraceKind::BundleRefused:
      j.kv("net", e.bundle.net);
      j.kv("lane", e.bundle.lane);
      j.kv("members", e.bundle.members);
      j.kv("to", e.bundle.to);
      break;
    case TraceKind::CandidateScored:
      j.ks("verdict", verdict_name(e.pass));
      j.kv("row", e.score.row);
      if (e.score.state != INVALID) {
        j.kstate(states, e.score.state);
        j.kv("rank", e.score.rank);
      }
      if (e.score.trans != INVALID) {
        j.kv("trans", e.score.trans);
        j.kv("leg", e.score.leg);
      }
      if (e.score.row == INVALID) {
        j.ks("move", (e.score.move < MOVES.size()) ? MOVES[e.score.move] : "?");
        if (e.score.move == TRACE_MOVE_FACE) {
          j.ks("end", (e.score.end == 0) ? "src" : "dst");
          j.kv("face", e.score.face);
        }
        if (e.score.move == TRACE_MOVE_SIDE) {
          j.ks("end", (e.score.end == 0) ? "src" : "dst");
          j.kv("side", e.score.face);
        }
        if (e.score.move == TRACE_MOVE_FOLD) { j.kv("layer", e.score.rank); }
        if (e.score.move == TRACE_MOVE_LOOP) {
          j.kv("face", e.score.face);
          j.kv("end", e.score.end);
        }
      }
      j.kv("t0", e.score.t0);
      j.kv("t2", e.score.t2);
      break;
    case TraceKind::CandidateTerms:
      j.key("share");
      j.raw("[");
      for (uint32_t k = 0; k < TIER2_TERMS; ++k) {
        if (k != 0) { j.raw(","); }
        j.num(e.terms.share[k]);
      }
      j.raw("]");
      break;
    case TraceKind::FoldCut:
      j.kv("rank", e.fold.rank);
      j.kv("refused", e.fold.refused);
      j.kv("carried", e.fold.carried);
      break;
    case TraceKind::PseudostateSeated: {
      static constexpr std::array<char const *, 3> HOW{ "moved", "levelled", "declined" };
      j.kstate(states, e.rank.state);
      j.ks("how", (e.pass < HOW.size()) ? HOW[e.pass] : "?");
      break;
    }
    case TraceKind::LoopFaced:
      j.kv("seg", e.port.seg);
      j.kv("trans", e.port.trans);
      j.kv("net", e.port.leg);
      j.kv("side", e.port.side);
      break;
    case TraceKind::PortTurned:
    case TraceKind::PortWalled:
      j.kv("seg", e.port.seg);
      j.kv("trans", e.port.trans);
      j.kv("leg", e.port.leg);
      j.kv("side", e.port.side);
      break;
    case TraceKind::PortAttached:
      j.kstate(states, e.shift.state);
      j.kv("seg", e.shift.seg);
      j.kv("at", e.shift.by);
      break;
    case TraceKind::ColumnCentred:
      j.kstate(states, e.shift.state);
      j.kv("by", e.shift.by);
      break;
    case TraceKind::PiecePacked:
      j.kv("rank", e.piece.rank);
      j.kxy("at", e.piece.x, e.piece.y);
      j.kxy("size", e.piece.w, e.piece.h);
      j.kv("carried", e.piece.carried);
      break;
    case TraceKind::GapCharged:
      switch (static_cast<GapCause>(e.pass)) {
        case GapCause::Label: j.ks("cause", "label"); break;
        case GapCause::Lanes: j.ks("cause", "lanes"); break;
        case GapCause::Held: j.ks("cause", "held"); break;
      }
      j.kv("boundary", e.gap.boundary);
      j.kv("seg", e.gap.seg);
      j.kv("width", e.gap.width);
      break;
    case TraceKind::FoldPinned: {
      static constexpr std::array<char const *, 3> MODE{ "scale", "always", "never" };
      j.ks("mode", (e.pass < MODE.size()) ? MODE[e.pass] : "?");
      j.kv("layer", e.fold.rank);
      break;
    }
    case TraceKind::BoundaryCarried:
      j.kv("seg", e.carry.seg);
      j.kv("rank", e.carry.rank);
      break;
    case TraceKind::RowSearched:
      j.ks("pass",
           (e.pass == static_cast<uint16_t>(RowPass::Refold)) ? "refold" : "first");
      j.kv("row", e.search.row);
      j.kv("t0", e.search.t0);
      j.kv("t2", e.search.t2);
      break;
    case TraceKind::RowRepeated:
      j.kv("row", e.search.row);
      j.kv("of", e.search.of);
      break;
    case TraceKind::KickScored: {
      uint16_t const m{ e.search.move };
      char const *kick{ "orient" };
      if (m == TRACE_MOVE_REVERSE) { kick = "reverse"; }
      if (m == TRACE_MOVE_FOLD) { kick = "fold"; }
      if (m == TRACE_MOVE_GROW) { kick = "grow"; }
      j.ks("verdict", kick_verdict_name(e.pass));
      j.kv("row", e.search.row);
      j.ks("move", kick);
      if (m == TRACE_MOVE_REVERSE) {
        j.kv("trans", e.search.trans);
        j.kv("leg", e.search.leg);
      }
      if (m == TRACE_MOVE_GROW) {
        j.kstate(states, e.search.state);
        j.kxy("seats",
              static_cast<int32_t>(e.search.seats_w),
              static_cast<int32_t>(e.search.seats_h));
      }
      j.kv("t0", e.search.t0);
      j.kv("t2", e.search.t2);
      j.kv("framed_t0", e.search.framed_t0);
      j.kv("framed", e.search.framed);
      break;
    }
    case TraceKind::KickTaken:
      j.ks("how", kick_how_name(e.pass));
      j.kv("row", e.search.row);
      j.kv("t0", e.search.t0);
      j.kv("t2", e.search.t2);
      break;
    case TraceKind::StateGrown:
      j.ks("by", (e.pass != 0) ? "pin" : "default");
      j.kstate(states, e.grow.state);
      j.kv("ends", e.grow.ends);
      j.kxy("seats",
            static_cast<int32_t>(e.grow.seats_w),
            static_cast<int32_t>(e.grow.seats_h));
      j.kxy("from", e.grow.from_w, e.grow.from_h);
      j.kxy("to", e.grow.to_w, e.grow.to_h);
      break;
    case TraceKind::FaceSpread:
      j.kv("box", e.spread.box);
      j.kv("face", e.spread.face);
      j.kv("round", e.spread.round);
      j.kv("seats", e.spread.seats);
      j.kv("step", e.spread.step);
      j.kv("blocks", e.spread.blocks);
      j.kv("relaxed", e.spread.relaxed);
      j.kv("frozen", e.spread.frozen);
      break;
    case TraceKind::None: break;
  }
  j.raw("}");
}

}  // namespace scav
