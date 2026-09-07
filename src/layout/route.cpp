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
#include "scav/scav_core.h"
#include "scav_int.h"
#include "scav_shard.h"
#include "scav_stable_sort.h"
#include "scav_thread.h"

#include <cstdint>
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
                         uint32_t threads) {
  Routes out;
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
      // The external self-loop: out the trailing side and back.
      scav_rect const r{ z.state[tr.src.v] };
      scav_point const lip{ .x = r.x + r.w, .y = r.y + floor_div(r.h, 2) };
      planned.push_back({ .frame = g.segments[segs.off].frame.v,
                          .src = lip,
                          .dst = { .x = lip.x + (2 * p.pad), .y = lip.y },
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
        planned.push_back({ .frame = g.segments[seg].frame.v,
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
    RouteInput &in{ sc.in };
    RouteOutput &ro{ sc.ro };
    in.obstacles.clear();
    in.inscribed.clear();
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
    }
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
      in.nets.push_back(net);
    }
    router.route(in, ro);

    // Nudged per frame, while the frame's obstacles are in hand.
    if (margin > 0) {
      scav_rect const frame{ (owner.v == INVALID) ? region : z.state[owner.v] };
      nudge_lanes(region,
                  frame,
                  in.obstacles,
                  margin,
                  margin,
                  ro.net_points,
                  ro.points,
                  frames[m].nudged);
    }

    frames[m].points = ro.points;
    frames[m].net_points = ro.net_points;
    frames[m].metrics = ro.metrics;
    for (uint32_t const st : sc.obstacle_states) { sc.obstacle_index[st] = INVALID; }
  };

  uint32_t const shards{ layout_shard_count(c) };
  auto body = [&](uint32_t shard) {
    scav_span const mine{
      shard_range(shard, shards, static_cast<uint32_t>(by_frame.size()))
    };
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

  out.unplaced = place_labels(c, z, s, out.route, out.points, out.placed);
  return out;
}

}  // namespace scav
