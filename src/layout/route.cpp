// A transition's polyline is its segments' nets end to end: one net per segment,
// routed in that segment's frame, with a port slot at every border it crosses.

#include "layout/route.h"

#include "layout/nudge.h"

#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/label.h"
#include "layout/order.h"
#include "layout/router.h"
#include "layout/shard.h"
#include "layout/size.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_int.h"
#include "scav_shard.h"
#include "scav_stable_sort.h"
#include "scav_thread.h"
#include "scav_vec.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace scav {

namespace {

// A state's four bands and its loop room, each a wall to everything but its own loops.
std::array<scav_rect, 5> bands_of(SizedLayout const &z, uint32_t st) {
  auto const row = [st](std::vector<scav_rect> const &v) {
    return (st < v.size()) ? v[st] : scav_rect{};
  };
  return { row(z.before), row(z.after), row(z.lead), row(z.trail), row(z.loop) };
}

scav_point centre(scav_rect const &r) {
  return { .x = r.x + floor_div(r.w, 2), .y = r.y + floor_div(r.h, 2) };
}

// Moves `a` toward `b` by `amount`, capped at half the distance so the two
// ends cannot cross.
scav_point trim(scav_point a, scav_point b, int32_t amount) {
  if (amount <= 0) { return a; }
  Wide const dx{ static_cast<Wide>(b.x) - a.x };
  Wide const dy{ static_cast<Wide>(b.y) - a.y };
  Wide const len{ static_cast<Wide>(isqrt(static_cast<uint64_t>((dx * dx) + (dy * dy)))) };
  if (len == 0) { return a; }
  Wide const k{ imin(Wide{ amount }, floor_div(len, Wide{ 2 })) };
  return { .x = a.x + static_cast<int32_t>(floor_div(dx * k, len)),
           .y = a.y + static_cast<int32_t>(floor_div(dy * k, len)) };
}

// One segment's routing problem, before it is grouped into its frame's batch.
struct Planned {
  uint32_t frame;
  scav_point src, dst;
  uint32_t src_state, dst_state;  // -> states, INVALID unless the end is a box centre
  uint32_t seg;                   // -> SplitGraph::segments, for its bend chain
  uint32_t face;  // both ends' face where no pin names one; INVALID for the router's rule
  int32_t loop;   // `RouteNet::loop`
};

// One frame's answer, written by the shard that owns the frame and read back
// in frame order.
// True when `b` is `a` with every coordinate moved by one delta, which it
// writes to `dx`/`dy`. Compares exactly what the router and the nudger read, so
// the only thing left to assume is that the router answers a shifted question
// with a shifted answer -- which `router_orthogonal_tests` pins directly
// (11.10c).
bool same_but_shifted(RouteFrameCache const &a,
                      RouteInput const &b,
                      scav_rect const &frame,
                      int32_t &dx,
                      int32_t &dy) {
  RouteInput const &x{ a.in };
  if ((x.obstacles.size() != b.obstacles.size()) || (x.nets.size() != b.nets.size()) ||
      (x.waypoints.size() != b.waypoints.size()) || (x.inscribed != b.inscribed) ||
      (x.corner != b.corner)) {
    return false;
  }
  if ((x.region.w != b.region.w) || (x.region.h != b.region.h)) { return false; }
  if ((x.enclosure.w != b.enclosure.w) || (x.enclosure.h != b.enclosure.h) ||
      ((b.enclosure.x - x.enclosure.x) != (b.region.x - x.region.x)) ||
      ((b.enclosure.y - x.enclosure.y) != (b.region.y - x.region.y))) {
    return false;
  }
  if (std::memcmp(&x.profile, &b.profile, sizeof(scav_profile)) != 0) { return false; }
  dx = b.region.x - x.region.x;
  dy = b.region.y - x.region.y;
  auto const moved_pt = [dx, dy](scav_point const &p, scav_point const &q) {
    return ((q.x - p.x) == dx) && ((q.y - p.y) == dy);
  };
  auto const moved_rect = [&](scav_rect const &p, scav_rect const &q) {
    return (p.w == q.w) && (p.h == q.h) && ((q.x - p.x) == dx) && ((q.y - p.y) == dy);
  };
  if (!moved_rect(a.frame, frame)) { return false; }
  for (uint32_t i = 0; i < x.obstacles.size(); ++i) {
    if (!moved_rect(x.obstacles[i], b.obstacles[i])) { return false; }
  }
  for (uint32_t i = 0; i < x.waypoints.size(); ++i) {
    if (!moved_pt(x.waypoints[i], b.waypoints[i])) { return false; }
  }
  for (uint32_t i = 0; i < x.nets.size(); ++i) {
    RouteNet const &p{ x.nets[i] };
    RouteNet const &q{ b.nets[i] };
    if ((p.src_obstacle != q.src_obstacle) || (p.dst_obstacle != q.dst_obstacle) ||
        (p.waypoint_off != q.waypoint_off) || (p.waypoint_len != q.waypoint_len) ||
        (p.src_face != q.src_face) || (p.dst_face != q.dst_face) || (p.lean != q.lean) ||
        (p.loop != q.loop) ||
        !moved_pt(p.src, q.src) || !moved_pt(p.dst, q.dst)) {
      return false;
    }
  }
  return true;
}

struct FrameRoutes {
  std::vector<scav_point> points;
  std::vector<scav_span> net_points;  // -> points, parallel to the frame's nets
  std::vector<RouteMetrics> metrics;  // parallel to the frame's nets
  NudgeStats nudged;
};

// Per-thread, reused across every frame the thread routes. A shard never waits on the
// pool, so no second shard on this thread starts while one uses it.
struct FrameScratch {
  RouteInput in;
  RouteOutput ro;
  std::vector<uint32_t> obstacle_index;  // -> in.obstacles; INVALID off this frame
  std::vector<uint32_t> obstacle_states;
  std::vector<scav_rect> own;
  std::vector<uint8_t> in_chain;  // parallel to states; 1 on the frame owner's chain
  std::vector<uint32_t> chain;    // -> states, the owner and its enclosing states
  std::vector<uint64_t> pick;     // one bit per state, the gather's candidates
};

FrameScratch &frame_scratch() {
  thread_local FrameScratch s;
  return s;
}

// Every buffer one call holds across its `parallel_for`, reassigned in place.
struct CallScratch {
  std::array<std::vector<uint32_t>, 2> faces;
  std::vector<uint32_t> port_seg, seg_reversed;
  std::vector<std::vector<uint32_t>> seg_bends;
  std::vector<uint8_t> source_node;
  std::vector<Planned> planned;
  std::vector<Span> trans_nets;
  std::vector<scav_extent> loop_label;
  std::vector<scav_rect> loop_row;
  std::vector<uint32_t> face_use;  // per state and face, the ends seated there
  std::vector<scav_point> loop_points;
  std::vector<scav_span> loop_span;  // per transition, its inner loop in `loop_points`
  std::vector<std::vector<uint32_t>> by_frame;
  std::vector<FrameRoutes> frames;
  std::vector<scav_point> routed;
  std::vector<scav_span> net_span;
  std::vector<scav_rect> walls, held;
  std::vector<uint8_t> up;
  std::vector<uint32_t> enclosing;  // parallel to states: `enclosing_state` of each
  // The live states each state encloses, as lists threaded through `kid_next`
  // from `kid_head`, and the live states a gather must always visit.
  std::vector<uint32_t> kid_head, kid_next, loose;
};

// A thread waiting in `parallel_for` can route another candidate before its call returns,
// so each call takes the next scratch down this stack for its whole run.
std::vector<CallScratch> &call_stack() {
  thread_local std::vector<CallScratch> s;
  return s;
}

// `v` resized to `n` empty lists, each keeping the capacity it had.
void reset_lists(std::vector<std::vector<uint32_t>> &v, size_t n) {
  vec_resize(v, n);
  for (std::vector<uint32_t> &list : v) { list.clear(); }
}

void merge_nudged(NudgeStats &into, NudgeStats const &from) {
  into.lanes += from.lanes;
  into.spread += from.spread;
  into.moved += from.moved;
  into.bundles += from.bundles;
  into.refused += from.refused;
  into.reordered += from.reordered;
}

}  // namespace

Routes route_transitions(Chart const &c,
                         SplitGraph const &g,
                         SubmachineOrders const &o,
                         SizedLayout const &z,
                         scav_spaces const &s,
                         scav_profile const &p,
                         Router const &router,
                         uint32_t threads,
                         RouteCache const *reuse,
                         RouteCache *fill,
                         SearchPins const *pins,
                         Routes const *was,
                         bool labels) {
  Routes out;
  route_transitions(out,
                    c,
                    g,
                    o,
                    z,
                    s,
                    p,
                    router,
                    threads,
                    reuse,
                    fill,
                    pins,
                    was,
                    labels);
  return out;
}

void route_transitions(Routes &out,
                       Chart const &c,
                       SplitGraph const &g,
                       SubmachineOrders const &o,
                       SizedLayout const &z,
                       scav_spaces const &s,
                       scav_profile const &p,
                       Router const &router,
                       uint32_t threads,
                       RouteCache const *reuse,
                       RouteCache *fill,
                       SearchPins const *pins,
                       Routes const *was,
                       bool labels) {
  out.points.clear();
  out.slots.clear();
  out.placed.clear();
  out.settled.clear();
  out.outside_region = 0;
  out.unreachable = 0;
  out.too_large = 0;
  out.reseated = 0;
  out.nudged = {};
  out.unplaced = 0;
  std::vector<CallScratch> &stack{ call_stack() };
  CallScratch cs;
  if (!stack.empty()) {
    cs = std::move(stack.back());
    stack.pop_back();
  }
  // `{trans, leg, end}` resolved to a face per segment end once, so the frame
  // workers read a flat table rather than searching the pin list per net.
  std::array<std::vector<uint32_t>, 2> &faces{ cs.faces };
  for (std::vector<uint32_t> &side : faces) { side.clear(); }
  if ((pins != nullptr) && !pins->faces.empty()) {
    for (std::vector<uint32_t> &side : faces) {
      vec_assign(side, g.segments.size(), INVALID);
    }
    for (FacePin const &fp : pins->faces) {
      if ((fp.trans.v == INVALID) || (fp.trans.v >= g.trans_segments.size()) ||
          (fp.end > 1) || (fp.face > 3)) {
        continue;
      }
      Span const segs{ g.trans_segments[fp.trans.v] };
      if (fp.leg >= segs.len) { continue; }
      faces[fp.end][segs.off + fp.leg] = fp.face;
    }
  }
  if (fill != nullptr) {
    vec_assign(fill->frame, c.submachines.size(), {});
    vec_assign(fill->faceable, size_t{ 2 } * g.segments.size(), 0);
  }
  uint32_t const n{ static_cast<uint32_t>(c.transitions.size()) };
  vec_assign(out.route, n, {});
  vec_assign(out.port, n, {});
  vec_assign(out.failed, n, 0);

  // The segment each port's boundary node belongs to, and the bends each
  // segment was chained through, both gathered once.
  std::vector<uint32_t> &port_seg{ cs.port_seg };
  vec_assign(port_seg, g.ports.size(), INVALID);
  for (uint32_t seg = 0; seg < g.segments.size(); ++seg) {
    if (o.seg_port[seg] != INVALID) { port_seg[o.seg_port[seg]] = seg; }
  }
  std::vector<uint32_t> &seg_reversed{ cs.seg_reversed };
  vec_assign(seg_reversed, g.segments.size(), 0);
  for (OrderEdge const &e : o.edges) { seg_reversed[e.segment] = e.reversed; }
  std::vector<std::vector<uint32_t>> &seg_bends{ cs.seg_bends };
  reset_lists(seg_bends, g.segments.size());
  for (uint32_t node = 0; node < o.nodes.size(); ++node) {
    if (o.nodes[node].kind == OrderKind::Bend) {
      vec_push_back(seg_bends[o.nodes[node].subject], node);
    }
  }
  for (uint32_t seg = 0; seg < seg_bends.size(); ++seg) {
    std::vector<uint32_t> &chain{ seg_bends[seg] };
    scav_stable_sort(chain, [&](uint32_t a, uint32_t b) {
      return o.nodes[a].rank < o.nodes[b].rank;
    });
    // Ranks climb in the acyclic direction, which is the authored one only
    // when the segment's edge is not reversed.
    if (seg_reversed[seg] != 0) {
      for (uint32_t i = 0; i < (chain.size() / 2); ++i) {
        uint32_t const other{ chain[i] };
        chain[i] = chain[chain.size() - 1 - i];
        chain[chain.size() - 1 - i] = other;
      }
    }
  }

  // Whether a route arrives at a boundary or leaves through one. Read from the
  // node's direction, not its absolute x, which carries the packer's offset.
  std::vector<uint8_t> &source_node{ cs.source_node };
  vec_assign(source_node, o.nodes.size(), 0);
  for (OrderEdge const &e : o.edges) { source_node[e.src] = 1; }

  // A slot sits on the crossed box's own border, at the height its boundary
  // node ended up.
  auto const slot_of = [&](uint32_t port) {
    SplitPort const &pt{ g.ports[port] };
    uint32_t const seg{ port_seg[port] };
    uint32_t const node{ (seg == INVALID) ? INVALID : o.seg_node[seg] };
    bool const on_state{ pt.state.v != INVALID };
    scav_rect const box{ on_state ? z.state[pt.state.v] : z.sub[pt.sub.v] };
    uint32_t const depth{ on_state ? g.state_depth[pt.state.v]
                                   : g.state_depth[c.submachines[pt.sub.v].owner.v] + 1 };
    if (node == INVALID) {
      scav_point const at{ centre(box) };
      return scav_port_slot{ .x = at.x, .y = at.y, .side = 0, .boundary_depth = depth };
    }
    bool const leading{ source_node[node] != 0 };
    // On the border the frame's ranks start and end at: left and right for a frame running
    // across, top and bottom for one running down; a cross-border node is on the others.
    uint32_t const frame{ g.segments[seg].frame.v };
    bool const down{ (frame < o.sub_down.size()) && (o.sub_down[frame] != 0) };
    uint8_t const cross{ (seg < o.seg_cross.size()) ? o.seg_cross[seg] : uint8_t{ 0 } };
    if (cross != 0) {
      bool const first{ cross == 1 };
      if (down) {
        return scav_port_slot{ .x = first ? box.x : (box.x + box.w),
                               .y = z.node[node].y,
                               .side = first ? 0U : 1U,
                               .boundary_depth = depth };
      }
      return scav_port_slot{ .x = z.node[node].x,
                             .y = first ? box.y : (box.y + box.h),
                             .side = first ? 2U : 3U,
                             .boundary_depth = depth };
    }
    if (down) {
      return scav_port_slot{ .x = z.node[node].x,
                             .y = leading ? box.y : (box.y + box.h),
                             .side = leading ? 2U : 3U,
                             .boundary_depth = depth };
    }
    return scav_port_slot{ .x = leading ? box.x : (box.x + box.w),
                           .y = z.node[node].y,
                           .side = leading ? 0U : 1U,
                           .boundary_depth = depth };
  };

  // Plan every net before routing any, so the port slots come out in
  // transition order however the frames are then visited.
  std::vector<Planned> &planned{ cs.planned };
  planned.clear();
  std::vector<Span> &trans_nets{ cs.trans_nets };
  vec_assign(trans_nets, n, Span{});
  std::vector<scav_rect> &loop_row{ cs.loop_row };
  loop_rows(c, z, s, p, cs.loop_label, loop_row);
  std::vector<scav_point> &loop_points{ cs.loop_points };
  loop_points.clear();
  std::vector<scav_span> &loop_span{ cs.loop_span };
  vec_assign(loop_span, n, scav_span{});
  for (uint32_t t = 0; t < n; ++t) {
    Span const segs{ g.trans_segments[t] };
    if (segs.len == 0) { continue; }
    Transition const &tr{ c.transitions[t] };
    uint32_t const first_net{ static_cast<uint32_t>(planned.size()) };
    uint32_t const first_slot{ static_cast<uint32_t>(out.slots.size()) };

    if (inner_loop(c, t)) {
      // Out of the state's inner trailing face and back, in the row of its loop room
      // this transition takes; the room is reserved, so nothing is routed round.
      scav_rect const r{ z.state[tr.src.v] };
      scav_rect const row{ loop_row[t] };
      int32_t const lane{ imin(loop_lane(p), row.h) };
      int32_t const ya{ row.y + floor_div(row.h - lane, 2) };
      int32_t const x{ (row.x + row.w) - loop_reach(p) };
      int32_t const right{ r.x + r.w };
      loop_span[t] = { .off = static_cast<uint32_t>(loop_points.size()), .len = 4 };
      vec_push_back(loop_points, { .x = right, .y = ya });
      vec_push_back(loop_points, { .x = x, .y = ya });
      vec_push_back(loop_points, { .x = x, .y = ya + lane });
      vec_push_back(loop_points, { .x = right, .y = ya + lane });
    } else if (tr.src == tr.dst) {
      // Out of the trailing face and back: both ends name the state, so the router
      // seats them apart like any departure and arrival sharing a face.
      uint32_t const frame{ g.segments[segs.off].frame.v };
      bool const down{ (frame < o.sub_down.size()) && (o.sub_down[frame] != 0) };
      scav_point const mid{ centre(z.state[tr.src.v]) };
      vec_push_back(planned,
                    { .frame = frame,
                      .src = mid,
                      .dst = mid,
                      .src_state = tr.src.v,
                      .dst_state = tr.src.v,
                      .seg = segs.off,
                      .face = down ? 3U : 1U,
                      .loop = 2 * p.pad });
    } else {
      // An endpoint that encloses its end of the route is met on that state's
      // inner face, which is where phase 1 put the segment's boundary node.
      // Nothing is crossed there, so the end names no obstacle and no slot.
      uint32_t const head{ o.seg_node[segs.off] };
      bool const head_inner{ (g.segments[segs.off].src_inner != 0) && (head != INVALID) };
      scav_point at{ head_inner ? z.node[head] : centre(z.state[tr.src.v]) };
      uint32_t at_state{ head_inner ? INVALID : tr.src.v };
      for (uint32_t k = 0; k < segs.len; ++k) {
        uint32_t const seg{ segs.off + k };
        uint32_t const port{ g.segments[seg].dst_port };
        uint32_t const tail{ o.seg_node[seg] };
        bool const tail_inner{ (g.segments[seg].dst_inner != 0) && (tail != INVALID) };
        scav_point end{};
        uint32_t end_state{ INVALID };
        if (port != INVALID) {
          scav_port_slot const slot{ slot_of(port) };
          vec_push_back(out.slots, slot);
          end = { .x = slot.x, .y = slot.y };
        } else if (tail_inner) {
          end = z.node[tail];
        } else {
          end = centre(z.state[tr.dst.v]);
          end_state = tr.dst.v;
        }
        // The channel between two concurrent regions of one state is routed
        // inside that state, in the source region's frame: in the frame the
        // state sits in, the state is an obstacle walling the route out of
        // the space between its own regions, and it went the long way round
        // outside them (11.8).
        uint32_t frame{ g.segments[seg].frame.v };
        uint32_t const from_port{ g.segments[seg].src_port };
        if ((g.segments[seg].separator != 0) && (from_port < g.ports.size()) &&
            (g.ports[from_port].sub.v < c.submachines.size())) {
          frame = g.ports[from_port].sub.v;
        }
        vec_push_back(planned,
                      { .frame = frame,
                        .src = at,
                        .dst = end,
                        .src_state = at_state,
                        .dst_state = end_state,
                        .seg = seg,
                        .face = INVALID,
                        .loop = 0 });
        at = end;
        at_state = end_state;
      }
    }
    trans_nets[t] =
        make_span(first_net, static_cast<uint32_t>(planned.size()) - first_net);
    out.port[t] = { .off = first_slot,
                    .len = static_cast<uint32_t>(out.slots.size()) - first_slot };
  }

  // An outer loop takes its state's least-used face, the trailing one first: a port's
  // side, and the face an end naming the state leaves by under the separation rule.
  std::vector<uint32_t> &face_use{ cs.face_use };
  vec_assign(face_use, size_t{ 4 } * c.states.size(), 0);
  for (uint32_t t = 0; t < n; ++t) {
    for (uint32_t k = 0; k < out.port[t].len; ++k) {
      scav_port_slot const &slot{ out.slots[out.port[t].off + k] };
      uint32_t const port{ g.segments[g.trans_segments[t].off + k].dst_port };
      if ((port < g.ports.size()) && (g.ports[port].state.v < c.states.size())) {
        ++face_use[(size_t{ 4 } * g.ports[port].state.v) + slot.side];
      }
    }
  }
  for (Planned const &pn : planned) {
    if (pn.loop > 0) { continue; }
    for (uint32_t end = 0; end < 2; ++end) {
      uint32_t const st{ (end == 0) ? pn.src_state : pn.dst_state };
      if (st >= c.states.size()) { continue; }
      std::vector<uint32_t> const &bends{ seg_bends[pn.seg] };
      scav_point aim{ (end == 0) ? pn.dst : pn.src };
      if (!bends.empty()) { aim = z.node[(end == 0) ? bends.front() : bends.back()]; }
      scav_rect const &r{ z.state[st] };
      Wide const dx{ imax(imax(Wide{ r.x } - aim.x, Wide{ aim.x } - (Wide{ r.x } + r.w)), Wide{ 0 }) };
      Wide const dy{ imax(imax(Wide{ r.y } - aim.y, Wide{ aim.y } - (Wide{ r.y } + r.h)), Wide{ 0 }) };
      scav_point const mid{ centre(r) };
      uint32_t const face{ (dx >= dy) ? ((aim.x < mid.x) ? 0U : 1U) : ((aim.y < mid.y) ? 2U : 3U) };
      ++face_use[(size_t{ 4 } * st) + face];
    }
  }
  for (Planned &pn : planned) {
    if (pn.loop <= 0) { continue; }
    bool const down{ pn.face == 3U };
    std::array<uint32_t, 4> const order{ down ? std::array<uint32_t, 4>{ 3, 1, 0, 2 }
                                              : std::array<uint32_t, 4>{ 1, 2, 3, 0 } };
    uint32_t const *use{ face_use.data() + (size_t{ 4 } * pn.src_state) };
    for (uint32_t const face : order) {
      if (use[face] < use[pn.face]) { pn.face = face; }
    }
  }

  // One batch per frame, in submachine order. A frame's nets keep the order
  // they were planned in, which is `(transition, ordinal)`.
  std::vector<std::vector<uint32_t>> &by_frame{ cs.by_frame };
  reset_lists(by_frame, c.submachines.size());
  for (uint32_t i = 0; i < planned.size(); ++i) {
    if (planned[i].frame < by_frame.size()) {
      vec_push_back(by_frame[planned[i].frame], i);
    }
  }

  int32_t const margin{ router.margin(p) };
  std::vector<FrameRoutes> &frames{ cs.frames };
  vec_resize(frames, by_frame.size());
  for (FrameRoutes &fr : frames) {
    fr.points.clear();
    fr.net_points.clear();
    fr.metrics.clear();
    fr.nudged = {};
  }

  // Every obstacle of a frame is loose or enclosed by a state on the owner's chain. A
  // loose state has no enclosing state, or lies outside its enclosing state's box.
  uint32_t const state_count{ static_cast<uint32_t>(c.states.size()) };
  std::vector<uint32_t> &enclosing{ cs.enclosing };
  vec_resize(enclosing, state_count);
  for (uint32_t st = 0; st < state_count; ++st) {
    enclosing[st] = enclosing_state(c, { st }).v;
  }
  std::vector<uint32_t> &kid_head{ cs.kid_head };
  std::vector<uint32_t> &kid_next{ cs.kid_next };
  std::vector<uint32_t> &loose{ cs.loose };
  vec_assign(kid_head, state_count, INVALID);
  vec_resize(kid_next, state_count);
  loose.clear();
  for (uint32_t st = 0; st < state_count; ++st) {
    if (c.states[st].live == 0) { continue; }
    uint32_t const up{ enclosing[st] };
    if (up >= state_count) {
      vec_push_back(loose, st);
      continue;
    }
    kid_next[st] = kid_head[up];
    kid_head[up] = st;
    if (!contains(z.state[up], z.state[st])) { vec_push_back(loose, st); }
  }

  // Reads the model, the orders, the geometry and the plan; writes `frames[m]`
  // and the caller's own scratch, so two frames share nothing.
  auto const route_frame = [&](uint32_t m, FrameScratch &sc) {
    TraceFrame const traced{ SubmachineId{ m } };
    RouteInput &in{ sc.in };
    RouteOutput &ro{ sc.ro };
    in.obstacles.clear();
    in.inscribed.clear();
    // Cleared with the obstacles it parallels: otherwise a later frame reads an
    // earlier one's radius at that index and the corner inset stops applying.
    in.corner.clear();
    in.nets.clear();
    in.waypoints.clear();
    sc.obstacle_states.clear();

    // Grown to hold every point its nets reach: a port sits on the *crossed* box's
    // border, which is outside this submachine by the owner's padding.
    scav_rect region{ z.sub[m] };
    auto const cover = [&region](scav_point at) {
      int32_t const right{ imax(region.x + region.w, at.x) };
      int32_t const bottom{ imax(region.y + region.h, at.y) };
      region.x = imin(region.x, at.x);
      region.y = imin(region.y, at.y);
      region.w = right - region.x;
      region.h = bottom - region.y;
    };
    for (uint32_t const i : by_frame[m]) {
      cover(planned[i].src);
      cover(planned[i].dst);
      for (uint32_t const bend : seg_bends[planned[i].seg]) { cover(z.node[bend]); }
      if (planned[i].loop > 0) {
        scav_rect const &r{ z.state[planned[i].src_state] };
        int32_t const reach{ planned[i].loop };
        cover({ .x = r.x - reach, .y = r.y - reach });
        cover({ .x = r.x + r.w + reach, .y = r.y + r.h + reach });
      }
    }

    // Exactly what this router asked for, not a guess: a margin larger than it
    // needs is canvas nobody draws in, smaller leaves a frame-edge box no lane.
    region.x -= margin;
    region.y -= margin;
    region.w += 2 * margin;
    region.h += 2 * margin;
    in.region = region;

    // Every live box overlapping the region except those enclosing it, and of those only
    // the outermost. The chain matches `ancestor_or_self`; candidates go in state order.
    StateId const owner{ c.submachines[m].owner };
    sc.chain.clear();
    uint32_t link{ owner.v };
    for (uint32_t step = 0; (step < state_count) && (link != INVALID); ++step) {
      vec_push_back(sc.chain, link);
      sc.in_chain[link] = 1;
      link = enclosing[link];
    }
    auto const pick = [&sc](uint32_t st) {
      sc.pick[st / 64U] |= uint64_t{ 1 } << (st % 64U);
    };
    for (uint32_t const st : loose) { pick(st); }
    for (uint32_t const a : sc.chain) {
      for (uint32_t k{ kid_head[a] }; k != INVALID; k = kid_next[k]) { pick(k); }
    }
    auto const shields = [&](uint32_t st) {
      uint32_t const up{ enclosing[st] };
      if (up == INVALID) { return false; }
      return (sc.in_chain[up] == 0) && overlaps(region, z.state[up]);
    };
    for (uint32_t w = 0; w < sc.pick.size(); ++w) {
      uint64_t bits{ sc.pick[w] };
      sc.pick[w] = 0;
      while (bits != 0) {
        uint32_t const st{ (w * 64U) + static_cast<uint32_t>(std::countr_zero(bits)) };
        bits &= bits - 1;
        if ((c.states[st].live == 0) || !overlaps(region, z.state[st])) { continue; }
        if ((sc.in_chain[st] != 0) || shields(st)) { continue; }
        sc.obstacle_index[st] = static_cast<uint32_t>(in.obstacles.size());
        vec_push_back(sc.obstacle_states, st);
        vec_push_back(in.obstacles, z.state[st]);
        vec_push_back(in.inscribed, kind_inscribed(c.states[st].kind) ? 1U : 0U);
        vec_push_back(in.corner,
                      state_corner_radius(c.states[st].kind,
                                          z.state[st],
                                          z.before[st].x - z.state[st].x));
      }
    }
    // The bands and loop rooms of the states the frame lies inside are walls inside them.
    for (uint32_t const a : sc.chain) {
      sc.in_chain[a] = 0;
      for (scav_rect const &band : bands_of(z, a)) {
        if ((band.w > 0) && (band.h > 0) && overlaps(region, band)) {
          vec_push_back(in.obstacles, band);
          vec_push_back(in.inscribed, 0U);
          vec_push_back(in.corner, 0);
        }
      }
    }
    // The state the frame's routes are drawn inside.
    in.enclosure = (owner.v == INVALID) ? scav_rect{} : z.state[owner.v];
    for (uint32_t const i : by_frame[m]) {
      Planned const &pn{ planned[i] };
      RouteNet net{ .src = pn.src, .dst = pn.dst };
      if (pn.src_state != INVALID) { net.src_obstacle = sc.obstacle_index[pn.src_state]; }
      if (pn.dst_state != INVALID) { net.dst_obstacle = sc.obstacle_index[pn.dst_state]; }
      net.waypoint_off = static_cast<uint32_t>(in.waypoints.size());
      for (uint32_t const bend : seg_bends[pn.seg]) {
        vec_push_back(in.waypoints, z.node[bend]);
      }

      net.waypoint_len = static_cast<uint32_t>(in.waypoints.size()) - net.waypoint_off;
      net.src_face = pn.face;
      net.dst_face = pn.face;
      if (!faces[0].empty()) {
        net.src_face = (faces[0][pn.seg] != INVALID) ? faces[0][pn.seg] : pn.face;
        net.dst_face = (faces[1][pn.seg] != INVALID) ? faces[1][pn.seg] : pn.face;
      }
      net.loop = pn.loop;
      if (pn.seg < z.lean.size()) { net.lean = z.lean[pn.seg]; }
      trace_emit({ .kind = TraceKind::NetPlanned,
                   .net = { .seg = pn.seg,
                            .trans = g.segments[pn.seg].trans.v,
                            .waypoints = net.waypoint_len,
                            .sx = net.src.x,
                            .sy = net.src.y,
                            .dx = net.dst.x,
                            .dy = net.dst.y } });
      for (uint32_t w = 0; w < net.waypoint_len; ++w) {
        scav_point const &at{ in.waypoints[net.waypoint_off + w] };
        trace_emit({ .kind = TraceKind::NetWaypoint, .point = { .x = at.x, .y = at.y } });
      }
      vec_push_back(in.nets, net);
      if (fill != nullptr) {
        auto const at{ static_cast<uint32_t>(in.nets.size() - 1) };
        for (uint32_t end = 0; end < 2; ++end) {
          fill->faceable[(size_t{ 2 } * pn.seg) + end] =
              static_cast<uint8_t>(router.effective_faces(in, at, end));
        }
      }
    }
    scav_rect const frame{ (owner.v == INVALID) ? region : z.state[owner.v] };

    // A frame whose question only moved is answered by moving its answer. The
    // comparison costs one walk of an input the gather above already built.
    int32_t dx{ 0 };
    int32_t dy{ 0 };
    if ((reuse != nullptr) && (m < reuse->frame.size()) && (reuse->frame[m].valid != 0) &&
        same_but_shifted(reuse->frame[m], in, frame, dx, dy)) {
      RouteFrameCache const &had{ reuse->frame[m] };
      std::vector<scav_point> &moved{ frames[m].points };
      vec_resize(moved, had.points.size());
      for (size_t k = 0; k < moved.size(); ++k) {
        moved[k] = { .x = had.points[k].x + dx, .y = had.points[k].y + dy };
      }
      vec_assign(frames[m].net_points, had.net_points.begin(), had.net_points.end());
      vec_assign(frames[m].metrics, had.metrics.begin(), had.metrics.end());
      frames[m].nudged = had.nudged;
      if (fill != nullptr) {
        RouteFrameCache &to{ fill->frame[m] };
        to.valid = had.valid;
        to.frame = frame;
        to.in = in;
        to.points = moved;
        to.net_points = had.net_points;
        to.metrics = had.metrics;
        to.nudged = had.nudged;
      }
      for (uint32_t const st : sc.obstacle_states) { sc.obstacle_index[st] = INVALID; }
      return;
    }

    router.route(in, ro);

    // Nudged per frame, while the frame's obstacles are in hand.
    if (margin > 0) {
      // The pitch is a line of text, not the router's clearance (11.9.5), and
      // it is the grouping tolerance too, so what is spread apart by it is
      // exactly what was too close by it.
      vec_assign(sc.own, ro.net_points.size(), frame);
      nudge_lanes(region,
                  sc.own,
                  in.obstacles,
                  imax(margin, p.font_size_grid),
                  margin,
                  ro.net_points,
                  ro.points,
                  frames[m].nudged);
    }

    frames[m].points = ro.points;
    frames[m].net_points = ro.net_points;
    frames[m].metrics = ro.metrics;
    if (fill != nullptr) {
      fill->frame[m] = { .valid = 1,
                         .frame = frame,
                         .in = in,
                         .points = ro.points,
                         .net_points = ro.net_points,
                         .metrics = ro.metrics,
                         .nudged = frames[m].nudged };
    }
    for (uint32_t const st : sc.obstacle_states) { sc.obstacle_index[st] = INVALID; }
  };

  uint32_t const shards{ layout_shard_count(c) };
  auto body = [&](uint32_t shard) {
    scav_span const mine{
      shard_range(shard, shards, static_cast<uint32_t>(by_frame.size()))
    };
    if (mine.len == 0) { return; }
    FrameScratch &sc{ frame_scratch() };
    sc.in.profile = p;
    vec_assign(sc.obstacle_index, c.states.size(), INVALID);
    vec_assign(sc.in_chain, c.states.size(), 0);
    vec_assign(sc.pick, (size_t{ state_count } + 63) / 64, 0);
    for (uint32_t k = 0; k < mine.len; ++k) {
      uint32_t const m{ mine.off + k };
      if (!by_frame[m].empty()) { route_frame(m, sc); }
    }
  };
  parallel_for(shards, threads, body);

  // Merged in frame order, which is what makes the totals and the point array
  // the same at every worker count (6).
  std::vector<scav_point> &routed{ cs.routed };
  routed.clear();
  std::vector<scav_span> &net_span{ cs.net_span };
  vec_assign(net_span, planned.size(), scav_span{});
  for (uint32_t m = 0; m < by_frame.size(); ++m) {
    FrameRoutes const &fr{ frames[m] };
    merge_nudged(out.nudged, fr.nudged);
    for (uint32_t j = 0; j < by_frame[m].size(); ++j) {
      scav_span const at{ (j < fr.net_points.size()) ? fr.net_points[j] : scav_span{} };
      if (j < fr.metrics.size()) {
        out.reseated += static_cast<uint32_t>(fr.metrics[j].reseated);
        if (fr.metrics[j].failed != RouteFailure::None) {
          out.failed[g.segments[planned[by_frame[m][j]].seg].trans.v] = 1;
          trace_emit({ .kind = TraceKind::RouteDegraded,
                       .frame = m,
                       .seg = { .seg = planned[by_frame[m][j]].seg } });
        }
        switch (fr.metrics[j].failed) {
          case RouteFailure::OutsideRegion: ++out.outside_region; break;
          case RouteFailure::Unreachable: ++out.unreachable; break;
          case RouteFailure::TooLarge: ++out.too_large; break;
          case RouteFailure::None: break;
        }
      }
      uint32_t const off{ static_cast<uint32_t>(routed.size()) };
      for (uint32_t k = 0; k < at.len; ++k) {
        vec_push_back(routed, fr.points[at.off + k]);
      }
      net_span[by_frame[m][j]] = { .off = off, .len = at.len };
    }
  }

  // Laid end to end: consecutive nets share the endpoint the planner handed
  // both of them, so a net that begins on the point already laid down drops
  // it. Matched against that point rather than against the net's ordinal, so a
  // router that began somewhere else leaves the break in the polyline instead
  // of having a leg spliced over it.
  vec_reserve(out.points, routed.size());
  for (uint32_t t = 0; t < n; ++t) {
    Span const nets{ trans_nets[t] };
    scav_span const loop{ loop_span[t] };
    if ((nets.len == 0) && (loop.len == 0)) { continue; }
    uint32_t const first_point{ static_cast<uint32_t>(out.points.size()) };
    for (uint32_t k = 0; k < loop.len; ++k) {
      vec_push_back(out.points, loop_points[loop.off + k]);
    }
    for (uint32_t j = 0; j < nets.len; ++j) {
      scav_span const at{ net_span[nets.off + j] };
      if (at.len == 0) { continue; }
      bool const joined{ (out.points.size() > first_point) &&
                         same(out.points.back(), routed[at.off]) };
      for (uint32_t k = (joined ? 1U : 0U); k < at.len; ++k) {
        vec_push_back(out.points, routed[at.off + k]);
      }
    }
    uint32_t const count{ static_cast<uint32_t>(out.points.size()) - first_point };
    if ((s.path_clear != nullptr) && (t < s.n_path_clear) && (count >= 2)) {
      scav_point *const pts{ out.points.data() + first_point };
      pts[0] = trim(pts[0], pts[1], s.path_clear[t].src);
      pts[count - 1] = trim(pts[count - 1], pts[count - 2], s.path_clear[t].dst);
    }
    out.route[t] = { .off = first_point, .len = count };
  }

  // **One pass over the composed polylines, because a lane is what a reader
  // sees and a reader sees neither frames nor segments** (11.10a). Nudging
  // above runs per frame on per-segment nets, so a shared run falls between two
  // stools twice over: a decomposed transition's pieces are routed in different
  // frames, and a piece three points long has no interior segment for a lane to
  // hold. `dock` reads both -- `Charging -> Solid` runs down the line
  // `Solid -> Off` runs up, 1,777 units of it in opposite directions, and the
  // down leg is the last segment of its own three-point net.
  if (margin > 0) {
    std::vector<scav_rect> &walls{ cs.walls };
    walls.clear();
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if ((c.states[st].live == 0) || (z.state[st].w == 0) || (z.state[st].h == 0)) {
        continue;
      }
      vec_push_back(walls, z.state[st]);
      for (scav_rect const &band : bands_of(z, st)) {
        if ((band.w > 0) && (band.h > 0)) { vec_push_back(walls, band); }
      }
    }
    // Each transition is bounded by the innermost state enclosing *both* its
    // ends -- the frame it was routed in -- and by the chart where there is
    // none. A hierarchy-crossing transition is bounded by the ancestor it
    // crosses inside, which is what lets its pieces leave the child frames;
    // one wholly inside a composite may not leave that composite.
    std::vector<scav_rect> &held{ cs.held };
    vec_assign(held, out.route.size(), z.chart);
    std::vector<uint8_t> &up{ cs.up };
    vec_assign(up, c.states.size(), 0);
    auto const above = [&enclosing](StateId of) {
      return (of.v == INVALID) ? INVALID : enclosing[of.v];
    };
    for (uint32_t t = 0; t < out.route.size(); ++t) {
      if (t >= c.transitions.size()) { continue; }
      for (uint32_t a{ above(c.transitions[t].src) }; a != INVALID; a = enclosing[a]) {
        up[a] = 1;
      }
      for (uint32_t b{ above(c.transitions[t].dst) }; b != INVALID; b = enclosing[b]) {
        if (up[b] != 0) {
          held[t] = z.state[b];
          break;
        }
      }
      for (uint32_t a{ above(c.transitions[t].src) }; a != INVALID; a = enclosing[a]) {
        up[a] = 0;
      }
      if (inner_loop(c, t)) { held[t] = z.state[c.transitions[t].src.v]; }
    }
    nudge_lanes(z.chart,
                held,
                walls,
                imax(margin, p.font_size_grid),
                margin,
                out.route,
                out.points,
                out.nudged);
  }

  if (labels) { label_routes(out, c, z, s, p, was); }
  vec_push_back(stack, std::move(cs));
}

void label_routes(Routes &out,
                  Chart const &c,
                  SizedLayout const &z,
                  scav_spaces const &s,
                  scav_profile const &p,
                  Routes const *was) {
  LabelBase const base{ .route = (was != nullptr) ? &was->route : nullptr,
                        .points = (was != nullptr) ? &was->points : nullptr,
                        .placed = (was != nullptr) ? &was->placed : nullptr,
                        .settled = (was != nullptr) ? &was->settled : nullptr };
  out.unplaced = place_labels(c,
                              z,
                              s,
                              out.route,
                              out.points,
                              p,
                              out.placed,
                              out.settled,
                              (was != nullptr) ? &base : nullptr);
}

}  // namespace scav
