// Ordering tests on hand-written charts and position lists: inversion count, ranks,
// boundary nodes, bend chains, gap widening, and the crossing sweep.

#include "core/tests/corpus.h"
#include "layout/decompose.h"
#include "layout/order.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"
#include "scav_thread.h"

#include "doctest.h"

#include <algorithm>
#include <array>
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
  // Equal south positions share an endpoint and count no crossing.
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
  // After orientation every edge runs from a lower rank to a higher one.
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

  // In the root the port is the composite itself, so the outer segment joins two
  // children.
  CHECK(frame_nodes(o, root).size() == 2);
  CHECK(node_of(o, comp).rank == 0);
  CHECK(node_of(o, d).rank == 1);
}

TEST_CASE(
    "order: a port on a cross border shares its neighbour's rank, at one end of it") {
  // An end pin puts the port on the top or bottom border. The sweep swaps `B` and `Y` to
  // uncross `A -> Y` and `Q -> B`, and the port stays at one end of `B`'s rank.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const q{ build_state(c, inner, "Q", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  StateId const y{ build_state(c, inner, "Y", StateKind::Normal, {}) };
  build_trans(c, a, y, TransKind::External, {});
  build_trans(c, q, b, TransKind::External, {});
  TransId const drop{ build_trans(c, d, b, TransKind::External, {}) };
  SplitGraph const g{ decompose(c) };
  REQUIRE(g.trans_segments[drop.v].len == 2);
  uint32_t const seg{ g.trans_segments[drop.v].off + 1 };
  REQUIRE(g.segments[seg].frame == inner);

  // Unpinned, the port is a source on the leading border, a rank before `B`.
  SubmachineOrders const plain{ order_submachines(c, g, {}, profile()) };
  CHECK(plain.seg_cross[seg] == 0);
  CHECK(plain.seg_side[seg] == 0);
  CHECK((plain.nodes[plain.seg_node[seg]].rank + 1) == node_of(plain, b).rank);

  for (uint32_t const side : { 2U, 3U }) {
    CAPTURE(side);
    SearchPins const pin{ .ends = { { .trans = drop, .leg = 1, .face = side } } };
    SubmachineOrders const o{ order_submachines(c, g, {}, profile(), 0, pin) };
    CHECK(o.seg_cross[seg] == ((side == 2) ? 1 : 2));
    CHECK(o.seg_side[seg] == side);
    OrderNode const port{ o.nodes[o.seg_node[seg]] };
    OrderNode const mate{ node_of(o, b) };
    CHECK(port.rank == mate.rank);
    uint32_t in_rank{ 0 };
    for (OrderNode const &nd : frame_nodes(o, inner)) {
      in_rank += (nd.rank == mate.rank) ? 1U : 0U;
    }
    CHECK(in_rank == 3);
    CHECK(port.pos == ((side == 2) ? 0U : (in_rank - 1)));
    CHECK(node_of(o, y).pos < mate.pos);
    CHECK(node_of(o, a).rank == node_of(o, q).rank);
    CHECK(o.sub_ranks[inner.v] == 2);
    uint32_t flat{ 0 };
    for (OrderEdge const &e : o.edges) {
      if (e.segment != seg) { continue; }
      ++flat;
      CHECK(o.nodes[e.src].rank == o.nodes[e.dst].rank);
    }
    CHECK(flat == 1);
  }

  // Either leg meeting at the port names it: here the outer leg's arrival.
  SearchPins const outer{ .ends = { { .trans = drop, .leg = 0, .end = 1, .face = 2 } } };
  SubmachineOrders const named{ order_submachines(c, g, {}, profile(), 0, outer) };
  CHECK(named.seg_cross[seg] == 1);
  CHECK(named.seg_sided[seg] == 1);
  CHECK(named.seg_side[seg] == 2);

  // Turned down, left is one of the frame's cross borders.
  SearchPins const turned{ .ends = { { .trans = drop, .leg = 1, .face = 0 } },
                           .orients = { { .frame = inner } } };
  SubmachineOrders const down{ order_submachines(c, g, {}, profile(), 0, turned) };
  CHECK(down.seg_cross[seg] == 1);
  CHECK(down.seg_side[seg] == 0);
  CHECK(down.nodes[down.seg_node[seg]].rank == node_of(down, b).rank);
}

TEST_CASE("order: a port pinned where its frame's ranks start or end turns its edge") {
  // Left, where an entering port already is, changes nothing, even against a reversal pin;
  // right makes the port a sink on the last rank and turns its edge.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  TransId const drop{ build_trans(c, d, b, TransKind::External, {}) };
  SplitGraph const g{ decompose(c) };
  uint32_t const seg{ g.trans_segments[drop.v].off + 1 };
  SubmachineOrders const plain{ order_submachines(c, g, {}, profile()) };
  CHECK(plain.nodes[plain.seg_node[seg]].rank == 0);

  SearchPins const left{ .reverses = { { .trans = drop, .leg = 1 } },
                         .ends = { { .trans = drop, .leg = 1, .face = 0 } } };
  SubmachineOrders const same{ order_submachines(c, g, {}, profile(), 0, left) };
  CHECK(same.nodes == plain.nodes);
  CHECK(same.edges == plain.edges);
  CHECK(same.seg_cross[seg] == 0);
  CHECK(same.seg_sided[seg] == 1);
  CHECK(same.seg_side[seg] == 0);

  SearchPins const right{ .ends = { { .trans = drop, .leg = 1, .face = 1 } } };
  SubmachineOrders const o{ order_submachines(c, g, {}, profile(), 0, right) };
  CHECK(o.seg_cross[seg] == 0);
  CHECK(o.seg_side[seg] == 1);  // a sink on the trailing border
  CHECK((o.nodes[o.seg_node[seg]].rank + 1) == o.sub_ranks[inner.v]);
  CHECK(node_of(o, b).rank < o.nodes[o.seg_node[seg]].rank);
  CHECK(node_of(o, a).rank == 0);
  for (OrderEdge const &e : o.edges) {
    if (e.segment != seg) { continue; }
    CHECK(e.dst == o.seg_node[seg]);
    CHECK(e.reversed == 1);
  }
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
  // The boundary is a source on rank 0; `S` takes rank 1.
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
  // Every emitted edge spans one rank.
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

TEST_CASE("order: a label across several boundaries is charged where none holds it") {
  // A -> B -> C -> D and A -> D. The first and last boundaries carry two turning lanes,
  // 538 at `readable`; the middle carries none.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  StateId const e{ build_state(c, root, "D", StateKind::Normal, {}) };
  TransId const first{ build_trans(c, a, b, TransKind::External, {}) };
  build_trans(c, b, d, TransKind::External, {});
  build_trans(c, d, e, TransKind::External, {});
  TransId const across{ build_trans(c, a, e, TransKind::External, {}) };

  auto const gaps_for = [&](int32_t first_w, int32_t across_w) {
    std::array<scav_path_box, 2> const boxes{
      scav_path_box{ .subject = first.v, .w = first_w, .h = 40, .order = 0 },
      scav_path_box{ .subject = across.v, .w = across_w, .h = 40, .order = 0 },
    };
    scav_spaces const s{ .path_box = boxes.data(),
                         .n_path_box = (first_w == 0) ? 0U : 2U };
    scav_spaces const alone{ .path_box = boxes.data() + 1, .n_path_box = 1 };
    SubmachineOrders const o{ order_of(c, (first_w == 0) ? alone : s) };
    Span const sp{ o.sub_gaps[root.v] };
    return std::vector<int32_t>{ o.gaps.begin() + sp.off,
                                 o.gaps.begin() + sp.off + sp.len };
  };
  // The first boundary is already 700 wide for A -> B, so it holds A -> D's 500.
  CHECK(gaps_for(700, 500) == std::vector<int32_t>{ 700, 0, 538 });
  // Nothing it crosses is 900 wide, so the widest boundary it crosses grows.
  CHECK(gaps_for(700, 900) == std::vector<int32_t>{ 900, 0, 538 });
  // Equal widths either side of the middle: the first of them.
  CHECK(gaps_for(0, 600) == std::vector<int32_t>{ 600, 0, 538 });
  // The lanes already hold a label no wider than them.
  CHECK(gaps_for(0, 538) == std::vector<int32_t>{ 538, 0, 538 });
}

TEST_CASE("order: the label row holds every label where it was charged, and no lane") {
  // The chain and long edge above. `gaps` carries the two possible lanes at the first and
  // last boundaries; `labels` carries the labels alone, the held long label included.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  StateId const e{ build_state(c, root, "D", StateKind::Normal, {}) };
  TransId const first{ build_trans(c, a, b, TransKind::External, {}) };
  build_trans(c, b, d, TransKind::External, {});
  build_trans(c, d, e, TransKind::External, {});
  TransId const across{ build_trans(c, a, e, TransKind::External, {}) };

  auto const rows_for =
      [&](int32_t first_w, int32_t across_w, std::vector<int32_t> &gaps) {
        std::array<scav_path_box, 2> const boxes{
          scav_path_box{ .subject = first.v, .w = first_w, .h = 40, .order = 0 },
          scav_path_box{ .subject = across.v, .w = across_w, .h = 40, .order = 0 },
        };
        scav_spaces const s{ .path_box = boxes.data(),
                             .n_path_box = (first_w == 0) ? 0U : 2U };
        scav_spaces const alone{ .path_box = boxes.data() + 1, .n_path_box = 1 };
        SubmachineOrders const o{ order_of(c, (first_w == 0) ? alone : s) };
        REQUIRE(o.labels.size() == o.gaps.size());
        Span const sp{ o.sub_gaps[root.v] };
        gaps.assign(o.gaps.begin() + sp.off, o.gaps.begin() + sp.off + sp.len);
        return std::vector<int32_t>{ o.labels.begin() + sp.off,
                                     o.labels.begin() + sp.off + sp.len };
      };
  std::vector<int32_t> gaps;
  CHECK(rows_for(700, 500, gaps) == std::vector<int32_t>{ 700, 0, 0 });
  CHECK(gaps == std::vector<int32_t>{ 700, 0, 538 });
  CHECK(rows_for(0, 538, gaps) == std::vector<int32_t>{ 538, 0, 0 });
  CHECK(gaps == std::vector<int32_t>{ 538, 0, 538 });
  CHECK(rows_for(0, 0, gaps) == std::vector<int32_t>{ 0, 0, 0 });
  CHECK(gaps == std::vector<int32_t>{ 538, 0, 538 });
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
  // 500 for the label, charged in one frame; 538 for two turning lanes at `readable`.
  CHECK(total == 1038);
  std::vector<int32_t> per;
  for (Span const &frame : o.sub_gaps) {
    int32_t most{ 0 };
    for (uint32_t k = 0; k < frame.len; ++k) { most = imax(most, o.gaps[frame.off + k]); }
    per.push_back(most);
  }
  CHECK(per == std::vector<int32_t>{ 500, 538 });
}

TEST_CASE("order: a label into a composite is charged to the frame holding both ends") {
  // D -> C/T lies in the root from D to C's border, then in C from there to T. The label
  // is charged in the root, which holds both ends, though C holds the middle segment.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const t{ build_state(c, inner, "T", StateKind::Normal, {}) };
  TransId const into{ build_trans(c, d, t, TransKind::External, {}) };

  SplitGraph const g{ decompose(c) };
  Span const segs{ g.trans_segments[into.v] };
  REQUIRE(segs.len == 2);
  REQUIRE(g.segments[segs.off].frame == root);
  CHECK(label_segment(c, g, into.v) == segs.off);

  scav_path_box const box{ .subject = into.v, .w = 700, .h = 40, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  SubmachineOrders const o{ order_submachines(c, g, s, profile()) };
  Span const outer{ o.sub_gaps[root.v] };
  REQUIRE(outer.len == 1);
  CHECK(o.gaps[outer.off] == 700);
  Span const within{ o.sub_gaps[inner.v] };
  for (uint32_t k = 0; k < within.len; ++k) { CHECK(o.gaps[within.off + k] == 0); }
}

TEST_CASE("order: the lowest common ancestor of every shape of two ends") {
  // Root: A, B, and P with two regions R1 and R2. A holds A1, which holds A2.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  SubmachineId const a_sub{ build_submachine(c, a, {}, {}) };
  StateId const a1{ build_state(c, a_sub, "A1", StateKind::Normal, {}) };
  SubmachineId const a1_sub{ build_submachine(c, a1, {}, {}) };
  StateId const a2{ build_state(c, a1_sub, "A2", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const pst{ build_state(c, root, "P", StateKind::Normal, {}) };
  SubmachineId const r1{ build_submachine(c, pst, {}, {}) };
  SubmachineId const r2{ build_submachine(c, pst, {}, {}) };
  StateId const x{ build_state(c, r1, "X", StateKind::Normal, {}) };
  StateId const y{ build_state(c, r2, "Y", StateKind::Normal, {}) };
  StateId const none{ INVALID };

  auto const holds =
      [&](StateId src, StateId dst, SubmachineId frame, StateId from, StateId to) {
        TransId const t{ build_trans(c, src, dst, TransKind::External, {}) };
        CommonAncestor const got{ decompose(c).trans_common[t.v] };
        CAPTURE(src.v);
        CAPTURE(dst.v);
        CHECK(got.frame == frame);
        CHECK(got.child[0] == from);
        CHECK(got.child[1] == to);
      };
  holds(a, b, root, a, b);          // siblings
  holds(a2, b, root, a, b);         // out of two composites
  holds(b, a2, root, b, a);         // into two
  holds(a, a2, a_sub, none, a1);    // a composite to its own descendant
  holds(a2, a, a_sub, a1, none);    // and back
  holds(a2, a1, a1_sub, a2, none);  // to the composite holding it
  holds(a1, a1, a_sub, a1, a1);     // a self-transition
  holds(x, y, { INVALID }, x, y);   // two regions of one state
  holds(x, b, root, pst, b);        // out of a region
}

TEST_CASE("order: a sweep removes a crossing document order would have left") {
  // Two sources and two sinks wired across, so document order crosses once.
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
  // Zero crossings: each sink takes its source's position.
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
    same.ranks.push_back(
        { .state = st, .rank = plain.nodes[plain.state_node[st.v]].rank });
  }
  SubmachineOrders const pinned{ order_submachines(c, g, {}, profile(), 0, same) };
  CHECK(pinned.nodes == plain.nodes);
  CHECK(pinned.edges == plain.edges);
  CHECK(pinned.gaps == plain.gaps);
  CHECK(pinned.sub_ranks == plain.sub_ranks);
}

TEST_CASE("order: a pin moves a node's rank and the ranks stay contiguous") {
  // A -> B -> D ranks 0, 1, 2. Pinning D onto B's rank empties rank 2, and the ranks
  // renumber onto those that still hold a node.
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
  // Every rank holds at least one node.
  std::vector<uint32_t> held(moved.sub_ranks[root.v], 0);
  Span const sp{ moved.sub_nodes[root.v] };
  for (uint32_t i = 0; i < sp.len; ++i) { ++held[moved.nodes[sp.off + i].rank]; }
  for (uint32_t const n : held) { CHECK(n > 0); }
}

TEST_CASE("order: a labelled edge inside one rank charges no rank boundary") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  std::vector<scav_path_box> const boxes{
    { .subject = 0, .w = 5000, .h = 200, .order = 0 }
  };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 1 };
  SplitGraph const g{ decompose(c) };

  // Unpinned, the edge spans ranks and its label is charged.
  SubmachineOrders const plain{ order_submachines(c, g, s, profile()) };
  Span const plain_gaps{ plain.sub_gaps[root.v] };
  REQUIRE(plain_gaps.len > 0);
  CHECK(plain.gaps[plain_gaps.off] >= 5000);

  SearchPins const flat{ .ranks = { { .state = b, .rank = 0 } } };
  SubmachineOrders const pinned{ order_submachines(c, g, s, profile(), 0, flat) };
  REQUIRE(pinned.nodes[pinned.state_node[a.v]].rank ==
          pinned.nodes[pinned.state_node[b.v]].rank);
  Span const gaps{ pinned.sub_gaps[root.v] };
  for (uint32_t k = 0; k < gaps.len; ++k) {
    CAPTURE(k);
    CHECK(pinned.gaps[gaps.off + k] < 5000);
  }
}

TEST_CASE("order: a frame turned down charges a label's height to its rank gap") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  std::vector<scav_path_box> const boxes{
    { .subject = 0, .w = 5000, .h = 300, .order = 0 }
  };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 1 };
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const across{ order_submachines(c, g, s, profile()) };
  SearchPins const turned{ .orients = { { .frame = root } } };
  SubmachineOrders const down{ order_submachines(c, g, s, profile(), 0, turned) };
  REQUIRE(down.sub_down[root.v] == 1);
  REQUIRE(across.sub_gaps[root.v].len == 1);
  REQUIRE(down.sub_gaps[root.v].len == 1);
  CHECK(across.gaps[across.sub_gaps[root.v].off] >= 5000);
  CHECK(down.gaps[down.sub_gaps[root.v].off] >= 300);
  CHECK(down.gaps[down.sub_gaps[root.v].off] < 5000);
}

TEST_CASE("order: undoing a move is running with the pins one held before it") {
  // `before` pins each node to its unpinned rank and reproduces the unpinned orders.
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

TEST_CASE("order: an initial pseudostate is ranked just before the state it enters") {
  // The initial follows `B` wherever a pin moves it.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, start, b, TransKind::External, {});
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  SplitGraph const g{ decompose(c) };

  auto const ranks = [&](SearchPins const &pins) {
    SubmachineOrders const o{ order_submachines(c, g, {}, profile(), 0, pins) };
    return std::array<uint32_t, 3>{ node_of(o, start).rank,
                                    node_of(o, b).rank,
                                    node_of(o, d).rank };
  };
  // Unpinned: longest path puts it next to `B`.
  std::array<uint32_t, 3> const plain{ ranks({}) };
  CHECK(plain[0] + 1 == plain[1]);
  // `B` pinned two ranks further on: the initial is re-seated beside it.
  std::array<uint32_t, 3> const moved{ ranks({ .ranks = { { .state = b, .rank = 3 } } }) };
  CHECK(moved[0] + 1 == moved[1]);
  // `B` pinned to rank 0: the initial takes rank 0 and `B` rank 1.
  std::array<uint32_t, 3> const first{ ranks({ .ranks = { { .state = b, .rank = 0 } } }) };
  CHECK(first[0] == 0);
  CHECK(first[1] == 1);
  // A pin on the initial is ignored.
  CHECK(ranks({ .ranks = { { .state = start, .rank = 2 } } }) == plain);
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

// A three-state cycle whose back edge spans two ranks after cycle-breaking. A cut
// drops that edge's bend.
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
  for (OrderNode const &nd : o.nodes) { n += (nd.kind == OrderKind::Bend) ? 1U : 0U; }
  return n;
}

// A cut on leg `leg` of the back edge.
SearchPins cut_of(Cycle const &z, uint32_t leg = 0) {
  return { .cuts = { { .trans = z.back, .leg = leg } } };
}

}  // namespace

TEST_CASE("order: the back edge of a cycle chains, and a cut leaves it long") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  REQUIRE(bends_of(plain) == 1);

  SubmachineOrders const freed{ order_submachines(z.c, z.g, {}, profile(), 0, cut_of(z)) };
  CHECK(bends_of(freed) == 0);

  // One edge spans more than one rank, with no bend between its ends.
  uint32_t spanning{ 0 };
  for (OrderEdge const &e : freed.edges) {
    uint32_t const from{ freed.nodes[e.src].rank };
    uint32_t const to{ freed.nodes[e.dst].rank };
    spanning += ((to - from) > 1) ? 1U : 0U;
  }
  CHECK(spanning == 1);
  CHECK(freed.edges.size() == (plain.edges.size() - 1));  // the chain has two edges
}

TEST_CASE("order: a cut is undone by dropping it, and the orders come back") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  SubmachineOrders const freed{ order_submachines(z.c, z.g, {}, profile(), 0, cut_of(z)) };
  REQUIRE(freed.nodes != plain.nodes);  // the cut did something
  SubmachineOrders const back{ order_submachines(z.c, z.g, {}, profile(), 0, {}) };
  CHECK(back.nodes == plain.nodes);
  CHECK(back.edges == plain.edges);
}

TEST_CASE("order: a cut naming nothing this chart has is ignored, not applied") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  // Legs 1 and 99 of a one-leg transition, transition 4096, and INVALID each leave
  // the orders unchanged.
  for (SearchPins const &pins :
       { cut_of(z, 1),
         cut_of(z, 99),
         SearchPins{ .cuts = { { .trans = TransId{ 4096 }, .leg = 0 } } },
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
  CHECK(bends_of(o) == 0);                          // the cut held
  CHECK(o.nodes[o.state_node[z.d.v]].rank != was);  // and so did the pin
  CHECK(o.nodes[o.state_node[z.d.v]].rank == 0);
}

TEST_CASE("order: a reversal pin turns the named edge, and the walk turns no other") {
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  auto const reversed_segs = [](SubmachineOrders const &o) {
    std::vector<uint32_t> segs;
    for (OrderEdge const &e : o.edges) {
      if ((e.reversed != 0) && (std::ranges::find(segs, e.segment) == segs.end())) {
        segs.push_back(e.segment);
      }
    }
    return segs;
  };
  // Unpinned, the walk turns the edge that closes the cycle: the back edge.
  CHECK(reversed_segs(plain) == std::vector<uint32_t>{ 2 });

  // Pinned, only the pinned edge is reversed; one edge still spans two ranks, so one bend.
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

TEST_CASE("order: an edge a pin turns and the walk turns back is not marked reversed") {
  // Turning `A -> B` closes `A -> C -> D -> B -> A`, which the walk breaks at the pinned
  // edge.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  StateId const e{ build_state(c, root, "D", StateKind::Normal, {}) };
  TransId const ab{ build_trans(c, a, b, TransKind::External, {}) };
  build_trans(c, a, d, TransKind::External, {});
  build_trans(c, d, e, TransKind::External, {});
  build_trans(c, e, b, TransKind::External, {});
  build_trans(c, b, a, TransKind::External, {});
  SplitGraph const g{ decompose(c) };
  SearchPins const pin{ .reverses = { { .trans = ab, .leg = 0 } } };
  SubmachineOrders const o{ order_submachines(c, g, {}, profile(), 0, pin) };
  REQUIRE(node_of(o, a).rank < node_of(o, b).rank);  // turned back to its authored way
  uint32_t legs{ 0 };
  for (OrderEdge const &oe : o.edges) {
    if (oe.segment != g.trans_segments[ab.v].off) { continue; }
    CHECK(oe.reversed == 0);
    ++legs;
  }
  CHECK(legs == 3);  // A to B spans three ranks
}

TEST_CASE(
    "order: an edge is marked reversed exactly where it runs against its authoring") {
  // Every corpus chart, unpinned and with each segment on a cycle pinned turned in turn.
  constexpr std::array<char const *, 14> CHARTS{
    "axis.scav",    "bottler.scav", "brew.scav",        "dock.scav", "elevator.scav",
    "estop.scav",   "kiln.scav",    "led.scav",         "mill.scav", "ota.scav",
    "printer.scav", "tcp.scav",     "toolchanger.scav", "vac.scav"
  };
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    std::string const chart{ name };
    CAPTURE(chart);
    std::string const path{ SCAV_TEST_DATA_DIR "/charts/" + chart };
    Loader loader;
    Chart c;
    std::vector<Diagnostic> diags;
    std::string failed;
    REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const plain{ order_submachines(c, g, {}, profile()) };
    std::vector<SearchPins> runs{ SearchPins{} };
    for (uint32_t seg = 0; seg < g.segments.size(); ++seg) {
      if (plain.seg_cyclic[seg] == 0) { continue; }
      TransId const t{ g.segments[seg].trans };
      runs.push_back(
          { .reverses = { { .trans = t, .leg = seg - g.trans_segments[t.v].off } } });
    }
    uint32_t checked{ 0 };
    for (SearchPins const &pins : runs) {
      uint32_t const turned{ pins.reverses.empty() ? INVALID : pins.reverses[0].trans.v };
      CAPTURE(turned);
      SubmachineOrders const o{ order_submachines(c, g, {}, profile(), 0, pins) };
      for (OrderEdge const &oe : o.edges) {
        SplitSegment const &sg{ g.segments[oe.segment] };
        if ((sg.src_port != INVALID) || (sg.dst_port != INVALID) || (sg.src_inner != 0) ||
            (sg.dst_inner != 0)) {
          continue;
        }
        Transition const &t{ c.transitions[sg.trans.v] };
        uint32_t const from{ o.state_node[t.src.v] };
        uint32_t const to{ o.state_node[t.dst.v] };
        if ((from == INVALID) || (to == INVALID) ||
            (o.nodes[from].rank == o.nodes[to].rank)) {
          continue;
        }
        CHECK(oe.reversed == ((o.nodes[from].rank > o.nodes[to].rank) ? 1U : 0U));
        ++checked;
      }
    }
    CHECK(checked > 0);
  }
}

TEST_CASE("order: a segment on a cycle is reported, and one on none is not") {
  // A segment is cyclic when its ends share a strongly connected component of the
  // frame's graph before any turn, so reversal pins leave the flags unchanged.
  Cycle const z{ three_state_cycle() };
  SubmachineOrders const plain{ order_submachines(z.c, z.g, {}, profile()) };
  for (uint32_t seg = 0; seg < 3; ++seg) { CHECK(plain.seg_cyclic[seg] == 1); }
  for (uint32_t t = 0; t < 3; ++t) {
    SearchPins const pin{ .reverses = { { .trans = TransId{ t }, .leg = 0 } } };
    SubmachineOrders const turned{ order_submachines(z.c, z.g, {}, profile(), 0, pin) };
    CHECK(turned.seg_cyclic == plain.seg_cyclic);
  }

  // An entry into the cycle is on no cycle, and neither is a chain.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  StateId const e{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, e, a, TransKind::External, {});  // into the cycle
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  build_trans(c, d, a, TransKind::External, {});
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ order_submachines(c, g, {}, profile()) };
  CHECK(o.seg_cyclic[0] == 0);
  CHECK(o.seg_cyclic[1] == 1);
  CHECK(o.seg_cyclic[2] == 1);
  CHECK(o.seg_cyclic[3] == 1);

  Chart chain;
  SubmachineId const r2{ build_chart(chain, "t", {}) };
  StateId const x{ build_state(chain, r2, "X", StateKind::Normal, {}) };
  StateId const y{ build_state(chain, r2, "Y", StateKind::Normal, {}) };
  StateId const w{ build_state(chain, r2, "W", StateKind::Normal, {}) };
  build_trans(chain, x, y, TransKind::External, {});
  build_trans(chain, y, w, TransKind::External, {});
  SplitGraph const cg{ decompose(chain) };
  SubmachineOrders const co{ order_submachines(chain, cg, {}, profile()) };
  CHECK(co.seg_cyclic == std::vector<uint8_t>{ 0, 0 });
}

TEST_CASE("order: cycle detection survives a frame deep enough to overflow recursion") {
  // One ring of 4,096 states, every edge on the one cycle.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> ring;
  ring.reserve(4096);
  for (uint32_t i = 0; i < 4096; ++i) {
    ring.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 0; i < ring.size(); ++i) {
    build_trans(c, ring[i], ring[(i + 1) % ring.size()], TransKind::External, {});
  }
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ order_submachines(c, g, {}, profile()) };
  uint32_t on{ 0 };
  for (uint8_t const flag : o.seg_cyclic) { on += flag; }
  CHECK(on == 4096);
}

TEST_CASE("order: a call nested on a thread waiting in another is each its own") {
  // Each outer shard runs a call sharded on the pool and a job of whole calls, so one call
  // can start on a thread while another there waits on its shards.
  std::string path{ SCAV_TEST_DATA_DIR "/charts/mill.scav" };
  Loader loader;
  Chart c;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const base{ order_submachines(c, g, {}, p, 1, {}) };
  constexpr uint32_t OUTER{ 12 };
  constexpr uint32_t INNER{ 3 };
  constexpr uint32_t CALLS{ OUTER * (1 + INNER) };
  std::vector<SearchPins> pins(CALLS);
  uint32_t k{ 0 };
  for (uint32_t st = 0; (st < c.states.size()) && (k < CALLS); ++st) {
    if ((c.states[st].live == 0) || (base.state_node[st] == INVALID)) { continue; }
    uint32_t const frame{ c.states[st].parent.v };
    if ((frame >= base.sub_ranks.size()) || (base.sub_ranks[frame] < 2)) { continue; }
    pins[k].ranks.push_back({ .state = StateId{ st }, .rank = 1 });
    ++k;
  }
  REQUIRE(k == CALLS);
  std::vector<SubmachineOrders> want(CALLS);
  for (uint32_t i = 0; i < CALLS; ++i) {
    want[i] = order_submachines(c, g, {}, p, 1, pins[i]);
  }
  std::vector<SubmachineOrders> got(CALLS);
  for (uint32_t trial = 0; trial < 24; ++trial) {
    parallel_for(OUTER, 0U, [&](uint32_t o) {
      uint32_t const first{ o * (1 + INNER) };
      got[first] = order_submachines(c, g, {}, p, 0, pins[first]);
      parallel_for(INNER, 0U, [&, first](uint32_t i) {
        got[first + 1 + i] = order_submachines(c, g, {}, p, 1, pins[first + 1 + i]);
      });
    });
    bool same{ true };
    for (uint32_t i = 0; i < CALLS; ++i) {
      same = same && (got[i].nodes == want[i].nodes) && (got[i].edges == want[i].edges) &&
             (got[i].sub_nodes == want[i].sub_nodes) && (got[i].gaps == want[i].gaps) &&
             (got[i].state_node == want[i].state_node) &&
             (got[i].seg_cyclic == want[i].seg_cyclic);
    }
    CAPTURE(trial);
    CHECK(same);
  }
}
