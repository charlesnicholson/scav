// Routing against a hand-written `SizedLayout`: rects and node positions
// typed straight into the test, so the polyline and the port slots are what
// is under test rather than whatever sizing produced.

#include "layout/route.h"

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"

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

  Routes const r{ phase3_route(c, g, o, z, {}, profile()) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile()) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile()) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile()) };
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
  // An entering route: the boundary node is a source in the inner frame, so
  // the slot belongs on the composite's leading border. Sizing places such a
  // node at its *component's* leading edge and then the packer offsets the
  // whole component, so the node's absolute x says nothing about which border
  // it is on -- a frame with more than one component puts it anywhere.
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile()) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, profile()) };
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

  Routes const r{ phase3_route(c, g, o, z, {}, p) };
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
  Routes const r{ phase3_route(c, g, o, z, {}, profile()) };
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

  Routes const r{ phase3_route(c, g, o, z, s, profile()) };
  REQUIRE(r.route[0].len == 2);
  CHECK(r.points[0].x == 30);
  // The far end is capped at half of what is left after the near end moved,
  // not half the original span: 970 remains, so 485 of the 700 is granted.
  CHECK(r.points[1].x == 515);
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

  Routes const r{ phase3_route(c, g, o, z, s, profile()) };
  REQUIRE(r.placed.size() == 1);
  scav_point const mid{ r.points[r.route[0].off + (r.route[0].len / 2)] };
  CHECK((r.placed[0] == scav_rect{ .x = mid.x - 10, .y = mid.y - 4, .w = 20, .h = 8 }));
}
