// Routing against a hand-written `SizedLayout`, so the polyline and the port
// slots are what is under test rather than whatever sizing produced.

#include "layout/route.h"
#include "layout/tests/pod_eq.h"

#include "core/tests/corpus.h"
#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/label.h"
#include "layout/order.h"
#include "layout/router.h"
#include "layout/size.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_int.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scav;

scav_profile profile() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

// These cases pin the shape the ranks alone produce, so they name the router
// that does nothing else rather than taking whatever index 0 is today.
StraightRouter const STRAIGHT;

SubmachineOrders empty_orders(Chart const &c, SplitGraph const &g) {
  SubmachineOrders o;
  o.sub_nodes.assign(c.submachines.size(), Span{});
  o.sub_edges.assign(c.submachines.size(), Span{});
  o.sub_ranks.assign(c.submachines.size(), 0);
  o.sub_gaps.assign(c.submachines.size(), Span{});
  o.state_node.assign(c.states.size(), INVALID);
  o.seg_node.assign(g.segments.size(), INVALID);
  o.seg_port.assign(g.segments.size(), INVALID);
  return o;
}

SizedLayout blank(Chart const &c, SubmachineOrders const &o) {
  SizedLayout z;
  z.state.assign(c.states.size(), scav_rect{});
  z.before.assign(c.states.size(), scav_rect{});
  z.after.assign(c.states.size(), scav_rect{});
  z.lead.assign(c.states.size(), scav_rect{});
  z.trail.assign(c.states.size(), scav_rect{});
  z.loop.assign(c.states.size(), scav_rect{});
  z.sub.assign(c.submachines.size(), scav_rect{});
  z.node.assign(o.nodes.size(), scav_point{});
  return z;
}

// A chart from source, decomposed, ordered, sized and routed orthogonally at row 0 with
// the search off and `pins` drawn as given.
struct Drawn {
  Chart c;
  SplitGraph g;
  SubmachineOrders o;
  SizedLayout z;
  Routes r;
};

void draw_text(std::string_view text,
               scav_spaces const &s,
               SearchPins const &pins,
               Drawn &out) {
  Loader loader;
  REQUIRE(load_add(loader,
                   reinterpret_cast<scav_byte const *>(text.data()),
                   text.size(),
                   "t.scav"));
  REQUIRE(load_pending(loader).empty());
  std::vector<Diagnostic> diags;
  REQUIRE(load_finish(loader, out.c, diags));
  scav_profile const p{ profile() };
  out.g = decompose(out.c);
  out.o = order_submachines(out.c, out.g, s, p, 0, pins);
  REQUIRE(size_layout(out.c, out.g, out.o, s, p, out.z, diags));
  OrthogonalRouter const ortho;
  out.r = route_transitions(out.c,
                            out.g,
                            out.o,
                            out.z,
                            s,
                            p,
                            ortho,
                            0,
                            nullptr,
                            nullptr,
                            &pins);
}

uint32_t named(Chart const &c, std::string_view name) {
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if (chart_string(c, c.states[st].name) == name) { return st; }
  }
  return INVALID;
}

Wide orient(scav_point a, scav_point b, scav_point c) {
  return ((Wide{ b.x } - a.x) * (Wide{ c.y } - a.y)) -
         ((Wide{ b.y } - a.y) * (Wide{ c.x } - a.x));
}

// Whether `ab` and `cd` cross at a point inside both.
bool crosses(scav_point a, scav_point b, scav_point c, scav_point d) {
  Wide const d1{ orient(a, b, c) };
  Wide const d2{ orient(a, b, d) };
  Wide const d3{ orient(c, d, a) };
  Wide const d4{ orient(c, d, b) };
  if ((d1 == 0) || (d2 == 0) || (d3 == 0) || (d4 == 0)) { return false; }
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

bool strictly_inside(scav_point at, scav_rect const &r) {
  return (at.x > r.x) && (at.x < (r.x + r.w)) && (at.y > r.y) && (at.y < (r.y + r.h));
}

// Whether `ab` has a point strictly inside `r`.
bool enters(scav_point a, scav_point b, scav_rect const &r) {
  scav_point const mid{ .x = a.x + ((b.x - a.x) / 2), .y = a.y + ((b.y - a.y) / 2) };
  if (strictly_inside(a, r) || strictly_inside(b, r) || strictly_inside(mid, r)) {
    return true;
  }
  scav_point const tl{ .x = r.x, .y = r.y };
  scav_point const tr{ .x = r.x + r.w, .y = r.y };
  scav_point const bl{ .x = r.x, .y = r.y + r.h };
  scav_point const br{ .x = r.x + r.w, .y = r.y + r.h };
  return crosses(a, b, tl, tr) || crosses(a, b, bl, br) || crosses(a, b, tl, bl) ||
         crosses(a, b, tr, br);
}

}  // namespace

TEST_CASE("route: a sibling transition is a straight line between two centres") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 60, .w = 100, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 400, .h = 100 };

  Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
  REQUIRE(r.route[0].len == 2);
  CHECK((r.points[0] == scav_point{ .x = 50, .y = 20 }));
  CHECK((r.points[1] == scav_point{ .x = 350, .y = 80 }));
  CHECK(r.port[0].len == 0);
  CHECK(r.slots.empty());
}

TEST_CASE("route: a bend the layering left is a point on the way") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders o{ empty_orders(c, g) };
  o.nodes = { { .kind = OrderKind::Bend, .subject = 0, .rank = 1, .pos = 0 } };
  o.edges = { { .src = 0, .dst = 0, .segment = 0, .reversed = 0 } };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.node[0] = { .x = 250, .y = 200 };

  Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
  REQUIRE(r.route[0].len == 3);
  CHECK((r.points[1] == scav_point{ .x = 250, .y = 200 }));
}

TEST_CASE("route: a reversed chain is walked the way it was authored") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders o{ empty_orders(c, g) };
  o.nodes = { { .kind = OrderKind::Bend, .subject = 0, .rank = 1, .pos = 0 },
              { .kind = OrderKind::Bend, .subject = 0, .rank = 2, .pos = 0 } };
  o.edges = { { .src = 0, .dst = 1, .segment = 0, .reversed = 1 } };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.node[0] = { .x = 150, .y = 10 };  // rank 1
  z.node[1] = { .x = 300, .y = 10 };  // rank 2

  Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
  REQUIRE(r.route[0].len == 4);
  // Ranks climb the acyclic way, so a reversed edge walks them back down.
  CHECK(r.points[1].x == 300);
  CHECK(r.points[2].x == 150);
}

TEST_CASE("route: a crossing puts its slot on the crossed border") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, s, d, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(g.trans_segments[0].len == 2);
  SubmachineOrders o{ empty_orders(c, g) };
  // The exit's boundary node lives in the inner frame, at its trailing edge.
  o.nodes = { { .kind = OrderKind::Boundary, .subject = 0, .rank = 1, .pos = 0 } };
  o.seg_node[0] = 0;
  o.seg_port[0] = 0;
  o.sub_nodes[inner.v] = make_span(0, 1);
  SizedLayout z{ blank(c, o) };
  z.state[comp.v] = { .x = 0, .y = 0, .w = 200, .h = 200 };
  z.state[s.v] = { .x = 20, .y = 60, .w = 100, .h = 40 };
  z.state[d.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 500, .h = 200 };
  z.sub[inner.v] = { .x = 10, .y = 10, .w = 180, .h = 180 };
  z.node[0] = { .x = 190, .y = 80 };  // the frame's trailing edge

  Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
  REQUIRE(r.port[0].len == 1);
  scav_port_slot const slot{ r.slots[0] };
  // The node's height, but the composite's own border, not the frame's.
  CHECK(slot.x == z.state[comp.v].x + z.state[comp.v].w);
  CHECK(slot.y == 80);
  CHECK(slot.side == 1);
  CHECK(slot.boundary_depth == 0);
  REQUIRE(r.route[0].len == 3);
  CHECK((r.points[1] == scav_point{ .x = slot.x, .y = slot.y }));
}

TEST_CASE("route: a route entering a composite leaves its border square, never along it") {
  // The port sits on the composite's border at the height its boundary node
  // came out at, and the state it enters is lower. Nothing in the inner frame
  // blocked the border line itself, so the route went down along it before
  // turning in: a run a reader cannot tell from the border (11.10g).
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, d, s, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(g.trans_segments[0].len == 2);
  SubmachineOrders o{ empty_orders(c, g) };
  // The entry's boundary node, at the inner frame's leading edge.
  o.nodes = { { .kind = OrderKind::Boundary, .subject = 1, .rank = 0, .pos = 0 } };
  o.seg_node[1] = 0;
  o.seg_port[1] = 0;
  o.sub_nodes[inner.v] = make_span(0, 1);
  o.edges = { { .src = 0, .dst = 0, .segment = 1, .reversed = 0 } };
  SizedLayout z{ blank(c, o) };
  z.state[comp.v] = { .x = 0, .y = 0, .w = 400, .h = 400 };
  z.state[s.v] = { .x = 200, .y = 250, .w = 120, .h = 60 };
  z.state[d.v] = { .x = -600, .y = 20, .w = 120, .h = 60 };
  z.sub[root.v] = { .x = -600, .y = 0, .w = 1000, .h = 400 };
  z.sub[inner.v] = { .x = 16, .y = 16, .w = 368, .h = 368 };
  z.node[0] = { .x = 16, .y = 50 };

  OrthogonalRouter const ortho;
  Routes const r{ route_transitions(c, g, o, z, {}, profile(), ortho) };
  REQUIRE(r.failed[0] == 0);
  scav_span const route{ r.route[0] };
  REQUIRE(route.len >= 2);
  REQUIRE(r.port[0].len == 1);
  scav_point const slot{ .x = r.slots[r.port[0].off].x, .y = r.slots[r.port[0].off].y };
  CHECK(slot.x == z.state[comp.v].x);
  for (uint32_t k = 0; (k + 1) < route.len; ++k) {
    CAPTURE(k);
    scav_point const a{ r.points[route.off + k] };
    scav_point const b{ r.points[route.off + k + 1] };
    CHECK_FALSE(along_border(a, b, z.state[comp.v]));
    // The leg out of the slot is square to the border it crosses.
    if (same(a, slot)) { CHECK(a.y == b.y); }
  }
}

TEST_CASE("route: the slot side follows the route's direction, not the packing") {
  // An entering route: the boundary node is a source in the inner frame, so the
  // slot belongs on the composite's leading border.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  build_trans(c, d, s, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(g.trans_segments[0].len == 2);
  uint32_t const enter{ g.trans_segments[0].off + 1 };  // the piece inside `inner`
  SubmachineOrders o{ empty_orders(c, g) };
  o.nodes = { { .kind = OrderKind::Boundary, .subject = enter, .rank = 0, .pos = 0 },
              { .kind = OrderKind::State, .subject = s.v, .rank = 1, .pos = 0 } };
  o.edges = { { .src = 0, .dst = 1, .segment = enter, .reversed = 0 } };
  o.seg_node[enter] = 0;
  o.seg_port[enter] = 0;
  o.sub_nodes[inner.v] = make_span(0, 2);
  SizedLayout z{ blank(c, o) };
  z.state[d.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[comp.v] = { .x = 400, .y = 0, .w = 200, .h = 200 };
  z.state[s.v] = { .x = 480, .y = 60, .w = 100, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 600, .h = 200 };
  z.sub[inner.v] = { .x = 410, .y = 10, .w = 180, .h = 180 };
  // Nowhere near the frame's own origin, which is exactly the case a packed
  // second component produces.
  z.node[0] = { .x = 560, .y = 80 };

  Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
  REQUIRE(r.port[0].len == 1);
  CHECK(r.slots[0].side == 0);
  CHECK(r.slots[0].x == z.state[comp.v].x);
  CHECK(r.slots[0].y == 80);
}

TEST_CASE("route: a port on a cross border puts its slot on the top or bottom border") {
  // The boundary node sits on the inner frame's top or bottom edge, so the slot is on the
  // composite's top or bottom border at the node's x; turned down, a side at its y.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  build_trans(c, d, s, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(g.trans_segments[0].len == 2);
  uint32_t const enter{ g.trans_segments[0].off + 1 };
  for (uint8_t const cross : { uint8_t{ 1 }, uint8_t{ 2 } }) {
    for (bool const down : { false, true }) {
      CAPTURE(static_cast<uint32_t>(cross));
      CAPTURE(down);
      SubmachineOrders o{ empty_orders(c, g) };
      o.nodes = { { .kind = OrderKind::Boundary, .subject = enter, .rank = 0, .pos = 0 },
                  { .kind = OrderKind::State, .subject = s.v, .rank = 0, .pos = 1 } };
      o.edges = { { .src = 0, .dst = 1, .segment = enter, .reversed = 0 } };
      o.seg_node[enter] = 0;
      o.seg_port[enter] = 0;
      o.seg_cross.assign(g.segments.size(), 0);
      o.seg_cross[enter] = cross;
      o.sub_down.assign(c.submachines.size(), 0);
      o.sub_down[inner.v] = down ? 1 : 0;
      o.sub_nodes[inner.v] = make_span(0, 2);
      SizedLayout z{ blank(c, o) };
      z.state[d.v] = { .x = 480, .y = -400, .w = 100, .h = 40 };
      z.state[comp.v] = { .x = 400, .y = 0, .w = 200, .h = 200 };
      z.state[s.v] = { .x = 480, .y = 60, .w = 100, .h = 40 };
      z.sub[root.v] = { .x = 0, .y = -400, .w = 600, .h = 600 };
      z.sub[inner.v] = { .x = 410, .y = 10, .w = 180, .h = 180 };
      z.node[0] = down ? scav_point{ .x = (cross == 1) ? 410 : 590, .y = 80 }
                       : scav_point{ .x = 530, .y = (cross == 1) ? 10 : 190 };

      Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
      REQUIRE(r.port[0].len == 1);
      scav_port_slot const slot{ r.slots[0] };
      scav_rect const box{ z.state[comp.v] };
      if (down) {
        CHECK(slot.side == ((cross == 1) ? 0U : 1U));
        CHECK(slot.x == ((cross == 1) ? box.x : (box.x + box.w)));
        CHECK(slot.y == 80);
      } else {
        CHECK(slot.side == ((cross == 1) ? 2U : 3U));
        CHECK(slot.x == 530);
        CHECK(slot.y == ((cross == 1) ? box.y : (box.y + box.h)));
      }
    }
  }
}

TEST_CASE("route: an internal transition starts on the source's inner face") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  build_trans(c, comp, s, TransKind::Internal, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(g.trans_segments[0].len == 1);
  SubmachineOrders o{ empty_orders(c, g) };
  o.nodes = { { .kind = OrderKind::Boundary, .subject = 0, .rank = 0, .pos = 0 } };
  o.seg_node[0] = 0;  // seg_port stays INVALID: an inner face, not a crossing
  o.sub_nodes[inner.v] = make_span(0, 1);
  SizedLayout z{ blank(c, o) };
  z.state[comp.v] = { .x = 0, .y = 0, .w = 200, .h = 200 };
  z.state[s.v] = { .x = 60, .y = 60, .w = 100, .h = 40 };
  z.sub[inner.v] = { .x = 10, .y = 10, .w = 180, .h = 180 };
  z.node[0] = { .x = 10, .y = 90 };

  Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
  REQUIRE(r.route[0].len == 2);
  // On the composite's own border, level with the boundary node: not its centre.
  CHECK((r.points[0] == scav_point{ .x = 0, .y = 90 }));
  CHECK(r.port[0].len == 0);
}

TEST_CASE("route: an external self-loop leaves its trailing face and returns to it") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 4000, .y = 4000, .w = 1600, .h = 640 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 10000, .h = 10000 };
  scav_profile const p{ profile() };

  OrthogonalRouter const ortho;
  Routes const r{ route_transitions(c, g, o, z, {}, p, ortho) };
  REQUIRE(r.route[0].len == 4);
  scav_point const *const pt{ r.points.data() + r.route[0].off };
  scav_rect const &box{ z.state[a.v] };
  // A C off the right face, its far leg the loop's reach out, its ends a line apart.
  CHECK(pt[0].x == (box.x + box.w));
  CHECK(pt[3].x == (box.x + box.w));
  CHECK(pt[1].x == (box.x + box.w + (2 * p.pad)));
  CHECK(pt[2].x == pt[1].x);
  CHECK((pt[0].y - pt[3].y) * (pt[0].y - pt[3].y) >= (p.node_sep / 3) * (p.node_sep / 3));
  CHECK(r.port[0].len == 0);
  CHECK(r.degraded() == 0);
}

TEST_CASE("route: an internal self-transition loops inside its state's loop room") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::Internal, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  scav_profile const p{ profile() };
  z.state[a.v] = { .x = 0, .y = 0, .w = 1000, .h = 1000 };
  int32_t const row{ loop_row(p, {}).h };
  z.loop[a.v] = { .x = 600, .y = 500, .w = loop_reach(p), .h = row };
  Routes const r{ route_transitions(c, g, o, z, {}, p, STRAIGHT) };
  REQUIRE(r.route[0].len == 4);
  scav_point const *const pt{ r.points.data() + r.route[0].off };
  // Out of the right border and back to it, the far leg at the room's leading edge and
  // both legs centred in the row.
  int32_t const top{ 500 + ((row - loop_lane(p)) / 2) };
  CHECK(top > 500);
  CHECK((pt[0] == scav_point{ .x = 1000, .y = top }));
  CHECK((pt[1] == scav_point{ .x = 600, .y = top }));
  CHECK((pt[2] == scav_point{ .x = 600, .y = top + loop_lane(p) }));
  CHECK((pt[3] == scav_point{ .x = 1000, .y = top + loop_lane(p) }));
  CHECK(r.port[0].len == 0);
}

TEST_CASE("route: clears trim each end toward the other, capped at half") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 0, .h = 0 };
  z.state[b.v] = { .x = 1000, .y = 0, .w = 0, .h = 0 };
  std::vector<scav_path_clear> const clears{ { .src = 30, .dst = 700 } };
  scav_spaces const s{ .path_clear = clears.data(), .n_path_clear = 1 };

  Routes const r{ route_transitions(c, g, o, z, s, profile(), STRAIGHT) };
  REQUIRE(r.route[0].len == 2);
  CHECK(r.points[0].x == 30);
  // The far end is capped at half of what is left after the near end moved,
  // not half the original span: 970 remains, so 485 of the 700 is granted.
  CHECK(r.points[1].x == 515);
}

TEST_CASE("route: a clear against a leg of no length trims nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  // One rect for both, so the straight line between the two centres is a point.
  z.state[a.v] = { .x = 100, .y = 100, .w = 40, .h = 40 };
  z.state[b.v] = { .x = 100, .y = 100, .w = 40, .h = 40 };
  std::vector<scav_path_clear> const clears{ { .src = 30, .dst = 30 } };
  scav_spaces const s{ .path_clear = clears.data(), .n_path_clear = 1 };

  Routes const r{ route_transitions(c, g, o, z, s, profile(), STRAIGHT) };
  REQUIRE(r.route[0].len == 2);
  CHECK((r.points[0] == scav_point{ .x = 120, .y = 120 }));
  CHECK((r.points[1] == scav_point{ .x = 120, .y = 120 }));
}

TEST_CASE("route: a tombstoned state is no obstacle to the frame it sat in") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const gone{ build_state(c, root, "G", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  c.states[gone.v].live = 0;

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 40, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 40, .h = 40 };
  z.state[gone.v] = { .x = 100, .y = -100, .w = 200, .h = 240 };  // right across the way
  z.sub[root.v] = { .x = 0, .y = 0, .w = 440, .h = 40 };
  z.chart = { .x = -100, .y = -200, .w = 700, .h = 500 };

  OrthogonalRouter const orthogonal;
  Routes const r{ route_transitions(c, g, o, z, {}, profile(), orthogonal) };
  CHECK(r.degraded() == 0);
  REQUIRE(r.route[0].len == 2);  // straight through where the tombstone lay
  CHECK(r.points[0].y == 20);
  CHECK(r.points[1].y == 20);
}

TEST_CASE("route: a port with no boundary node falls back on the crossed box's centre") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, s, d, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(g.segments[0].dst_port != INVALID);
  SubmachineOrders o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[comp.v] = { .x = 0, .y = 0, .w = 200, .h = 200 };
  z.state[s.v] = { .x = 20, .y = 60, .w = 100, .h = 40 };
  z.state[d.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 500, .h = 200 };
  z.sub[inner.v] = { .x = 10, .y = 10, .w = 180, .h = 180 };

  SUBCASE("no ordering node behind the port") {
    o.seg_port[0] = 0;  // the port is named, but seg_node stays INVALID
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
    REQUIRE(r.port[0].len == 1);
    CHECK(r.slots[0].x == 100);  // the composite's centre
    CHECK(r.slots[0].y == 100);
    CHECK(r.slots[0].side == 0);
    CHECK(r.slots[0].boundary_depth == 0);
  }
  SUBCASE("no segment behind the port at all") {
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
    REQUIRE(r.port[0].len == 1);
    CHECK(r.slots[0].x == 100);
    CHECK(r.slots[0].y == 100);
  }
}

TEST_CASE("route: a path box centres on its route's middle point") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 100, .w = 100, .h = 40 };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 20, .h = 8, .order = 0 } };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 1 };

  Routes const r{ route_transitions(c, g, o, z, s, profile(), STRAIGHT) };
  REQUIRE(r.placed.size() == 1);
  // The middle of the longest leg, which is the one crossing the boundary
  // phase 1 widened for this box -- not the middle point of the polyline.
  scav_span const at{ r.route[0] };
  Wide longest{ -1 };
  scav_point mid{};
  for (uint32_t k = 0; (k + 1) < at.len; ++k) {
    scav_point const p0{ r.points[at.off + k] };
    scav_point const p1{ r.points[at.off + k + 1] };
    Wide const span{ imax(Wide{ p0.x } - p1.x, Wide{ p1.x } - p0.x) +
                     imax(Wide{ p0.y } - p1.y, Wide{ p1.y } - p0.y) };
    if (span > longest) {
      longest = span;
      mid = { .x = p0.x + ((p1.x - p0.x) / 2), .y = p0.y + ((p1.y - p0.y) / 2) };
    }
  }
  CHECK((r.placed[0] == scav_rect{ .x = mid.x - 10, .y = mid.y - 4, .w = 20, .h = 8 }));
}

TEST_CASE("route: a transition to an enclosing state ends on that state's inner face") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "O", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  build_trans(c, s, outer, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(g.trans_segments[0].len == 1);
  REQUIRE(g.segments[0].src_inner == 0);
  REQUIRE(g.segments[0].dst_inner == 1);
  SubmachineOrders o{ empty_orders(c, g) };
  o.nodes = { { .kind = OrderKind::State, .subject = s.v, .rank = 0, .pos = 0 },
              { .kind = OrderKind::Boundary, .subject = 0, .rank = 1, .pos = 0 } };
  o.edges = { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } };
  o.state_node[s.v] = 0;
  o.seg_node[0] = 1;  // seg_port stays INVALID: an inner face, not a crossing
  o.sub_nodes[inner.v] = make_span(0, 2);
  SizedLayout z{ blank(c, o) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.state[s.v] = { .x = 40, .y = 60, .w = 100, .h = 40 };
  z.sub[inner.v] = { .x = 10, .y = 10, .w = 380, .h = 180 };
  z.chart = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.node[1] = { .x = 390, .y = 80 };  // Outer's inner face, at the frame's trailing edge

  OrthogonalRouter const orthogonal;
  std::vector<Router const *> const routers{ &STRAIGHT, &orthogonal };
  for (Router const *router : routers) {
    CAPTURE(router->name().bytes);
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), *router) };
    REQUIRE(r.route[0].len >= 2);
    // The head is the source's own box, not the boundary node the target end
    // put in this same frame; the straight router takes the centre it was
    // handed and the orthogonal one slides it onto the border.
    scav_point const head{ r.points[r.route[0].off] };
    CHECK(head.x >= z.state[s.v].x);
    CHECK(head.x <= (z.state[s.v].x + z.state[s.v].w));
    CHECK(head.y >= z.state[s.v].y);
    CHECK(head.y <= (z.state[s.v].y + z.state[s.v].h));
    // The tail is on Outer's own border level with the boundary node, and no border
    // is crossed, so there is no slot.
    CHECK((r.points[(r.route[0].off + r.route[0].len) - 1] ==
           scav_point{ .x = 400, .y = 80 }));
    CHECK(r.port[0].len == 0);
    CHECK(r.degraded() == 0);
  }
}

namespace {

// One leg per net, from `net.src` to `net.dst`. A router built to break the
// contract starts a step off `net.src` instead, which is the shape phase 3's
// end-to-end join must leave visible.
class ScriptedRouter final : public Router {
 public:
  explicit ScriptedRouter(bool honour) : honour_src{ honour } {}
  [[nodiscard]] RouterName name() const override {
    return { .bytes = "scripted", .len = 8 };
  }
  [[nodiscard]] uint32_t version() const override { return 1; }
  void route(RouteInput const &in, RouteOutput &out) const override {
    out.points.clear();
    out.net_points.clear();
    out.metrics.clear();
    for (RouteNet const &net : in.nets) {
      uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
      out.points.push_back(
          honour_src ? net.src
                     : scav_point{ .x = net.src.x + STRAY, .y = net.src.y + STRAY });
      out.points.push_back(net.dst);
      scav_span const at{ .off = off,
                          .len = static_cast<uint32_t>(out.points.size()) - off };
      out.net_points.push_back(at);
      RouteMetrics m;
      measure(out.points, at, m);
      out.metrics.push_back(m);
    }
  }

  static constexpr int32_t STRAY{ 7 };

 private:
  bool honour_src;
};

}  // namespace

namespace {

// Every net routed as a straight line, with one net's index reported as a named
// failure: the counters and `failed` are what phase 3 makes of that.
class FailingRouter final : public Router {
 public:
  FailingRouter(uint32_t net, RouteFailure cause) : which{ net }, how{ cause } {}
  [[nodiscard]] RouterName name() const override {
    return { .bytes = "failing", .len = 7 };
  }
  [[nodiscard]] uint32_t version() const override { return 1; }
  void route(RouteInput const &in, RouteOutput &out) const override {
    out.points.clear();
    out.net_points.clear();
    out.metrics.clear();
    for (uint32_t n = 0; n < in.nets.size(); ++n) {
      uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
      out.points.push_back(in.nets[n].src);
      out.points.push_back(in.nets[n].dst);
      scav_span const at{ .off = off, .len = 2 };
      out.net_points.push_back(at);
      RouteMetrics m;
      measure(out.points, at, m);
      if (n == which) { m.failed = how; }
      out.metrics.push_back(m);
    }
  }

 private:
  uint32_t which;
  RouteFailure how;
};

// One elbow per net through a shared height, which is the one shape a lane is a
// run of. `margin` is the knob phase 3 reads to decide whether to nudge at all.
class LaneRouter final : public Router {
 public:
  explicit LaneRouter(int32_t want) : wanted{ want } {}
  [[nodiscard]] RouterName name() const override { return { .bytes = "lane", .len = 4 }; }
  [[nodiscard]] uint32_t version() const override { return 1; }
  [[nodiscard]] int32_t margin(scav_profile const & /*p*/) const override {
    return wanted;
  }
  void route(RouteInput const &in, RouteOutput &out) const override {
    out.points.clear();
    out.net_points.clear();
    out.metrics.clear();
    for (RouteNet const &net : in.nets) {
      uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
      out.points.push_back(net.src);
      out.points.push_back({ .x = net.src.x, .y = LANE });
      out.points.push_back({ .x = net.dst.x, .y = LANE });
      out.points.push_back(net.dst);
      scav_span const at{ .off = off, .len = 4 };
      out.net_points.push_back(at);
      RouteMetrics m;
      measure(out.points, at, m);
      out.metrics.push_back(m);
    }
  }

  static constexpr int32_t LANE{ 100 };

 private:
  int32_t wanted;
};

}  // namespace

namespace {

// A router that answers a frame with nothing at all, which is the one shape
// phase 3 cannot read a polyline, a metric or a failure cause out of.
class MuteRouter final : public Router {
 public:
  [[nodiscard]] RouterName name() const override { return { .bytes = "mute", .len = 4 }; }
  [[nodiscard]] uint32_t version() const override { return 1; }
  void route(RouteInput const & /*in*/, RouteOutput &out) const override {
    out.points.clear();
    out.net_points.clear();
    out.metrics.clear();
  }
};

}  // namespace

TEST_CASE("route: a net the router said nothing about leaves no polyline behind") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 0, .w = 100, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 400, .h = 40 };

  MuteRouter const mute;
  Routes const r{ route_transitions(c, g, o, z, {}, profile(), mute) };
  CHECK(r.route[0].len == 0);
  CHECK(r.points.empty());
  // No metric came back either, so nothing is counted as a fallback.
  CHECK(r.degraded() == 0);
  CHECK(r.failed[0] == 0);
}

TEST_CASE("route: the transitions marked failed are the ones with a fallen-back net") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  build_trans(c, d, a, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 200, .y = 0, .w = 100, .h = 40 };
  z.state[d.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 500, .h = 40 };

  SUBCASE("an unreachable end") {
    FailingRouter const failing{ 1, RouteFailure::Unreachable };
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), failing) };
    CHECK(r.failed[0] == 0);
    CHECK(r.failed[1] == 1);
    CHECK(r.failed[2] == 0);
    CHECK(r.unreachable == 1);
    CHECK(r.outside_region == 0);
    CHECK(r.too_large == 0);
    CHECK(r.degraded() == 1);
  }
  SUBCASE("an anchor outside the region") {
    FailingRouter const failing{ 0, RouteFailure::OutsideRegion };
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), failing) };
    CHECK(r.failed[0] == 1);
    CHECK(r.failed[1] == 0);
    CHECK(r.outside_region == 1);
    CHECK(r.unreachable == 0);
  }
  SUBCASE("a graph past the budget") {
    FailingRouter const failing{ 2, RouteFailure::TooLarge };
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), failing) };
    CHECK(r.failed[2] == 1);
    CHECK(r.too_large == 1);
    CHECK(r.degraded() == 1);
  }
  SUBCASE("nothing at all") {
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), STRAIGHT) };
    for (uint8_t const one : r.failed) { CHECK(one == 0); }
    CHECK(r.degraded() == 0);
  }
}

TEST_CASE("route: the unplaced count is the one the strip matching returned") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, a, TransKind::Internal, {});  // its loop room seats its label

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 500, .h = 40 };
  z.chart = { .x = 0, .y = 0, .w = 500, .h = 200 };

  std::vector<scav_path_box> const both{ { .subject = 0, .w = 20, .h = 8, .order = 0 },
                                         { .subject = 1, .w = 20, .h = 8, .order = 0 } };
  scav_spaces const s{ .path_box = both.data(), .n_path_box = 2 };
  Routes const r{ route_transitions(c, g, o, z, s, profile(), STRAIGHT) };
  REQUIRE(r.placed.size() == 2);
  // The loop's label is seated in its room and never counted, which is exactly what
  // the strip matching reports back.
  CHECK(r.unplaced == 0);
  std::vector<scav_rect> expected;
  CHECK(place_labels(c, z, s, r.route, r.points, profile(), expected) == r.unplaced);
}

TEST_CASE("route: nothing is nudged for a router that asks for no margin") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const p{ build_state(c, root, "P", StateKind::Normal, {}) };
  StateId const q{ build_state(c, root, "Q", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, p, q, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 40, .h = 40 };
  z.state[b.v] = { .x = 200, .y = 0, .w = 40, .h = 40 };
  z.state[p.v] = { .x = 0, .y = 200, .w = 40, .h = 40 };
  z.state[q.v] = { .x = 200, .y = 200, .w = 40, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 240, .h = 240 };
  z.chart = { .x = -100, .y = -100, .w = 440, .h = 440 };

  LaneRouter const asks{ 16 };
  Routes const nudged{ route_transitions(c, g, o, z, {}, profile(), asks) };
  // Two: the frame's own pass and the chart-wide one over the composed
  // polylines, which examines the same lane a second time (11.10a).
  CHECK(nudged.nudged.lanes == 2);
  CHECK(nudged.nudged.moved == 2);
  // The root frame has no owning state, so the region is what bounds it, and the
  // region holds every point either net touches. The pitch these two spread by
  // is a line of the profile's text rather than the router's margin, so the
  // 16 asked for above bounds the routing and not the spreading (11.9.5).
  CHECK(nudged.points[nudged.route[0].off + 1].y == 56);
  CHECK(nudged.points[nudged.route[1].off + 1].y == 184);

  LaneRouter const silent{ 0 };
  Routes const plain{ route_transitions(c, g, o, z, {}, profile(), silent) };
  CHECK(plain.nudged.lanes == 0);
  CHECK(plain.nudged.moved == 0);
  // Untouched: both elbows still turn at the height the router put them at.
  for (uint32_t t = 0; t < 2; ++t) {
    CAPTURE(t);
    scav_span const at{ plain.route[t] };
    REQUIRE(at.len == 4);
    CHECK(plain.points[at.off + 1].y == LaneRouter::LANE);
    CHECK(plain.points[at.off + 2].y == LaneRouter::LANE);
  }
}

TEST_CASE("route: a nudge inside a composite is bounded by that state's own box") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  StateId const p{ build_state(c, inner, "P", StateKind::Normal, {}) };
  StateId const q{ build_state(c, inner, "Q", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, p, q, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 40, .h = 40 };
  z.state[b.v] = { .x = 200, .y = 0, .w = 40, .h = 40 };
  z.state[p.v] = { .x = 0, .y = 200, .w = 40, .h = 40 };
  z.state[q.v] = { .x = 200, .y = 200, .w = 40, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 240, .h = 240 };
  z.sub[inner.v] = { .x = 0, .y = 0, .w = 240, .h = 240 };
  z.chart = { .x = -200, .y = -200, .w = 640, .h = 640 };

  LaneRouter const asks{ 16 };
  // A box that leaves the lane all the room it wants: the two members spread by
  // a whole line of text either side.
  z.state[comp.v] = { .x = -40, .y = -40, .w = 320, .h = 320 };
  Routes const wide{ route_transitions(c, g, o, z, {}, profile(), asks) };
  REQUIRE(wide.nudged.lanes == 2);
  CHECK(wide.points[wide.route[0].off + 1].y == 56);
  CHECK(wide.points[wide.route[1].off + 1].y == 184);

  // Eight units of it, centred on the lane, and the members stop one unit inside
  // each border. The region reaches a margin past every point either net
  // touches, so only the owner's box can be doing this.
  z.state[comp.v] = { .x = -40, .y = 96, .w = 320, .h = 8 };
  Routes const tight{ route_transitions(c, g, o, z, {}, profile(), asks) };
  REQUIRE(tight.nudged.lanes == 2);
  CHECK(tight.points[tight.route[0].off + 1].y == 97);
  CHECK(tight.points[tight.route[1].off + 1].y == 103);
}

TEST_CASE("route: nets join only where one ends exactly where the next begins") {
  // One transition out of a composite, so phase 3 has two nets to lay down.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, s, d, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(g.trans_segments[0].len == 2);
  SubmachineOrders o{ empty_orders(c, g) };
  o.nodes = { { .kind = OrderKind::Boundary, .subject = 0, .rank = 1, .pos = 0 } };
  o.seg_node[0] = 0;
  o.seg_port[0] = 0;
  o.sub_nodes[inner.v] = make_span(0, 1);
  SizedLayout z{ blank(c, o) };
  z.state[comp.v] = { .x = 0, .y = 0, .w = 200, .h = 200 };
  z.state[s.v] = { .x = 20, .y = 60, .w = 100, .h = 40 };
  z.state[d.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 500, .h = 200 };
  z.sub[inner.v] = { .x = 10, .y = 10, .w = 180, .h = 180 };
  z.chart = { .x = 0, .y = 0, .w = 500, .h = 200 };
  z.node[0] = { .x = 190, .y = 80 };

  SUBCASE("a net that honours the contract contributes the shared point once") {
    ScriptedRouter const scripted{ true };
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), scripted) };
    REQUIRE(r.port[0].len == 1);
    scav_point const meet{ .x = r.slots[0].x, .y = r.slots[0].y };
    REQUIRE(r.route[0].len == 3);
    CHECK((r.points[0] == scav_point{ .x = 70, .y = 80 }));  // the source's centre
    CHECK((r.points[1] == meet));
    CHECK((r.points[2] == scav_point{ .x = 450, .y = 20 }));  // the target's centre
  }

  SUBCASE("a net that starts elsewhere keeps the point it did start at") {
    // Dropping the second net's first point regardless would splice a leg
    // straight from the slot to the target and hide the break.
    ScriptedRouter const scripted{ false };
    Routes const r{ route_transitions(c, g, o, z, {}, profile(), scripted) };
    REQUIRE(r.port[0].len == 1);
    scav_point const meet{ .x = r.slots[0].x, .y = r.slots[0].y };
    REQUIRE(r.route[0].len == 4);
    CHECK((r.points[1] == meet));
    CHECK((r.points[2] == scav_point{ .x = meet.x + ScriptedRouter::STRAY,
                                      .y = meet.y + ScriptedRouter::STRAY }));
  }
}

namespace {

// Every corpus chart, by the bare name, so the reuse below is held to real
// frames rather than to hand-built ones.
constexpr std::array<char const *, 12> CORPUS{
  "axis.scav", "bottler.scav", "brew.scav", "dock.scav", "estop.scav",       "kiln.scav",
  "led.scav",  "mill.scav",    "ota.scav",  "tcp.scav",  "toolchanger.scav", "vac.scav"
};

void load_corpus_chart(char const *name, Chart &c) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
}

bool same_routes(Routes const &a, Routes const &b) {
  return same_rows(a.points, b.points) && same_rows(a.route, b.route) &&
         same_rows(a.port, b.port) && same_rows(a.slots, b.slots) &&
         same_rows(a.placed, b.placed) && (a.failed == b.failed) &&
         (a.outside_region == b.outside_region) && (a.unreachable == b.unreachable) &&
         (a.too_large == b.too_large) && (a.reseated == b.reseated) &&
         (a.unplaced == b.unplaced);
}

}  // namespace

TEST_CASE("route: a reused frame answers exactly what routing it again would") {
  // 11.10c's whole bet. A Level 1 move leaves every frame but one translated,
  // and a translated frame is answered by translating its answer -- but only if
  // that is the same answer, which is what this pins on real geometry.
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  Router const *const router{ router_at(0) };
  REQUIRE(router != nullptr);

  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Chart c;
    load_corpus_chart(name, c);
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const base_o{ order_submachines(c, g, {}, p, 1, {}) };
    SizedLayout base_z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, base_o, {}, p, base_z, diags));
    RouteCache base;
    Routes const base_r{
      route_transitions(c, g, base_o, base_z, {}, p, *router, 1, nullptr, &base)
    };

    // Every single-state move the chart admits, capped so the suite stays a
    // suite; each one is a fresh layout the cache has never seen.
    uint32_t tried{ 0 };
    for (uint32_t st = 0; (st < c.states.size()) && (tried < 12); ++st) {
      if ((c.states[st].live == 0) || (base_o.state_node[st] == INVALID)) { continue; }
      uint32_t const frame{ c.states[st].parent.v };
      if (frame >= base_o.sub_ranks.size()) { continue; }
      uint32_t const ranks{ base_o.sub_ranks[frame] };
      uint32_t const at{ base_o.nodes[base_o.state_node[st]].rank };
      for (uint32_t r = 0; (r < ranks) && (tried < 12); ++r) {
        if (r == at) { continue; }
        ++tried;
        CAPTURE(st);
        CAPTURE(r);
        SearchPins const pins{ .ranks = { { .state = StateId{ st }, .rank = r } } };
        SubmachineOrders const o{ order_submachines(c, g, {}, p, 1, pins) };
        SizedLayout z;
        std::vector<Diagnostic> spilled;
        if (!size_layout(c, g, o, {}, p, z, spilled)) { continue; }
        Routes const cold{ route_transitions(c, g, o, z, {}, p, *router, 1) };
        Routes const warm{ route_transitions(c, g, o, z, {}, p, *router, 1, &base) };
        CHECK(same_routes(cold, warm));
      }
    }
    CHECK(tried > 0);
    // And the base itself: routing with its own cache is routing it again.
    Routes const again{
      route_transitions(c, g, base_o, base_z, {}, p, *router, 1, &base)
    };
    CHECK(same_routes(base_r, again));
  }
}

TEST_CASE("route: a cache filled by a run that reused one answers like routing afresh") {
  // A chain of moves, each routed through the cache the one before filled.
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  Router const *const router{ router_at(0) };
  REQUIRE(router != nullptr);

  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Chart c;
    load_corpus_chart(name, c);
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const base_o{ order_submachines(c, g, {}, p, 1, {}) };
    SizedLayout base_z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, base_o, {}, p, base_z, diags));
    RouteCache cache;
    route_transitions(c, g, base_o, base_z, {}, p, *router, 1, nullptr, &cache);

    SearchPins pins;
    uint32_t chained{ 0 };
    for (uint32_t st = 0; (st < c.states.size()) && (chained < 6); ++st) {
      if ((c.states[st].live == 0) || (base_o.state_node[st] == INVALID)) { continue; }
      uint32_t const frame{ c.states[st].parent.v };
      if ((frame >= base_o.sub_ranks.size()) || (base_o.sub_ranks[frame] < 2)) {
        continue;
      }
      uint32_t const at{ base_o.nodes[base_o.state_node[st]].rank };
      pins.ranks.push_back({ .state = StateId{ st }, .rank = (at == 0) ? 1U : 0U });
      SubmachineOrders const o{ order_submachines(c, g, {}, p, 1, pins) };
      SizedLayout z;
      std::vector<Diagnostic> spilled;
      if (!size_layout(c, g, o, {}, p, z, spilled)) {
        pins.ranks.pop_back();
        continue;
      }
      CAPTURE(st);
      RouteCache next;
      Routes const warm{ route_transitions(c, g, o, z, {}, p, *router, 1, &cache, &next) };
      Routes const cold{ route_transitions(c, g, o, z, {}, p, *router, 1) };
      CHECK(same_routes(cold, warm));
      cache = std::move(next);
      ++chained;
    }
    CHECK(chained > 0);
  }
}

TEST_CASE("route: a face with no effect at an end changes nothing it draws") {
  // Both kinds of unmarked face occur, at an end the router reads no face at and the face
  // it seats an end on anyway, and some marked face changes the route.
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  Router const *const router{ router_at(0) };
  REQUIRE(router != nullptr);
  uint32_t unread{ 0 };
  uint32_t seated{ 0 };
  uint32_t moved{ 0 };
  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Chart c;
    load_corpus_chart(name, c);
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ order_submachines(c, g, {}, p, 1, {}) };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, o, {}, p, z, diags));
    RouteCache marks;
    Routes const plain{
      route_transitions(c, g, o, z, {}, p, *router, 1, nullptr, &marks)
    };
    REQUIRE(marks.faceable.size() == 2 * g.segments.size());
    for (uint32_t seg = 0; seg < g.segments.size(); ++seg) {
      TransId const t{ g.segments[seg].trans };
      if (t.v == INVALID) { continue; }
      uint32_t const leg{ seg - g.trans_segments[t.v].off };
      for (uint32_t end = 0; end < 2; ++end) {
        uint32_t const mask{ marks.faceable[(2 * seg) + end] };
        for (uint32_t face = 0; face < 4; ++face) {
          SearchPins const pins{
            .faces = { { .trans = t, .leg = leg, .end = end, .face = face } }
          };
          Routes const pinned{
            route_transitions(c, g, o, z, {}, p, *router, 1, nullptr, nullptr, &pins)
          };
          if (((mask >> face) & 1U) == 0) {
            CAPTURE(seg);
            CAPTURE(end);
            CAPTURE(face);
            CHECK(same_routes(plain, pinned));
            ++((mask == 0) ? unread : seated);
          } else if (!same_routes(plain, pinned)) {
            ++moved;
          }
        }
      }
    }
  }
  CHECK(unread > 0);
  CHECK(seated > 0);
  CHECK(moved > 0);
}

TEST_CASE("route: a router that reads no faces marks no end") {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  StraightRouter const straight;
  Chart c;
  load_corpus_chart("tcp.scav", c);
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ order_submachines(c, g, {}, p, 1, {}) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, o, {}, p, z, diags));
  RouteCache marks;
  Routes const r{ route_transitions(c, g, o, z, {}, p, straight, 1, nullptr, &marks) };
  REQUIRE(marks.faceable.size() == 2 * g.segments.size());
  bool none{ true };
  for (uint8_t const f : marks.faceable) { none = none && (f == 0); }
  CHECK(none);
}

namespace {

// A frame's obstacles as a walk over every state finds them: live, overlapping the region,
// not the owner or enclosing it, and not inside an overlapping box that is neither.
struct Gathered {
  std::vector<scav_rect> obstacles;
  std::vector<uint8_t> inscribed;
  std::vector<int32_t> corner;
};

Gathered gather_every_state(Chart const &c,
                            SizedLayout const &z,
                            uint32_t m,
                            scav_rect const &region) {
  Gathered out;
  StateId const owner{ c.submachines[m].owner };
  auto const shields = [&](uint32_t st) {
    StateId const up{ c.submachines[c.states[st].parent.v].owner };
    if (up.v == INVALID) { return false; }
    return !ancestor_or_self(c, up, owner) && overlaps(region, z.state[up.v]);
  };
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if ((c.states[st].live == 0) || !overlaps(region, z.state[st])) { continue; }
    if (ancestor_or_self(c, { st }, owner) || shields(st)) { continue; }
    out.obstacles.push_back(z.state[st]);
    out.inscribed.push_back(kind_inscribed(c.states[st].kind) ? 1U : 0U);
    out.corner.push_back(state_corner_radius(c.states[st].kind,
                                             z.state[st],
                                             z.before[st].x - z.state[st].x));
  }
  return out;
}

// Every frame a filled cache holds, against that walk; how many were compared.
uint32_t frames_gathered_as_every_state(Chart const &c,
                                        SizedLayout const &z,
                                        RouteCache const &cache) {
  uint32_t compared{ 0 };
  for (uint32_t m = 0; m < cache.frame.size(); ++m) {
    RouteFrameCache const &f{ cache.frame[m] };
    if (f.valid == 0) { continue; }
    CAPTURE(m);
    Gathered const want{ gather_every_state(c, z, m, f.in.region) };
    CHECK(same_rows(f.in.obstacles, want.obstacles));
    CHECK((f.in.inscribed == want.inscribed));
    CHECK((f.in.corner == want.corner));
    ++compared;
  }
  return compared;
}

}  // namespace

TEST_CASE("route: a frame's obstacles are the ones a walk over every state finds") {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  Router const *const router{ router_at(0) };
  REQUIRE(router != nullptr);

  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Chart c;
    load_corpus_chart(name, c);
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const base_o{ order_submachines(c, g, {}, p, 1, {}) };
    SizedLayout base_z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, base_o, {}, p, base_z, diags));
    RouteCache base;
    route_transitions(c, g, base_o, base_z, {}, p, *router, 1, nullptr, &base);
    uint32_t compared{ frames_gathered_as_every_state(c, base_z, base) };

    // Moved layouts too, each gathered afresh while the cache answers.
    uint32_t tried{ 0 };
    for (uint32_t st = 0; (st < c.states.size()) && (tried < 4); ++st) {
      if ((c.states[st].live == 0) || (base_o.state_node[st] == INVALID)) { continue; }
      uint32_t const frame{ c.states[st].parent.v };
      if ((frame >= base_o.sub_ranks.size()) || (base_o.sub_ranks[frame] < 2)) {
        continue;
      }
      uint32_t const at{ base_o.nodes[base_o.state_node[st]].rank };
      SearchPins const pins{ .ranks = { { .state = StateId{ st },
                                          .rank = (at == 0) ? 1U : 0U } } };
      SubmachineOrders const o{ order_submachines(c, g, {}, p, 1, pins) };
      SizedLayout z;
      std::vector<Diagnostic> spilled;
      if (!size_layout(c, g, o, {}, p, z, spilled)) { continue; }
      CAPTURE(st);
      RouteCache moved;
      route_transitions(c, g, o, z, {}, p, *router, 1, &base, &moved);
      compared += frames_gathered_as_every_state(c, z, moved);
      ++tried;
    }
    CHECK(compared > 0);
  }
}

TEST_CASE("route: a state outside its composite's box is an obstacle where it lies") {
  // `S` belongs to `A` but lies inside `D`, outside `A`'s box. Built last, it sorts after
  // the frame's own states.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  SubmachineId const in_d{ build_submachine(c, d, {}, {}) };
  StateId const x{ build_state(c, in_d, "X", StateKind::Normal, {}) };
  SubmachineId const in_x{ build_submachine(c, x, {}, {}) };
  StateId const w{ build_state(c, in_x, "W", StateKind::Normal, {}) };
  StateId const y{ build_state(c, in_d, "Y", StateKind::Normal, {}) };
  SubmachineId const in_a{ build_submachine(c, a, {}, {}) };
  StateId const s{ build_state(c, in_a, "S", StateKind::Normal, {}) };
  build_trans(c, x, y, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[d.v] = { .x = 400, .y = 0, .w = 300, .h = 200 };
  z.state[x.v] = { .x = 420, .y = 100, .w = 60, .h = 60 };
  z.state[w.v] = { .x = 430, .y = 110, .w = 20, .h = 20 };  // inside X, so X shields it
  z.state[y.v] = { .x = 620, .y = 100, .w = 60, .h = 60 };
  z.state[s.v] = { .x = 500, .y = 20, .w = 40, .h = 40 };  // inside D, outside A
  z.sub[root.v] = { .x = 0, .y = 0, .w = 700, .h = 200 };
  z.sub[in_d.v] = { .x = 410, .y = 10, .w = 280, .h = 180 };
  z.sub[in_x.v] = { .x = 425, .y = 105, .w = 50, .h = 50 };
  z.sub[in_a.v] = { .x = 10, .y = 10, .w = 80, .h = 80 };
  z.chart = { .x = 0, .y = 0, .w = 700, .h = 200 };

  RouteCache cache;
  route_transitions(c, g, o, z, {}, profile(), STRAIGHT, 1, nullptr, &cache);
  REQUIRE(cache.frame.size() == c.submachines.size());
  RouteFrameCache const &f{ cache.frame[in_d.v] };
  REQUIRE(f.valid != 0);
  REQUIRE(f.in.obstacles.size() == 3);
  CHECK((f.in.obstacles[0] == z.state[x.v]));
  CHECK((f.in.obstacles[1] == z.state[y.v]));
  CHECK((f.in.obstacles[2] == z.state[s.v]));
  CHECK(frames_gathered_as_every_state(c, z, cache) == 1);
}

TEST_CASE("route: a self-transition on a pseudostate loops outside its glyph") {
  // A choice, a junction and a history have no room inside, so each of their internal
  // and local self-transitions is drawn as an external one is.
  Drawn d;
  draw_text(
      "chart pseudo {\n"
      "  state A, state Pick choice, state Hop junction, state Shallow history,\n"
      "  trans * -> A, trans A -> Pick, trans Pick -> Hop, trans Hop -> Shallow,\n"
      "  trans internal Pick -> Pick, trans internal Hop -> Hop,\n"
      "  trans local Shallow -> Shallow,\n"
      "}\n",
      {},
      {},
      d);
  uint32_t loops{ 0 };
  for (uint32_t t = 0; t < d.c.transitions.size(); ++t) {
    Transition const &tr{ d.c.transitions[t] };
    if (tr.src != tr.dst) { continue; }
    CAPTURE(t);
    ++loops;
    CHECK_FALSE(inner_loop(d.c, t));
    CHECK(d.z.loop[tr.src.v].w == 0);
    scav_rect const &box{ d.z.state[tr.src.v] };
    scav_span const route{ d.r.route[t] };
    REQUIRE(route.len >= 4);
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      CHECK_FALSE(enters(d.r.points[route.off + k], d.r.points[route.off + k + 1], box));
    }
  }
  CHECK(loops == 3);
}

TEST_CASE("route: a route inside a state goes round its loop room, never across a leg") {
  // The port Child leaves by sits on S's bottom face, under the room of S's loop: the
  // route to it keeps off the strip between the room and the border the legs cross.
  // The requests are the reference builder's at this profile.
  std::string_view const text{
    "chart roomport {\n"
    "  state S { state Child, trans * -> Child }, state B,\n"
    "  trans * -> S, trans S/Child -> B, trans internal S -> S,\n"
    "}\n"
  };
  Drawn probe;
  draw_text(text, {}, {}, probe);
  std::vector<scav_box_space> rows(probe.c.states.size());
  for (uint32_t st = 0; st < probe.c.states.size(); ++st) {
    if (probe.c.states[st].kind == StateKind::Normal) { rows[st].h_before = 397; }
  }
  rows[named(probe.c, "Child")].min_w = 832;
  std::vector<scav_path_box> const boxes{
    { .subject = 2, .w = 474, .h = 269, .order = 0 },
    { .subject = 3, .w = 1741, .h = 269, .order = 0 }
  };
  scav_spaces const s{ .box_state = rows.data(),
                       .n_box_state = static_cast<uint32_t>(rows.size()),
                       .box_state_stride = static_cast<uint32_t>(sizeof(scav_box_space)),
                       .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()),
                       .path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box)) };
  SearchPins const pins{ .sides = {
                             { .trans = TransId{ 2 }, .leg = 0, .end = 1, .side = 3 } } };
  Drawn d;
  draw_text(text, s, pins, d);
  REQUIRE(inner_loop(d.c, 3));
  scav_span const loop{ d.r.route[3] };
  REQUIRE(loop.len == 4);
  scav_point const *const leg{ d.r.points.data() + loop.off };
  for (uint32_t t = 0; t < 3; ++t) {
    scav_span const route{ d.r.route[t] };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      CAPTURE(t);
      CAPTURE(k);
      scav_point const a{ d.r.points[route.off + k] };
      scav_point const b{ d.r.points[route.off + k + 1] };
      CHECK_FALSE(crosses(a, b, leg[0], leg[1]));
      CHECK_FALSE(crosses(a, b, leg[2], leg[3]));
    }
  }
}

TEST_CASE("route: a nudge leaves an outer loop's corridor and its arrowhead's leg") {
  // The loop and the port route run under S a little apart; the chart-wide nudge spreads
  // them, and the loop's last leg keeps the clear its arrowhead was trimmed by. The
  // requests are the reference builder's at this profile.
  scav_profile const p{ profile() };
  int32_t const head{ (3 * p.pad) / 4 };
  Drawn probe;
  std::string_view const text{
    "chart portloop {\n"
    "  state A, state S { state C, trans * -> C },\n"
    "  trans * -> A, trans A -> S/C, trans S -> S,\n"
    "}\n"
  };
  draw_text(text, {}, {}, probe);
  std::vector<scav_box_space> rows(probe.c.states.size());
  for (uint32_t st = 0; st < probe.c.states.size(); ++st) {
    if (probe.c.states[st].kind == StateKind::Normal) { rows[st].h_before = 397; }
  }
  std::vector<scav_path_box> const boxes{
    { .subject = 2, .w = 359, .h = 269, .order = 0 },
    { .subject = 3, .w = 704, .h = 269, .order = 0 }
  };
  std::vector<scav_path_clear> const clears(probe.c.transitions.size(),
                                            { .src = 0, .dst = head });
  scav_spaces const s{ .box_state = rows.data(),
                       .n_box_state = static_cast<uint32_t>(rows.size()),
                       .box_state_stride = static_cast<uint32_t>(sizeof(scav_box_space)),
                       .path_clear = clears.data(),
                       .n_path_clear = static_cast<uint32_t>(clears.size()),
                       .path_clear_stride = static_cast<uint32_t>(sizeof(scav_path_clear)),
                       .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()),
                       .path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box)) };
  SearchPins const pins{
    .faces = { { .trans = TransId{ 3 }, .leg = 0, .end = 0, .face = 3 },
               { .trans = TransId{ 3 }, .leg = 0, .end = 1, .face = 3 } },
    .sides = { { .trans = TransId{ 2 }, .leg = 1, .end = 0, .side = 3 } }
  };
  Drawn d;
  draw_text(text, s, pins, d);
  scav_rect const &box{ d.z.state[named(d.c, "S")] };
  scav_span const loop{ d.r.route[3] };
  REQUIRE(loop.len == 4);
  scav_point const *const pt{ d.r.points.data() + loop.off };
  CHECK(pt[0].y == (box.y + box.h));
  CHECK((pt[1].y - pt[0].y) >= (2 * p.pad));
  CHECK((pt[2].y - pt[3].y) > head);
}

TEST_CASE("route: a state lined on its trailing face loops out of its leading one") {
  // A trail band and no lead band: the room sits against the leading pad, the loop leaves
  // the left border and returns to it, and its label stands beyond the far leg.
  scav_profile const p{ profile() };
  std::string_view const text{
    "chart lined {\n"
    "  state A, state B,\n"
    "  trans * -> A, trans A -> B, trans internal A -> A,\n"
    "}\n"
  };
  Drawn probe;
  draw_text(text, {}, {}, probe);
  uint32_t const a{ named(probe.c, "A") };
  REQUIRE(a != INVALID);
  std::vector<scav_box_space> rows(probe.c.states.size());
  rows[a].w_after = 3 * p.font_size_grid;
  std::vector<scav_path_box> const boxes{
    { .subject = 2, .w = 4 * p.font_size_grid, .h = p.font_size_grid, .order = 0 }
  };
  scav_spaces const s{ .box_state = rows.data(),
                       .n_box_state = static_cast<uint32_t>(rows.size()),
                       .box_state_stride = static_cast<uint32_t>(sizeof(scav_box_space)),
                       .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()),
                       .path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box)) };
  Drawn d;
  draw_text(text, s, {}, d);
  REQUIRE(inner_loop(d.c, 2));
  scav_rect const &box{ d.z.state[a] };
  scav_rect const &trail{ d.z.trail[a] };
  CHECK(loop_mirrored(d.z, a));
  CHECK(d.z.loop[a].x == d.z.lead[a].x);
  scav_span const loop{ d.r.route[2] };
  REQUIRE(loop.len == 4);
  scav_point const *const pt{ d.r.points.data() + loop.off };
  CHECK(pt[0].x == box.x);
  CHECK(pt[3].x == box.x);
  CHECK(pt[0].y < pt[3].y);
  for (uint32_t k = 1; k < 3; ++k) { CHECK(strictly_inside(pt[k], box)); }
  for (uint32_t k = 0; k < 3; ++k) { CHECK_FALSE(enters(pt[k], pt[k + 1], trail)); }
  REQUIRE(d.r.placed.size() == 1);
  scav_rect const &label{ d.r.placed[0] };
  CHECK(label.x > pt[1].x);
  CHECK((label.x + label.w) <= trail.x);
  CHECK(cost_terms(d.c, d.g, d.z, d.r, s, p).through_band == 0);
}

TEST_CASE("route: an outer loop's face is traced with its segment and transition") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, b, a, TransKind::External, {});
  build_trans(c, a, a, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 4000, .y = 4000, .w = 1600, .h = 640 };
  z.state[b.v] = { .x = 0, .y = 4000, .w = 1600, .h = 640 };
  z.sub[root.v] = { .x = 0, .y = 0, .w = 10000, .h = 10000 };

  LayoutTrace trace;
  trace_sink_set(&trace);
  OrthogonalRouter const ortho;
  Routes const r{ route_transitions(c, g, o, z, {}, profile(), ortho) };
  trace_sink_set(nullptr);
  uint32_t faced{ 0 };
  for (TraceEvent const &e : trace.events) {
    if (e.kind != TraceKind::LoopFaced) { continue; }
    ++faced;
    CHECK(e.port.trans == 1);
    CHECK(e.port.seg == g.trans_segments[1].off);
    CHECK(e.port.side == 1);
  }
  CHECK(faced == 1);
  CHECK(r.degraded() == 0);
}
