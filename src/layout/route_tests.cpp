// Routing against a hand-written `SizedLayout`, so the polyline and the port
// slots are what is under test rather than whatever sizing produced.

#include "layout/route.h"

#include "layout/decompose.h"
#include "layout/label.h"
#include "layout/order.h"
#include "layout/router.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_int.h"

#include "doctest.h"

#include <cstdint>
#include <vector>

namespace {

using namespace scav;

// The C structs carry no operators; the tests compare them field-wise.
constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

constexpr bool operator==(scav_point const &a, scav_point const &b) {
  return (a.x == b.x) && (a.y == b.y);
}

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
  z.sub.assign(c.submachines.size(), scav_rect{});
  z.node.assign(o.nodes.size(), scav_point{});
  return z;
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
  REQUIRE(r.port[0].len == 1);
  CHECK(r.slots[0].side == 0);
  CHECK(r.slots[0].x == z.state[comp.v].x);
  CHECK(r.slots[0].y == 80);
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
  REQUIRE(r.route[0].len == 2);
  CHECK((r.points[0] == scav_point{ .x = 10, .y = 90 }));  // not the composite's centre
  CHECK(r.port[0].len == 0);
}

TEST_CASE("route: an external self-loop leaves and returns, with no slot") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout z{ blank(c, o) };
  z.state[a.v] = { .x = 40, .y = 0, .w = 100, .h = 40 };
  scav_profile const p{ profile() };

  Routes const r{ phase3_route(c, g, o, z, {}, p, STRAIGHT) };
  REQUIRE(r.route[0].len == 2);
  CHECK((r.points[0] == scav_point{ .x = 140, .y = 20 }));
  CHECK((r.points[1] == scav_point{ .x = 140 + (2 * p.pad), .y = 20 }));
  CHECK(r.port[0].len == 0);
}

TEST_CASE("route: an internal self-transition has no route at all") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::Internal, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ empty_orders(c, g) };
  SizedLayout const z{ blank(c, o) };
  Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
  CHECK(r.route[0].len == 0);
  CHECK(r.points.empty());
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

  Routes const r{ phase3_route(c, g, o, z, s, profile(), STRAIGHT) };
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

  Routes const r{ phase3_route(c, g, o, z, s, profile(), STRAIGHT) };
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
  Routes const r{ phase3_route(c, g, o, z, {}, profile(), orthogonal) };
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
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
    REQUIRE(r.port[0].len == 1);
    CHECK(r.slots[0].x == 100);  // the composite's centre
    CHECK(r.slots[0].y == 100);
    CHECK(r.slots[0].side == 0);
    CHECK(r.slots[0].boundary_depth == 0);
  }
  SUBCASE("no segment behind the port at all") {
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
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

  Routes const r{ phase3_route(c, g, o, z, s, profile(), STRAIGHT) };
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
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), *router) };
    REQUIRE(r.route[0].len >= 2);
    // The head is the source's own box, not the boundary node the target end
    // put in this same frame; the straight router takes the centre it was
    // handed and the orthogonal one slides it onto the border.
    scav_point const head{ r.points[r.route[0].off] };
    CHECK(head.x >= z.state[s.v].x);
    CHECK(head.x <= (z.state[s.v].x + z.state[s.v].w));
    CHECK(head.y >= z.state[s.v].y);
    CHECK(head.y <= (z.state[s.v].y + z.state[s.v].h));
    // The tail is the boundary node exactly: no obstacle names that end, so no
    // router moves it, and no border is crossed, so there is no slot.
    CHECK((r.points[(r.route[0].off + r.route[0].len) - 1] ==
           scav_point{ .x = 390, .y = 80 }));
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
  Routes const r{ phase3_route(c, g, o, z, {}, profile(), mute) };
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
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), failing) };
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
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), failing) };
    CHECK(r.failed[0] == 1);
    CHECK(r.failed[1] == 0);
    CHECK(r.outside_region == 1);
    CHECK(r.unreachable == 0);
  }
  SUBCASE("a graph past the budget") {
    FailingRouter const failing{ 2, RouteFailure::TooLarge };
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), failing) };
    CHECK(r.failed[2] == 1);
    CHECK(r.too_large == 1);
    CHECK(r.degraded() == 1);
  }
  SUBCASE("nothing at all") {
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), STRAIGHT) };
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
  build_trans(c, a, a, TransKind::Internal, {});  // no route, so no strip to ride

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
  Routes const r{ phase3_route(c, g, o, z, s, profile(), STRAIGHT) };
  REQUIRE(r.placed.size() == 2);
  // One box rides its route and the other has none, which is exactly what the
  // strip matching reports back.
  std::vector<scav_rect> expected;
  CHECK(place_labels(c, z, s, r.route, r.points, expected) == r.unplaced);
  CHECK(r.unplaced == 1);
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
  Routes const nudged{ phase3_route(c, g, o, z, {}, profile(), asks) };
  CHECK(nudged.nudged.lanes == 1);
  CHECK(nudged.nudged.moved == 2);
  // The root frame has no owning state, so the region is what bounds it, and the
  // region holds every point either net touches.
  CHECK(nudged.points[nudged.route[0].off + 1].y == 92);
  CHECK(nudged.points[nudged.route[1].off + 1].y == 108);

  LaneRouter const silent{ 0 };
  Routes const plain{ phase3_route(c, g, o, z, {}, profile(), silent) };
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
  // the whole margin either side.
  z.state[comp.v] = { .x = -40, .y = -40, .w = 320, .h = 320 };
  Routes const wide{ phase3_route(c, g, o, z, {}, profile(), asks) };
  REQUIRE(wide.nudged.lanes == 1);
  CHECK(wide.points[wide.route[0].off + 1].y == 92);
  CHECK(wide.points[wide.route[1].off + 1].y == 108);

  // Eight units of it, centred on the lane. The region reaches a margin past
  // every point either net touches, so only the owner's box can be doing this.
  z.state[comp.v] = { .x = -40, .y = 96, .w = 320, .h = 8 };
  Routes const tight{ phase3_route(c, g, o, z, {}, profile(), asks) };
  REQUIRE(tight.nudged.lanes == 1);
  CHECK(tight.points[tight.route[0].off + 1].y == 96);
  CHECK(tight.points[tight.route[1].off + 1].y == 104);
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
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), scripted) };
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
    Routes const r{ phase3_route(c, g, o, z, {}, profile(), scripted) };
    REQUIRE(r.port[0].len == 1);
    scav_point const meet{ .x = r.slots[0].x, .y = r.slots[0].y };
    REQUIRE(r.route[0].len == 4);
    CHECK((r.points[1] == meet));
    CHECK((r.points[2] == scav_point{ .x = meet.x + ScriptedRouter::STRAY,
                                      .y = meet.y + ScriptedRouter::STRAY }));
  }
}
