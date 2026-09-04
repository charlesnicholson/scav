// Sizing against hand-written intermediates, so what is under test is the box
// formula rather than whatever ordering produced.

#include "layout/size.h"

#include "layout/decompose.h"
#include "layout/order.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"

#include "doctest.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace scav;

// The C structs carry no operators; the tests compare them field-wise.
constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

scav_profile profile() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

// A desired aspect so wide that folding a rank run can never beat leaving it
// flat, which is what lets a test measure the layering axis on its own.
scav_profile unfolded() {
  scav_profile p{ profile() };
  p.dar_num = 1024;
  p.dar_den = 1;
  return p;
}

// Depths only: sizing reads nothing else from the split unless a route
// terminates on a frame border, which the boundary case below supplies.
SplitGraph depths(std::vector<uint32_t> const &state_depth) {
  SplitGraph g;
  g.state_depth = state_depth;
  return g;
}

OrderNode state_node(uint32_t subject, uint32_t rank, uint32_t pos) {
  return { .kind = OrderKind::State, .subject = subject, .rank = rank, .pos = pos };
}

// One frame's worth of orders over `nodes`, everything else empty.
SubmachineOrders one_frame(Chart const &c,
                           SubmachineId frame,
                           std::vector<OrderNode> nodes,
                           std::vector<OrderEdge> edges,
                           std::vector<int32_t> gaps) {
  SubmachineOrders o;
  o.sub_nodes.assign(c.submachines.size(), Span{});
  o.sub_edges.assign(c.submachines.size(), Span{});
  o.sub_ranks.assign(c.submachines.size(), 0);
  o.sub_gaps.assign(c.submachines.size(), Span{});
  o.state_node.assign(c.states.size(), INVALID);
  uint32_t ranks{ 0 };
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    ranks = (nodes[i].rank + 1 > ranks) ? (nodes[i].rank + 1) : ranks;
    if (nodes[i].kind == OrderKind::State) { o.state_node[nodes[i].subject] = i; }
  }
  o.nodes = std::move(nodes);
  o.edges = std::move(edges);
  o.gaps = std::move(gaps);
  o.sub_nodes[frame.v] = make_span(0, static_cast<uint32_t>(o.nodes.size()));
  o.sub_edges[frame.v] = make_span(0, static_cast<uint32_t>(o.edges.size()));
  o.sub_ranks[frame.v] = ranks;
  o.sub_gaps[frame.v] = make_span(0, static_cast<uint32_t>(o.gaps.size()));
  return o;
}

}  // namespace

TEST_CASE("size: a leaf is its kind minimum plus two pads") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  scav_profile const p{ profile() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(phase2_size(c,
                      depths({ 0 }),
                      one_frame(c, root, { state_node(a.v, 0, 0) }, {}, {}),
                      {},
                      p,
                      z,
                      diags));
  CHECK(z.state[a.v].w == p.kind_min_w[0] + (2 * p.pad));
  CHECK(z.state[a.v].h == p.kind_min_h[0] + (2 * p.pad));
  CHECK(z.state[a.v].x == 0);
  CHECK(z.state[a.v].y == 0);
  CHECK((z.chart == z.state[a.v]));
}

TEST_CASE("size: two ranks sit rank_sep apart along the layering axis") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  scav_profile const p{ unfolded() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(phase2_size(c,
                      depths({ 0, 0 }),
                      one_frame(c,
                                root,
                                { state_node(a.v, 0, 0), state_node(b.v, 1, 0) },
                                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } },
                                { 0 }),
                      {},
                      p,
                      z,
                      diags));
  CHECK(z.state[a.v].x == 0);
  CHECK(z.state[b.v].x == z.state[a.v].w + p.rank_sep);
  CHECK(z.state[a.v].y == z.state[b.v].y);  // one edge, so the two align
  CHECK(z.chart.w == z.state[b.v].x + z.state[b.v].w);
}

TEST_CASE("size: a label's gap widens the boundary it was charged to") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  scav_profile const p{ unfolded() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(phase2_size(c,
                      depths({ 0, 0 }),
                      one_frame(c,
                                root,
                                { state_node(a.v, 0, 0), state_node(b.v, 1, 0) },
                                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } },
                                { 777 }),
                      {},
                      p,
                      z,
                      diags));
  CHECK(z.state[b.v].x == z.state[a.v].w + p.rank_sep + 777);
}

TEST_CASE("size: a rank run folds when folding scales larger") {
  // Six ranks in a chain: flat they draw a strip far off the desired aspect,
  // folded they stack into something nearer it, and the scale measure picks.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<OrderNode> nodes;
  std::vector<OrderEdge> edges;
  StateId prev{ INVALID };
  for (uint32_t i = 0; i < 6; ++i) {
    StateId const at{ build_state(c, root, "S", StateKind::Normal, {}) };
    nodes.push_back(state_node(at.v, i, 0));
    if (prev.v != INVALID) {
      edges.push_back({ .src = i - 1, .dst = i, .segment = i - 1, .reversed = 0 });
    }
    prev = at;
  }
  SubmachineOrders const o{ one_frame(c, root, nodes, edges, { 0, 0, 0, 0, 0 }) };

  SizedLayout flat;
  std::vector<Diagnostic> diags;
  REQUIRE(
      phase2_size(c, depths(std::vector<uint32_t>(6, 0)), o, {}, unfolded(), flat, diags));
  SizedLayout folded;
  diags.clear();
  REQUIRE(phase2_size(c,
                      depths(std::vector<uint32_t>(6, 0)),
                      o,
                      {},
                      profile(),
                      folded,
                      diags));

  CHECK(flat.chart.h == flat.state[0].h);     // one row, six columns
  CHECK(folded.chart.h > folded.state[0].h);  // more than one row
  CHECK(folded.chart.w < flat.chart.w);
}

TEST_CASE("size: two nodes in one rank stack node_sep apart") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, d, TransKind::External, {});
  scav_profile const p{ profile() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(phase2_size(
      c,
      depths({ 0, 0, 0 }),
      one_frame(c,
                root,
                { state_node(a.v, 0, 0), state_node(b.v, 1, 0), state_node(d.v, 1, 1) },
                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                  { .src = 0, .dst = 2, .segment = 1, .reversed = 0 } },
                { 0 }),
      {},
      p,
      z,
      diags));
  CHECK(z.state[b.v].x == z.state[d.v].x);
  CHECK(z.state[d.v].y == z.state[b.v].y + z.state[b.v].h + p.node_sep);
}

TEST_CASE("size: unconnected states are separate components and pack") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  scav_profile const p{ profile() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(phase2_size(
      c,
      depths({ 0, 0 }),
      one_frame(c, root, { state_node(a.v, 0, 0), state_node(b.v, 0, 1) }, {}, {}),
      {},
      p,
      z,
      diags));
  // No edge joins them, so they are two components and the packer places them
  // side by side rather than stacking them in the one rank they share.
  CHECK(z.state[a.v].x == 0);
  CHECK(z.state[b.v].x == z.state[a.v].w + p.node_sep);
  CHECK(z.state[b.v].y == 0);
}

TEST_CASE("size: a boundary node lands on its frame's leading or trailing edge") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  scav_profile const p{ profile() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  // One state with a route leaving it: the boundary is a sink, so it belongs
  // at the frame's trailing edge whatever rank arithmetic put it in.
  REQUIRE(phase2_size(
      c,
      depths({ 0 }),
      one_frame(c,
                root,
                { state_node(a.v, 0, 0),
                  { .kind = OrderKind::Boundary, .subject = 0, .rank = 1, .pos = 0 } },
                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } },
                { 0 }),
      {},
      p,
      z,
      diags));
  CHECK(z.node[1].x == z.sub[root.v].w);

  // The same shape with the route arriving instead: a source, at the leading
  // edge, and the state to its right.
  SizedLayout in;
  diags.clear();
  REQUIRE(phase2_size(
      c,
      depths({ 0 }),
      one_frame(c,
                root,
                { { .kind = OrderKind::Boundary, .subject = 0, .rank = 0, .pos = 0 },
                  state_node(a.v, 1, 0) },
                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } },
                { 0 }),
      {},
      p,
      in,
      diags));
  CHECK(in.node[0].x == 0);
  CHECK(in.state[a.v].x > 0);
}

TEST_CASE("size: a folded rank run packs its pieces rather than stacking them") {
  // Stacking gives every piece the width of the widest. One huge rank among small
  // ones is where that shows: each small piece gets a row as wide as the huge one.
  scav_profile const p{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> chain;
  chain.reserve(9);
  for (uint32_t i = 0; i < 9; ++i) {
    chain.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < chain.size(); ++i) {
    build_trans(c, chain[i - 1], chain[i], TransKind::External, {});
  }
  // One rank far larger than the rest, which is the shape a composite state
  // makes of its siblings.
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[chain[4].v] = { .min_w = 400, .h_before = 6000, .h_after = 0 };
  scav_spaces const sp{ .box_state = boxes.data(),
                        .n_box_state = static_cast<uint32_t>(boxes.size()) };

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ phase1_order(c, g, sp, p) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(phase2_size(c, g, o, sp, p, z, diags));

  scav_rect const frame{ z.sub[root.v] };
  REQUIRE(frame.w > 0);
  REQUIRE(frame.h > 0);
  MESSAGE("folded frame: ", frame.w, " x ", frame.h);

  // Stacking is monotonic, so a later rank can never sit higher than an earlier
  // one. Packing is not, and here it puts the last short piece beside the first.
  Span const all{ o.sub_nodes[root.v] };
  int32_t highest_so_far{ 0 };
  uint32_t previous_rank{ 0 };
  bool beside{ false };
  for (uint32_t k = 0; k < all.len; ++k) {
    uint32_t const rank{ o.nodes[all.off + k].rank };
    int32_t const y{ z.node[all.off + k].y };
    if ((rank > previous_rank) && (y < highest_so_far)) { beside = true; }
    highest_so_far = imax(highest_so_far, y);
    previous_rank = imax(previous_rank, rank);
  }
  CHECK(beside);

  // And every node still lands inside the frame it was sized into.
  Span const nodes{ o.sub_nodes[root.v] };
  for (uint32_t k = 0; k < nodes.len; ++k) {
    scav_point const at{ z.node[nodes.off + k] };
    CAPTURE(k);
    CHECK(at.x >= frame.x);
    CHECK(at.y >= frame.y);
    CHECK(at.x <= (frame.x + frame.w));
    CHECK(at.y <= (frame.y + frame.h));
  }
}

TEST_CASE("size: a rank past the domain is diagnosed rather than truncated") {
  // Five maximal-height states in one rank, joined so they stay one component
  // and cannot be packed apart: the column is taller than the domain.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const source{ build_state(c, root, "S", StateKind::Normal, {}) };
  std::vector<OrderNode> nodes{ state_node(source.v, 0, 0) };
  std::vector<OrderEdge> edges;
  for (uint32_t i = 0; i < 5; ++i) {
    StateId const target{ build_state(c, root, "T", StateKind::Normal, {}) };
    nodes.push_back(state_node(target.v, 1, i));
    edges.push_back({ .src = 0, .dst = i + 1, .segment = i, .reversed = 0 });
  }
  std::vector<scav_box_space> const boxes(
      c.states.size(),
      { .min_w = 0, .h_before = SPACE_MAX, .h_after = 0 });
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(phase2_size(c,
                          depths(std::vector<uint32_t>(c.states.size(), 0)),
                          one_frame(c, root, nodes, edges, { 0 }),
                          s,
                          profile(),
                          z,
                          diags));
  REQUIRE(!diags.empty());
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::Submachine);
  CHECK(diags[0].subject.ordinal == root.v);
}

TEST_CASE("size: a pseudostate takes the padding ring only where it has contents") {
  // A junction's kind minimum is narrower than two pads, so a ring the descent
  // insets but the box formula never reserved gives the bands a negative width.
  scav_profile const p{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const lone{ build_state(c, root, "J", StateKind::Junction, {}) };
  StateId const empty{ build_state(c, root, "K", StateKind::Junction, {}) };
  build_submachine(c, empty, {}, {});
  StateId const holding{ build_state(c, root, "L", StateKind::Junction, {}) };
  SubmachineId const inner{ build_submachine(c, holding, {}, {}) };
  build_state(c, inner, "M", StateKind::Normal, {});
  StateId const ordinary{ build_state(c, root, "N", StateKind::Normal, {}) };

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ phase1_order(c, g, {}, p) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(phase2_size(c, g, o, {}, p, z, diags));

  auto const banded = [&](StateId s) {
    scav_rect const r{ z.state[s.v] };
    CAPTURE(s.v);
    for (scav_rect const &band : { z.before[s.v], z.after[s.v] }) {
      CHECK(band.w >= 0);
      CHECK(band.x >= r.x);
      CHECK(band.y >= r.y);
      CHECK((band.x + band.w) <= (r.x + r.w));
      CHECK((band.y + band.h) <= (r.y + r.h));
    }
  };

  uint32_t const junction{ static_cast<uint32_t>(StateKind::Junction) };
  // Nothing inside either, declared submachine or not, so both are the mark
  // itself and the bands span the whole of it.
  CHECK(z.state[lone.v].w == p.kind_min_w[junction]);
  CHECK(z.state[empty.v].w == p.kind_min_w[junction]);
  CHECK(z.before[lone.v].w == z.state[lone.v].w);
  CHECK(z.before[empty.v].w == z.state[empty.v].w);
  banded(lone);
  banded(empty);

  // One with a child submachine to ring, and an ordinary box, which is a
  // container even with nothing in it.
  CHECK(z.state[holding.v].w == z.sub[inner.v].w + (2 * p.pad));
  CHECK(z.before[holding.v].w == z.state[holding.v].w - (2 * p.pad));
  CHECK(z.state[ordinary.v].w == p.kind_min_w[0] + (2 * p.pad));
  CHECK(z.before[ordinary.v].w == z.state[ordinary.v].w - (2 * p.pad));
  banded(holding);
  banded(ordinary);
}

TEST_CASE("size: a boundary node sits on the frame's border, not on its piece's") {
  // Nine ranks in a chain with one tall rank in the middle: the fold cuts three
  // pieces and the packer puts the last beside the first, so the middle piece's
  // own right edge is well inside the frame. A sink boundary sharing a rank
  // with that piece is the node the difference shows on.
  scav_profile const pf{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> chain;
  chain.reserve(9);
  for (uint32_t i = 0; i < 9; ++i) {
    chain.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  std::vector<OrderNode> nodes;
  std::vector<OrderEdge> edges;
  nodes.reserve(10);
  for (uint32_t i = 0; i < 7; ++i) { nodes.push_back(state_node(chain[i].v, i, 0)); }
  uint32_t const boundary{ static_cast<uint32_t>(nodes.size()) };
  nodes.push_back({ .kind = OrderKind::Boundary, .subject = 0, .rank = 7, .pos = 0 });
  nodes.push_back(state_node(chain[7].v, 7, 1));
  nodes.push_back(state_node(chain[8].v, 8, 0));
  for (uint32_t i = 1; i < 7; ++i) {
    edges.push_back({ .src = i - 1, .dst = i, .segment = i - 1, .reversed = 0 });
  }
  edges.push_back({ .src = 6, .dst = boundary, .segment = 6, .reversed = 0 });
  edges.push_back({ .src = 6, .dst = boundary + 1, .segment = 7, .reversed = 0 });
  edges.push_back(
      { .src = boundary + 1, .dst = boundary + 2, .segment = 8, .reversed = 0 });

  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[chain[4].v] = { .min_w = 0, .h_before = 6000, .h_after = 0 };
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(phase2_size(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, nodes, edges, { 0, 0, 0, 0, 0, 0, 0, 0 }),
                      s,
                      pf,
                      z,
                      diags));

  // The boundary's own rank ends mid-frame, which is where taking its x from
  // its piece would have left it.
  scav_rect const mate{ z.state[chain[7].v] };
  REQUIRE((mate.x + mate.w) < z.sub[root.v].w);
  CHECK(z.node[boundary].x == z.sub[root.v].w);
  // Only the x moves: the cross-axis assignment still owns the other.
  CHECK(z.node[boundary].y >= 0);
  CHECK(z.node[boundary].y <= z.sub[root.v].h);
}
