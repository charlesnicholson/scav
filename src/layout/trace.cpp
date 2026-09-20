// The trace sink and its JSON serialization (11.16).

#include "layout/trace.h"

#include <cstring>

namespace scav {

namespace {

// Thread-local so no phase signature carries a sink; a traced run is
// single-threaded, so this is the one sink there is.
thread_local LayoutTrace *g_sink{ nullptr };

char const *kind_name(TraceKind k) {
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
    case TraceKind::None: break;
  }
  return "none";
}

char const *seat_pass_name(uint16_t p) {
  switch (static_cast<SeatPass>(p)) {
    case SeatPass::Attach: return "attach";
    case SeatPass::Reface: return "reface";
    case SeatPass::Align: return "align";
    case SeatPass::Spread: return "spread";
    case SeatPass::Separate: return "separate";
    case SeatPass::Nudge: return "nudge";
  }
  return "?";
}

char const *verdict_name(uint16_t v) {
  switch (static_cast<MoveVerdict>(v)) {
    case MoveVerdict::Taken: return "taken";
    case MoveVerdict::NotViable: return "not_viable";
    case MoveVerdict::Inflated: return "inflated";
    case MoveVerdict::NotBetter: return "not_better";
  }
  return "?";
}

// Typed appends rather than a format string: `cert-dcl50-cpp` bans the
// variadic, and the trace has four shapes of value in it.
struct Json {
  std::vector<char> &out;

  void raw(char const *s, size_t n) { out.insert(out.end(), s, s + n); }
  void raw(char const *s) { raw(s, strlen(s)); }

  void num(int64_t v) {
    char buf[24];
    size_t n{ 0 };
    uint64_t mag{ (v < 0) ? (~static_cast<uint64_t>(v) + 1U) : static_cast<uint64_t>(v) };
    do {
      buf[n++] = static_cast<char>('0' + (mag % 10U));
      mag /= 10U;
    } while (mag != 0U);
    if (v < 0) { out.push_back('-'); }
    while (n != 0) { out.push_back(buf[--n]); }
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

  // A state by name, or its ordinal when the row has none -- an initial
  // pseudostate is nameless and a bend carries no state at all.
  void kstate(Chart const &c, uint32_t v) {
    key("state");
    if ((v == INVALID) || (v >= c.states.size())) {
      raw("null");
      return;
    }
    auto const n{ chart_string(c, c.states[v].name) };  // core's view; layout may not name it
    raw("\"");
    if (n.empty()) {
      raw("#");
      num(v);
    } else {
      raw(n.data(), n.size());
    }
    raw("\"");
  }
};

}  // namespace

LayoutTrace *trace_sink() { return g_sink; }
void trace_sink_set(LayoutTrace *t) { g_sink = t; }

void trace_to_json(LayoutTrace const &t, Chart const &c, std::vector<char> &out) {
  Json j{ out };
  j.raw("[\n");
  for (size_t i = 0; i < t.events.size(); ++i) {
    TraceEvent const &e{ t.events[i] };
    j.raw("  {\"i\":");
    j.num(static_cast<int64_t>(i));
    j.ks("kind", kind_name(e.kind));
    if (e.frame != INVALID) { j.kv("frame", e.frame); }
    switch (e.kind) {
      case TraceKind::RankAssigned:
      case TraceKind::RankPinned:
        j.kstate(c, e.rank.state);
        j.kv("rank", e.rank.rank);
        break;
      case TraceKind::EdgeReversed:
      case TraceKind::RouteDegraded: j.kv("seg", e.seg.seg); break;
      case TraceKind::EdgeChained:
        j.kv("seg", e.chain.seg);
        j.kv("rank", e.chain.rank);
        j.kv("index", e.chain.index);
        j.kv("count", e.chain.count);
        break;
      case TraceKind::NodePlaced:
        if (e.place.state != INVALID) {
          j.kstate(c, e.place.state);
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
      case TraceKind::CandidateScored:
        j.ks("verdict", verdict_name(e.pass));
        j.kv("row", e.score.row);
        if (e.score.state != INVALID) {
          j.kstate(c, e.score.state);
          j.kv("rank", e.score.rank);
        }
        j.kv("t0", e.score.t0);
        j.kv("t2", e.score.t2);
        break;
      case TraceKind::None: break;
    }
    j.raw((i + 1 == t.events.size()) ? "}\n" : "},\n");
  }
  j.raw("]\n");
}

}  // namespace scav
