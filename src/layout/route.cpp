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

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace scav {

namespace {

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
        (p.src_face != q.src_face) || (p.dst_face != q.dst_face) ||
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

// Allocated once per shard and reused across that shard's frames.
struct FrameScratch {
  RouteInput in;
  RouteOutput ro;
  std::vector<uint32_t> obstacle_index;  // -> in.obstacles; INVALID off this frame
  std::vector<uint32_t> obstacle_states;
};

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
                         SearchPins const *pins) {
  Routes out;
  // `{trans, leg, end}` resolved to a face per segment end once, so the frame
  // workers read a flat table rather than searching the pin list per net.
  std::array<std::vector<uint32_t>, 2> faces;
  if ((pins != nullptr) && !pins->faces.empty()) {
    for (std::vector<uint32_t> &side : faces) { side.assign(g.segments.size(), INVALID); }
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
  if (fill != nullptr) { fill->frame.assign(c.submachines.size(), {}); }
  uint32_t const n{ static_cast<uint32_t>(c.transitions.size()) };
  out.route.assign(n, {});
  out.port.assign(n, {});
  out.failed.assign(n, 0);

  // The segment each port's boundary node belongs to, and the bends each
  // segment was chained through, both gathered once.
  std::vector<uint32_t> port_seg(g.ports.size(), INVALID);
  for (uint32_t seg = 0; seg < g.segments.size(); ++seg) {
    if (o.seg_port[seg] != INVALID) { port_seg[o.seg_port[seg]] = seg; }
  }
  std::vector<uint32_t> seg_reversed(g.segments.size(), 0);
  for (OrderEdge const &e : o.edges) { seg_reversed[e.segment] = e.reversed; }
  std::vector<std::vector<uint32_t>> seg_bends(g.segments.size());
  for (uint32_t node = 0; node < o.nodes.size(); ++node) {
    if (o.nodes[node].kind == OrderKind::Bend) {
      seg_bends[o.nodes[node].subject].push_back(node);
    }
  }
  for (uint32_t seg = 0; seg < seg_bends.size(); ++seg) {
    std::vector<uint32_t> &chain{ seg_bends[seg] };
    scav_stable_sort(chain, [&](uint32_t a, uint32_t b) {
      return o.nodes[a].rank < o.nodes[b].rank;
    });
    // Ranks climb in the acyclic direction, which is the authored one only
    // when the segment's edge was not reversed to break a cycle.
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
  std::vector<uint8_t> source_node(o.nodes.size(), 0);
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
    return scav_port_slot{ .x = leading ? box.x : (box.x + box.w),
                           .y = z.node[node].y,
                           .side = leading ? 0U : 1U,
                           .boundary_depth = depth };
  };

  // Plan every net before routing any, so the port slots come out in
  // transition order however the frames are then visited.
  std::vector<Planned> planned;
  std::vector<Span> trans_nets(n, Span{});
  for (uint32_t t = 0; t < n; ++t) {
    Span const segs{ g.trans_segments[t] };
    if (segs.len == 0) { continue; }
    Transition const &tr{ c.transitions[t] };
    uint32_t const first_net{ static_cast<uint32_t>(planned.size()) };
    uint32_t const first_slot{ static_cast<uint32_t>(out.slots.size()) };

    if (tr.src == tr.dst) {
      // The external self-loop: out the trailing side and back, and no further
      // than the state it is drawn inside lets it, which keeps a route off
      // that state's border (11.10g).
      scav_rect const r{ z.state[tr.src.v] };
      scav_point const lip{ .x = r.x + r.w, .y = r.y + floor_div(r.h, 2) };
      uint32_t const frame{ g.segments[segs.off].frame.v };
      StateId const around{ (frame < c.submachines.size()) ? c.submachines[frame].owner
                                                           : StateId{ INVALID } };
      int32_t reach{ lip.x + (2 * p.pad) };
      if (around.v != INVALID) {
        scav_rect const box{ z.state[around.v] };
        reach = imax(imin(reach, (box.x + box.w) - imax(p.pad / 2, 1) - 1), lip.x + 1);
      }
      planned.push_back({ .frame = frame,
                          .src = lip,
                          .dst = { .x = reach, .y = lip.y },
                          .src_state = INVALID,
                          .dst_state = INVALID,
                          .seg = segs.off });
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
          out.slots.push_back(slot);
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
        planned.push_back({ .frame = frame,
                            .src = at,
                            .dst = end,
                            .src_state = at_state,
                            .dst_state = end_state,
                            .seg = seg });
        at = end;
        at_state = end_state;
      }
    }
    trans_nets[t] =
        make_span(first_net, static_cast<uint32_t>(planned.size()) - first_net);
    out.port[t] = { .off = first_slot,
                    .len = static_cast<uint32_t>(out.slots.size()) - first_slot };
  }

  // One batch per frame, in submachine order. A frame's nets keep the order
  // they were planned in, which is `(transition, ordinal)`.
  std::vector<std::vector<uint32_t>> by_frame(c.submachines.size());
  for (uint32_t i = 0; i < planned.size(); ++i) {
    if (planned[i].frame < by_frame.size()) { by_frame[planned[i].frame].push_back(i); }
  }

  int32_t const margin{ router.margin(p) };
  std::vector<FrameRoutes> frames(by_frame.size());

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
    }

    // Exactly what this router asked for, not a guess: a margin larger than it
    // needs is canvas nobody draws in, smaller leaves a frame-edge box no lane.
    region.x -= margin;
    region.y -= margin;
    region.w += 2 * margin;
    region.h += 2 * margin;
    in.region = region;

    // Every live box overlapping the region except those enclosing it, and of those
    // only the outermost -- a box already blocks its own descendants (11.14).
    StateId const owner{ c.submachines[m].owner };
    auto const shields = [&](uint32_t st) {
      StateId const up{ c.submachines[c.states[st].parent.v].owner };
      if (up.v == INVALID) { return false; }
      return !ancestor_or_self(c, up, owner) && overlaps(region, z.state[up.v]);
    };
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if ((c.states[st].live == 0) || !overlaps(region, z.state[st])) { continue; }
      if (ancestor_or_self(c, { st }, owner) || shields(st)) { continue; }
      sc.obstacle_index[st] = static_cast<uint32_t>(in.obstacles.size());
      sc.obstacle_states.push_back(st);
      in.obstacles.push_back(z.state[st]);
      in.inscribed.push_back(kind_inscribed(c.states[st].kind) ? 1U : 0U);
      in.corner.push_back(state_corner_radius(c.states[st].kind,
                                              z.state[st],
                                              z.before[st].x - z.state[st].x));
    }
    // The state the frame's routes are drawn inside, so none runs along its
    // border and a port on it leaves square (11.10g).
    in.enclosure = (owner.v == INVALID) ? scav_rect{} : z.state[owner.v];
    for (uint32_t const i : by_frame[m]) {
      Planned const &pn{ planned[i] };
      RouteNet net{ .src = pn.src, .dst = pn.dst };
      if (pn.src_state != INVALID) { net.src_obstacle = sc.obstacle_index[pn.src_state]; }
      if (pn.dst_state != INVALID) { net.dst_obstacle = sc.obstacle_index[pn.dst_state]; }
      net.waypoint_off = static_cast<uint32_t>(in.waypoints.size());
      for (uint32_t const bend : seg_bends[pn.seg]) {
        in.waypoints.push_back(z.node[bend]);
      }
      net.waypoint_len = static_cast<uint32_t>(in.waypoints.size()) - net.waypoint_off;
      if (!faces[0].empty()) {
        net.src_face = faces[0][pn.seg];
        net.dst_face = faces[1][pn.seg];
      }
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
      in.nets.push_back(net);
    }
    scav_rect const frame{ (owner.v == INVALID) ? region : z.state[owner.v] };

    // A frame whose question only moved is answered by moving its answer. The
    // comparison costs one walk of an input the gather above already built.
    int32_t dx{ 0 };
    int32_t dy{ 0 };
    if ((reuse != nullptr) && (m < reuse->frame.size()) && (reuse->frame[m].valid != 0) &&
        same_but_shifted(reuse->frame[m], in, frame, dx, dy)) {
      RouteFrameCache const &had{ reuse->frame[m] };
      frames[m].points = had.points;
      for (scav_point &q : frames[m].points) {
        q.x += dx;
        q.y += dy;
      }
      frames[m].net_points = had.net_points;
      frames[m].metrics = had.metrics;
      frames[m].nudged = had.nudged;
      if (fill != nullptr) {
        fill->frame[m] = had;
        fill->frame[m].in = in;
        fill->frame[m].frame = frame;
        fill->frame[m].points = frames[m].points;
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
      std::vector<scav_rect> const own(ro.net_points.size(), frame);
      nudge_lanes(region,
                  own,
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
      fill->frame[m] = { .in = in,
                         .frame = frame,
                         .points = ro.points,
                         .net_points = ro.net_points,
                         .metrics = ro.metrics,
                         .nudged = frames[m].nudged,
                         .valid = 1 };
    }
    for (uint32_t const st : sc.obstacle_states) { sc.obstacle_index[st] = INVALID; }
  };

  uint32_t const shards{ layout_shard_count(c) };
  auto body = [&](uint32_t shard) {
    scav_span const mine{
      shard_range(shard, shards, static_cast<uint32_t>(by_frame.size()))
    };
    if (mine.len == 0) { return; }
    FrameScratch sc;
    sc.in.profile = p;
    sc.obstacle_index.assign(c.states.size(), INVALID);
    for (uint32_t k = 0; k < mine.len; ++k) {
      uint32_t const m{ mine.off + k };
      if (!by_frame[m].empty()) { route_frame(m, sc); }
    }
  };
  parallel_for(shards, threads, body);

  // Merged in frame order, which is what makes the totals and the point array
  // the same at every worker count (6).
  std::vector<scav_point> routed;
  std::vector<scav_span> net_span(planned.size(), scav_span{});
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
      for (uint32_t k = 0; k < at.len; ++k) { routed.push_back(fr.points[at.off + k]); }
      net_span[by_frame[m][j]] = { .off = off, .len = at.len };
    }
  }

  // Laid end to end: consecutive nets share the endpoint the planner handed
  // both of them, so a net that begins on the point already laid down drops
  // it. Matched against that point rather than against the net's ordinal, so a
  // router that began somewhere else leaves the break in the polyline instead
  // of having a leg spliced over it.
  for (uint32_t t = 0; t < n; ++t) {
    Span const nets{ trans_nets[t] };
    if (nets.len == 0) { continue; }
    uint32_t const first_point{ static_cast<uint32_t>(out.points.size()) };
    for (uint32_t j = 0; j < nets.len; ++j) {
      scav_span const at{ net_span[nets.off + j] };
      if (at.len == 0) { continue; }
      bool const joined{ (out.points.size() > first_point) &&
                         same(out.points.back(), routed[at.off]) };
      for (uint32_t k = (joined ? 1U : 0U); k < at.len; ++k) {
        out.points.push_back(routed[at.off + k]);
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
    std::vector<scav_rect> walls;
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if ((c.states[st].live != 0) && (z.state[st].w != 0) && (z.state[st].h != 0)) {
        walls.push_back(z.state[st]);
      }
    }
    // Each transition is bounded by the innermost state enclosing *both* its
    // ends -- the frame it was routed in -- and by the chart where there is
    // none. A hierarchy-crossing transition is bounded by the ancestor it
    // crosses inside, which is what lets its pieces leave the child frames;
    // one wholly inside a composite may not leave that composite.
    std::vector<scav_rect> held(out.route.size(), z.chart);
    std::vector<uint8_t> up(c.states.size(), 0);
    for (uint32_t t = 0; t < out.route.size(); ++t) {
      if (t >= c.transitions.size()) { continue; }
      for (StateId a{ enclosing_state(c, c.transitions[t].src) }; a.v != INVALID;
           a = enclosing_state(c, a)) {
        up[a.v] = 1;
      }
      for (StateId b{ enclosing_state(c, c.transitions[t].dst) }; b.v != INVALID;
           b = enclosing_state(c, b)) {
        if (up[b.v] != 0) {
          held[t] = z.state[b.v];
          break;
        }
      }
      for (StateId a{ enclosing_state(c, c.transitions[t].src) }; a.v != INVALID;
           a = enclosing_state(c, a)) {
        up[a.v] = 0;
      }
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

  out.unplaced = place_labels(c, z, s, out.route, out.points, p, out.placed);
  return out;
}

}  // namespace scav
