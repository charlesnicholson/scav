// Ordering against hand-written charts and hand-written position lists: the
// inversion count on its own, then ranks, boundary nodes, bend chains, gap
// widening, and the sweep that removes a crossing.

#include "layout/decompose.h"
#include "layout/order.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"

#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace scav;

scav_profile profile() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

SubmachineOrders order_of(Chart const &c, scav_spaces const &s = {}) {
  return order_submachines(c, decompose(c), s, profile());
}

// The nodes of one frame, which `order_submachines` emits contiguously.
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
  // 500 for the label, in one frame and not both, which is the property. The
  // 538 on top of it is 11.9.5's lane reservation, two lines of `readable`
  // text on the one boundary where two edges turn.
  CHECK(total == 1038);
  std::vector<int32_t> per;
  for (Span const &frame : o.sub_gaps) {
    int32_t most{ 0 };
    for (uint32_t k = 0; k < frame.len; ++k) { most = imax(most, o.gaps[frame.off + k]); }
    per.push_back(most);
  }
  CHECK(per == std::vector<int32_t>{ 500, 538 });
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

TEST_CASE("order: widened separations order to the same rows") {
  // What lets `layout_run` order once and reuse the result on every
  // spacing-inflation attempt: the three separations reach no decision here.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const deep{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId prev{ build_state(c, root, "S0", StateKind::Normal, {}) };
  for (uint32_t i = 1; i < 9; ++i) {
    StateId const next{ build_state(c, root, "S", StateKind::Normal, {}) };
    build_trans(c, prev, next, TransKind::External, {});
    if ((i % 4) == 0) { build_trans(c, next, prev, TransKind::External, {}); }
    prev = next;
  }
  build_trans(c, prev, deep, TransKind::External, {});  // crosses a boundary

  scav_path_box const box{ .subject = 0, .w = 300, .h = 40, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };

  scav_profile const p{ profile() };
  scav_profile wider{ p };
  wider.rank_sep += 8 * p.spacing_inflation_increment;
  wider.node_sep += 8 * p.spacing_inflation_increment;
  wider.sub_sep += 8 * p.spacing_inflation_increment;
  REQUIRE(profile_validate(wider));

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const a{ order_submachines(c, g, s, p) };
  SubmachineOrders const b{ order_submachines(c, g, s, wider) };
  CHECK(a.nodes == b.nodes);
  CHECK(a.edges == b.edges);
  CHECK(a.sub_nodes == b.sub_nodes);
  CHECK(a.sub_edges == b.sub_edges);
  CHECK(a.sub_ranks == b.sub_ranks);
  CHECK(a.sub_gaps == b.sub_gaps);
  CHECK(a.gaps == b.gaps);
  CHECK(a.state_node == b.state_node);
  CHECK(a.seg_node == b.seg_node);
  CHECK(a.seg_port == b.seg_port);
}

TEST_CASE("order: a pin that asks for the rank a node already has changes nothing") {
  // The spine 11.10a's placement move stands on. A pin is a re-derivation and
  // not an edit, so pinning what longest path already chose has to be the
  // identity -- if it is not, the move is not measuring the move.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const plain{ order_submachines(c, g, {}, profile()) };
  SearchPins same;
  for (StateId const st : { a, b, d }) {
    same.ranks.push_back({ .state = st, .rank = plain.nodes[plain.state_node[st.v]].rank });
  }
  SubmachineOrders const pinned{ order_submachines(c, g, {}, profile(), 0, same) };
  CHECK(pinned.nodes == plain.nodes);
  CHECK(pinned.edges == plain.edges);
  CHECK(pinned.gaps == plain.gaps);
  CHECK(pinned.sub_ranks == plain.sub_ranks);
}

TEST_CASE("order: a pin moves a node's rank and the ranks stay contiguous") {
  // A -> B -> D ranks 0, 1, 2. Pinning D onto B's rank empties rank 2, and a
  // rank nothing sits in would still be sized a gap in phase 2 (11.10), so the
  // squeeze renumbers onto the ranks that kept a node.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const plain{ order_submachines(c, g, {}, profile()) };
  CHECK(plain.sub_ranks[root.v] == 3);
  CHECK(plain.nodes[plain.state_node[d.v]].rank == 2);

  SearchPins const onto_one{ .ranks = { { .state = d, .rank = 1 } } };
  SubmachineOrders const moved{ order_submachines(c, g, {}, profile(), 0, onto_one) };
  CHECK(moved.nodes[moved.state_node[d.v]].rank == 1);
  CHECK(moved.sub_ranks[root.v] == 2);
  // Every rank below the top holds at least one node.
  std::vector<uint32_t> held(moved.sub_ranks[root.v], 0);
  Span const sp{ moved.sub_nodes[root.v] };
  for (uint32_t i = 0; i < sp.len; ++i) { ++held[moved.nodes[sp.off + i].rank]; }
  for (uint32_t const n : held) { CHECK(n > 0); }
}

TEST_CASE("order: undoing a move is running with the pins one held before it") {
  // What makes a search loop possible at all: a move is not a mutation, so
  // there is nothing to roll back and no state to get wrong. Applying the
  // ranks a run produced reproduces that run exactly.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  build_trans(c, a, d, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const plain{ order_submachines(c, g, {}, profile()) };
  SearchPins before;
  for (StateId const st : { a, b, d }) {
    before.ranks.push_back(
        { .state = st, .rank = plain.nodes[plain.state_node[st.v]].rank });
  }
  SearchPins const away{ .ranks = { { .state = d, .rank = 1 } } };

  SubmachineOrders const gone{ order_submachines(c, g, {}, profile(), 0, away) };
  CHECK(gone.nodes != plain.nodes);  // the move did something
  SubmachineOrders const back{ order_submachines(c, g, {}, profile(), 0, before) };
  CHECK(back.nodes == plain.nodes);
  CHECK(back.edges == plain.edges);
  CHECK(back.gaps == plain.gaps);
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

// 11.10b: a cut drops a segment's bends, so phase 3 gets a net with no
// waypoints. The chart is estop's shape -- a three-state cycle whose back edge
// spans two ranks after cycle-breaking.
namespace {

struct Cycle {
  Chart c;
  SplitGraph g;
  StateId a{ INVALID }, b{ INVALID }, d{ INVALID };
  TransId back{ INVALID };
};

Cycle three_state_cycle() {
  Cycle out;
  SubmachineId const root{ build_chart(out.c, "t", {}) };
  out.a = build_state(out.c, root, "A", StateKind::Normal, {});
  out.b = build_state(out.c, root, "B", StateKind::Normal, {});
  out.d = build_state(out.c, root, "C", StateKind::Normal, {});
  build_trans(out.c, out.a, out.b, TransKind::External, {});
  build_trans(out.c, out.b, out.d, TransKind::External, {});
  out.back = build_trans(out.c, out.d, out.a, TransKind::External, {});
  out.g = decompose(out.c);
  return out;
}

uint32_t bends_of(SubmachineOrders const &o) {
  uint32_t n{ 0 };
  for (OrderNode const &nd : o.nodes) {
    n += (nd.kind == OrderKind::Bend) ? 1U : 0U;
  }
  return n;
}

// The cut naming a transition's only leg.
SearchPins cut_of(Cycle const &z, uint32_t leg = 0) {
  return { .cuts = { { .trans = z.back, .leg = leg } } };
}

}  // namespace

TEST_CASE("order: the back edge of a cycle chains, and a cut leaves it long") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  REQUIRE(bends_of(plain) == 1);

  SubmachineOrders const freed{
    order_submachines(z.c, z.g, {}, profile(), 0, cut_of(z))
  };
  CHECK(bends_of(freed) == 0);

  // The edge survives, spanning more than one rank -- which is the whole point:
  // it reaches the router as one net with no waypoint between its ends.
  uint32_t spanning{ 0 };
  for (OrderEdge const &e : freed.edges) {
    uint32_t const from{ freed.nodes[e.src].rank };
    uint32_t const to{ freed.nodes[e.dst].rank };
    spanning += ((to - from) > 1) ? 1U : 0U;
  }
  CHECK(spanning == 1);
  CHECK(freed.edges.size() == (plain.edges.size() - 1));  // the chain was two
}

TEST_CASE("order: a cut is undone by dropping it, and the orders come back") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  SubmachineOrders const freed{
    order_submachines(z.c, z.g, {}, profile(), 0, cut_of(z))
  };
  REQUIRE(freed.nodes != plain.nodes);  // the cut did something
  SubmachineOrders const back{ order_submachines(z.c, z.g, {}, profile(), 0, {}) };
  CHECK(back.nodes == plain.nodes);
  CHECK(back.edges == plain.edges);
}

TEST_CASE("order: a cut naming nothing this chart has is ignored, not applied") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  // A leg the transition does not have, a transition past the end, and INVALID.
  // Every one of them must leave the orders exactly as they were.
  for (SearchPins const &pins : { cut_of(z, 1),
                                  cut_of(z, 99),
                                  SearchPins{ .cuts = { { .trans = TransId{ 4096 },
                                                          .leg = 0 } } },
                                  SearchPins{ .cuts = { {} } } }) {
    SubmachineOrders const same{ order_submachines(z.c, z.g, {}, profile(), 0, pins) };
    CHECK(same.nodes == plain.nodes);
    CHECK(same.edges == plain.edges);
  }
}

TEST_CASE("order: cutting an edge that never chained changes nothing") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  // Transition 0 is A -> B, adjacent ranks, so it has no bend to drop.
  SearchPins const pins{ .cuts = { { .trans = TransId{ 0 }, .leg = 0 } } };
  SubmachineOrders const same{ order_submachines(z.c, z.g, {}, profile(), 0, pins) };
  CHECK(same.nodes == plain.nodes);
  CHECK(same.edges == plain.edges);
}

TEST_CASE("order: cuts and rank pins compose, and neither disables the other") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  uint32_t const was{ plain.nodes[plain.state_node[z.d.v]].rank };

  SearchPins both{ cut_of(z) };
  both.ranks.push_back({ .state = z.d, .rank = 0 });
  SubmachineOrders const o{ order_submachines(z.c, z.g, {}, profile(), 0, both) };
  CHECK(bends_of(o) == 0);                              // the cut held
  CHECK(o.nodes[o.state_node[z.d.v]].rank != was);      // and so did the pin
  CHECK(o.nodes[o.state_node[z.d.v]].rank == 0);
}

TEST_CASE("order: a reversal pin turns the named edge, and the walk turns no other") {
  // 11.10d: which edge of a cycle is turned around decides everything
  // downstream -- ranks, what spans two of them, what carries a bend -- and
  // cycle-breaking chose it by walking in node order, which is declaration
  // order. The pin names the edge instead.
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  auto const reversed_segs = [](SubmachineOrders const &o) {
    std::vector<uint32_t> segs;
    for (OrderEdge const &e : o.edges) {
      if ((e.reversed != 0) &&
          (std::find(segs.begin(), segs.end(), e.segment) == segs.end())) {
        segs.push_back(e.segment);
      }
    }
    return segs;
  };
  // Left alone, the walk turns the edge that closes it: the back edge.
  CHECK(reversed_segs(plain) == std::vector<uint32_t>{ 2 });

  // Pinned, it is the pinned one and nothing else -- the walk finds the cycle
  // already broken. A bare three-cycle still leaves one edge spanning two
  // ranks whichever is turned, so the bend count does not move; what moves is
  // *which* edge carries it, and on a chart with an entry state that is the
  // difference between a bend and none (11.10d).
  for (uint32_t t = 0; t < 3; ++t) {
    CAPTURE(t);
    SearchPins const pin{ .reverses = { { .trans = TransId{ t }, .leg = 0 } } };
    SubmachineOrders const turned{ order_submachines(z.c, z.g, {}, profile(), 0, pin) };
    CHECK(reversed_segs(turned) == std::vector<uint32_t>{ t });
    CHECK(bends_of(turned) == 1);
    if (t != 2) { CHECK(turned.nodes != plain.nodes); }
  }
}

TEST_CASE("order: a reversal pin naming nothing this chart has is ignored") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  for (SearchPins const &pins :
       { SearchPins{ .reverses = { { .trans = TransId{ 4096 }, .leg = 0 } } },
         SearchPins{ .reverses = { { .trans = z.back, .leg = 7 } } },
         SearchPins{ .reverses = { {} } } }) {
    SubmachineOrders const same{ order_submachines(z.c, z.g, {}, profile(), 0, pins) };
    CHECK(same.nodes == plain.nodes);
    CHECK(same.edges == plain.edges);
  }
}

TEST_CASE("order: turning an edge the walk would turn anyway changes nothing") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  SearchPins const pin{ .reverses = { { .trans = z.back, .leg = 0 } } };
  SubmachineOrders const same{ order_submachines(z.c, z.g, {}, profile(), 0, pin) };
  CHECK(same.nodes == plain.nodes);
  CHECK(same.edges == plain.edges);
}

