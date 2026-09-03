// Ordering against hand-written charts and hand-written position lists: the
// inversion count on its own, then ranks, boundary nodes, bend chains, gap
// widening, and the sweep that removes a crossing.

#include "layout/decompose.h"
#include "layout/order.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <cstdint>
#include <vector>

namespace {

using namespace scav;

scav_profile profile() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

SubmachineOrders order_of(Chart const &c, scav_spaces const &s = {}) {
  return phase1_order(c, decompose(c), s, profile());
}

// The nodes of one frame, which `phase1_order` emits contiguously.
std::vector<OrderNode> frame_nodes(SubmachineOrders const &o, SubmachineId m) {
  Span const sp{ o.sub_nodes[m.v] };
  return { o.nodes.begin() + sp.off, o.nodes.begin() + sp.off + sp.len };
}

OrderNode const &node_of(SubmachineOrders const &o, StateId s) {
  REQUIRE(o.state_node[s.v] != INVALID);
  return o.nodes[o.state_node[s.v]];
}

uint32_t count_kind(std::vector<OrderNode> const &nodes, OrderKind kind) {
  uint32_t n{ 0 };
  for (OrderNode const &nd : nodes) {
    if (nd.kind == kind) { ++n; }
  }
  return n;
}

}  // namespace

TEST_CASE("crossings: inversions over a hand-written position list") {
  CHECK(rank_crossings({}) == 0);
  CHECK(rank_crossings({ 3 }) == 0);
  CHECK(rank_crossings({ 0, 1, 2, 3 }) == 0);
  CHECK(rank_crossings({ 1, 0 }) == 1);
  CHECK(rank_crossings({ 3, 2, 1, 0 }) == 6);
  // Equal south positions share an endpoint, so they cannot cross each other.
  CHECK(rank_crossings({ 2, 2, 2 }) == 0);
  CHECK(rank_crossings({ 1, 1, 0 }) == 2);
}

TEST_CASE("order: a chain ranks one state per layer") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});

  SubmachineOrders const o{ order_of(c) };
  CHECK(o.sub_ranks[root.v] == 3);
  CHECK(node_of(o, a).rank == 0);
  CHECK(node_of(o, b).rank == 1);
  CHECK(node_of(o, d).rank == 2);
  CHECK(node_of(o, a).pos == 0);
  CHECK(o.sub_edges[root.v].len == 2);
  CHECK(o.gaps.size() == 2);
}

TEST_CASE("order: unconnected siblings share one rank in document order") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };

  SubmachineOrders const o{ order_of(c) };
  CHECK(o.sub_ranks[root.v] == 1);
  CHECK(node_of(o, a).rank == 0);
  CHECK(node_of(o, b).rank == 0);
  CHECK(node_of(o, a).pos == 0);
  CHECK(node_of(o, b).pos == 1);
  CHECK(o.gaps.empty());
}

TEST_CASE("order: a cycle reverses exactly one edge and still ranks") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, a, TransKind::External, {});

  SubmachineOrders const o{ order_of(c) };
  CHECK(o.sub_ranks[root.v] == 2);
  CHECK(node_of(o, a).rank == 0);
  CHECK(node_of(o, b).rank == 1);
  uint32_t reversed{ 0 };
  for (OrderEdge const &e : o.edges) { reversed += e.reversed; }
  CHECK(reversed == 1);
  // Both edges point the DAG way after orientation, whatever they were authored as.
  for (OrderEdge const &e : o.edges) { CHECK(o.nodes[e.src].rank < o.nodes[e.dst].rank); }
}

TEST_CASE("order: an external self-loop contributes no edge") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::External, {});

  SubmachineOrders const o{ order_of(c) };
  CHECK(o.sub_ranks[root.v] == 1);
  CHECK(o.sub_edges[root.v].len == 0);
}

TEST_CASE("order: a boundary node stands for the port on the frame's own border") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, s, d, TransKind::External, {});

  SubmachineOrders const o{ order_of(c) };
  std::vector<OrderNode> const in{ frame_nodes(o, inner) };
  REQUIRE(in.size() == 2);
  CHECK(count_kind(in, OrderKind::State) == 1);
  CHECK(count_kind(in, OrderKind::Boundary) == 1);
  // The exit leaves the frame, so its boundary sits on the last rank: S left,
  // the port right.
  CHECK(node_of(o, s).rank == 0);
  CHECK(o.sub_ranks[inner.v] == 2);
  for (OrderNode const &nd : in) {
    if (nd.kind == OrderKind::Boundary) { CHECK(nd.rank == 1); }
  }

  // Seen from the root, the same port is the composite itself, so the outer
  // segment is an ordinary edge between two children.
  CHECK(frame_nodes(o, root).size() == 2);
  CHECK(node_of(o, comp).rank == 0);
  CHECK(node_of(o, d).rank == 1);
}

TEST_CASE("order: an internal transition into a descendant anchors on the source border") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  build_trans(c, comp, s, TransKind::Internal, {});

  SubmachineOrders const o{ order_of(c) };
  std::vector<OrderNode> const in{ frame_nodes(o, inner) };
  REQUIRE(in.size() == 2);
  CHECK(count_kind(in, OrderKind::Boundary) == 1);
  // Nothing inside points at the boundary, so it is a source and stays left.
  for (OrderNode const &nd : in) {
    CHECK(nd.rank == ((nd.kind == OrderKind::Boundary) ? 0U : 1U));
  }
  CHECK(frame_nodes(o, root).size() == 1);
}

TEST_CASE("order: an edge spanning two ranks is chained through a bend") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  build_trans(c, a, d, TransKind::External, {});

  SubmachineOrders const o{ order_of(c) };
  REQUIRE(o.sub_ranks[root.v] == 3);
  std::vector<OrderNode> const nodes{ frame_nodes(o, root) };
  CHECK(count_kind(nodes, OrderKind::Bend) == 1);
  for (OrderNode const &nd : nodes) {
    if (nd.kind == OrderKind::Bend) { CHECK(nd.rank == 1); }
  }
  // Every emitted edge spans exactly one rank once the chain exists.
  for (OrderEdge const &e : o.edges) {
    CHECK((o.nodes[e.dst].rank - o.nodes[e.src].rank) == 1);
  }
  CHECK(o.sub_edges[root.v].len == 4);
}

TEST_CASE("order: a path box widens the rank boundary its label crosses") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});

  scav_path_box const box{ .subject = 1, .w = 700, .h = 40, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  SubmachineOrders const o{ order_of(c, s) };
  REQUIRE(o.gaps.size() == 2);
  CHECK(o.gaps[0] == 0);
  CHECK(o.gaps[1] == 700);
}

TEST_CASE("order: a label on a hierarchy-crossing route widens one frame only") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const s2{ build_state(c, inner, "S2", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, s, s2, TransKind::External, {});
  build_trans(c, s, d, TransKind::External, {});

  scav_path_box const box{ .subject = 1, .w = 500, .h = 40, .order = 0 };
  scav_spaces const s3{ .path_box = &box, .n_path_box = 1 };
  SubmachineOrders const o{ order_of(c, s3) };
  int32_t total{ 0 };
  for (int32_t const gap : o.gaps) { total += gap; }
  CHECK(total == 500);
}

TEST_CASE("order: a sweep removes a crossing document order would have left") {
  // Two sources and two sinks wired across, so the authored order crosses and
  // the median sweep has somewhere better to put them.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a1{ build_state(c, root, "A1", StateKind::Normal, {}) };
  StateId const a2{ build_state(c, root, "A2", StateKind::Normal, {}) };
  StateId const b1{ build_state(c, root, "B1", StateKind::Normal, {}) };
  StateId const b2{ build_state(c, root, "B2", StateKind::Normal, {}) };
  build_trans(c, a1, b2, TransKind::External, {});
  build_trans(c, a2, b1, TransKind::External, {});

  SubmachineOrders const o{ order_of(c) };
  REQUIRE(o.sub_ranks[root.v] == 2);
  // Zero crossings means the two sinks ended up under their own sources.
  std::vector<uint32_t> south;
  south.reserve(o.edges.size());
  for (OrderEdge const &e : o.edges) { south.push_back(o.nodes[e.dst].pos); }
  CHECK(rank_crossings(south) == 0);
  CHECK(node_of(o, b2).pos == node_of(o, a1).pos);
  CHECK(node_of(o, b1).pos == node_of(o, a2).pos);
}

TEST_CASE("order: two runs over one chart agree row for row") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId prev{ build_state(c, root, "S0", StateKind::Normal, {}) };
  for (uint32_t i = 1; i < 12; ++i) {
    StateId const next{ build_state(c, root, "S", StateKind::Normal, {}) };
    build_trans(c, prev, next, TransKind::External, {});
    if ((i % 3) == 0) { build_trans(c, next, prev, TransKind::External, {}); }
    prev = next;
  }

  SubmachineOrders const a{ order_of(c) };
  SubmachineOrders const b{ order_of(c) };
  CHECK(a.nodes == b.nodes);
  CHECK(a.edges == b.edges);
  CHECK(a.gaps == b.gaps);
  CHECK(a.state_node == b.state_node);
  CHECK(a.seg_node == b.seg_node);
}

TEST_CASE("order: a dead submachine gets an empty span and no nodes") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, a, {}, {}) };
  build_state(c, inner, "S", StateKind::Normal, {});
  c.submachines[inner.v].live = 0;

  SubmachineOrders const o{ order_of(c) };
  CHECK(o.sub_nodes[inner.v].len == 0);
  CHECK(o.sub_ranks[inner.v] == 0);
  CHECK(frame_nodes(o, root).size() == 1);
}
