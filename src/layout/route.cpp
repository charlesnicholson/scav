// A transition's polyline is its segments' chains laid end to end: the
// bends the layering put between two ranks, and a port slot wherever the
// route leaves one box's border for the next.

#include "layout/route.h"

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav_int.h"
#include "scav_stable_sort.h"

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

}  // namespace

Routes phase3_route(Chart const &c,
                    SplitGraph const &g,
                    SubmachineOrders const &o,
                    SizedLayout const &z,
                    scav_spaces const &s,
                    scav_profile const &p) {
  Routes out;
  uint32_t const n{ static_cast<uint32_t>(c.transitions.size()) };
  out.route.assign(n, {});
  out.port.assign(n, {});

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

  // A slot sits on the crossed box's own border, at the height its boundary
  // node ended up: which border follows from whether the node was placed at
  // its frame's leading or trailing edge.
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
    scav_rect const frame{ z.sub[g.segments[seg].frame.v] };
    bool const leading{ z.node[node].x <= frame.x };
    return scav_port_slot{ .x = leading ? box.x : (box.x + box.w),
                           .y = z.node[node].y,
                           .side = leading ? 0U : 1U,
                           .boundary_depth = depth };
  };

  for (uint32_t t = 0; t < n; ++t) {
    Span const segs{ g.trans_segments[t] };
    if (segs.len == 0) { continue; }
    Transition const &tr{ c.transitions[t] };
    uint32_t const first_point{ static_cast<uint32_t>(out.points.size()) };
    uint32_t const first_slot{ static_cast<uint32_t>(out.slots.size()) };

    if (tr.src == tr.dst) {
      // The external self-loop: out the trailing side and back.
      scav_rect const r{ z.state[tr.src.v] };
      scav_point const lip{ .x = r.x + r.w, .y = r.y + floor_div(r.h, 2) };
      out.points.push_back(lip);
      out.points.push_back({ .x = lip.x + (2 * p.pad), .y = lip.y });
    } else {
      // A source whose own border is not crossed starts on that border's
      // inner face, which is where its boundary node was placed (11.14).
      uint32_t const head{ o.seg_node[segs.off] };
      if ((g.segments[segs.off].src_port == INVALID) && (head != INVALID) &&
          (o.seg_port[segs.off] == INVALID)) {
        out.points.push_back(z.node[head]);
      } else {
        out.points.push_back(centre(z.state[tr.src.v]));
      }
      for (uint32_t k = 0; k < segs.len; ++k) {
        uint32_t const seg{ segs.off + k };
        for (uint32_t const bend : seg_bends[seg]) { out.points.push_back(z.node[bend]); }
        uint32_t const port{ g.segments[seg].dst_port };
        if (port == INVALID) { continue; }
        scav_port_slot const slot{ slot_of(port) };
        out.slots.push_back(slot);
        out.points.push_back({ .x = slot.x, .y = slot.y });
      }
      out.points.push_back(centre(z.state[tr.dst.v]));
    }

    if ((s.path_clear != nullptr) && (t < s.n_path_clear)) {
      scav_point *const pts{ out.points.data() + first_point };
      uint32_t const count{ static_cast<uint32_t>(out.points.size()) - first_point };
      pts[0] = trim(pts[0], pts[1], s.path_clear[t].src);
      pts[count - 1] = trim(pts[count - 1], pts[count - 2], s.path_clear[t].dst);
    }
    out.route[t] = { .off = first_point,
                     .len = static_cast<uint32_t>(out.points.size()) - first_point };
    out.port[t] = { .off = first_slot,
                    .len = static_cast<uint32_t>(out.slots.size()) - first_slot };
  }

  out.placed.assign(s.n_path_box, {});
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    scav_path_box const &box{ s.path_box[i] };
    scav_span const route{ (box.subject < n) ? out.route[box.subject] : scav_span{} };
    scav_point const mid{ (route.len == 0) ? scav_point{}
                                           : out.points[route.off + (route.len / 2)] };
    out.placed[i] = { .x = mid.x - floor_div(box.w, 2),
                      .y = mid.y - floor_div(box.h, 2),
                      .w = box.w,
                      .h = box.h };
  }
  return out;
}

}  // namespace scav
