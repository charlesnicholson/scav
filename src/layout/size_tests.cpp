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
  REQUIRE(size_layout(c,
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
  REQUIRE(size_layout(c,
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
  REQUIRE(size_layout(c,
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
      size_layout(c, depths(std::vector<uint32_t>(6, 0)), o, {}, unfolded(), flat, diags));
  SizedLayout folded;
  diags.clear();
  REQUIRE(size_layout(c,
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
  REQUIRE(size_layout(
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
  REQUIRE(size_layout(
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
  REQUIRE(size_layout(
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
  REQUIRE(size_layout(
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
  SubmachineOrders const o{ order_submachines(c, g, sp, p) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, o, sp, p, z, diags));

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
  CHECK_FALSE(size_layout(c,
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
  SubmachineOrders const o{ order_submachines(c, g, {}, p) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, o, {}, p, z, diags));

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
  REQUIRE(size_layout(c,
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

TEST_CASE("size: a pseudostate with a band of its own is a container") {
  scav_profile const p{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const bare{ build_state(c, root, "J", StateKind::Junction, {}) };
  StateId const trailing{ build_state(c, root, "K", StateKind::Junction, {}) };
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[trailing.v] = { .min_w = 0, .h_before = 0, .h_after = 24 };
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, order_submachines(c, g, s, p), s, p, z, diags));

  // A band is text the mark has to hold, so the ring the box formula skips for
  // a bare mark is reserved for this one after all.
  uint32_t const junction{ static_cast<uint32_t>(StateKind::Junction) };
  CHECK(z.state[bare.v].w == p.kind_min_w[junction]);
  CHECK(z.state[trailing.v].w == p.kind_min_w[junction] + (2 * p.pad));
  CHECK(z.state[trailing.v].h == p.kind_min_h[junction] + (2 * p.pad));
  CHECK(z.after[trailing.v].w == z.state[trailing.v].w - (2 * p.pad));
  CHECK(z.after[trailing.v].h == 24);
}

TEST_CASE("size: a tombstoned submachine is neither sized nor descended into") {
  scav_profile const p{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const owner{ build_state(c, root, "J", StateKind::Junction, {}) };
  SubmachineId const gone{ build_submachine(c, owner, "gone", {}) };
  build_state(c, gone, "M", StateKind::Normal, {});
  uint32_t const junction{ static_cast<uint32_t>(StateKind::Junction) };

  auto const sized = [&](SizedLayout &z) {
    SplitGraph const g{ decompose(c) };
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, order_submachines(c, g, {}, p), {}, p, z, diags));
  };

  SizedLayout live;
  sized(live);
  REQUIRE(live.sub[gone.v].w > 0);
  CHECK(live.state[owner.v].w == live.sub[gone.v].w + (2 * p.pad));

  // Tombstoned, the submachine is no longer contents: the mark is bare again
  // and the frame keeps the zero rect it was assigned.
  c.submachines[gone.v].live = 0;
  SizedLayout dead;
  sized(dead);
  CHECK(dead.sub[gone.v].w == 0);
  CHECK(dead.sub[gone.v].h == 0);
  CHECK(dead.state[owner.v].w == p.kind_min_w[junction]);
  CHECK(dead.state[owner.v].h == p.kind_min_h[junction]);
  CHECK(dead.before[owner.v].w == dead.state[owner.v].w);
}

TEST_CASE("size: a submachine with height and no width is still contents") {
  // A profile that asks no width of a junction, so the mark inside is a line:
  // the one extent it has is enough to make its frame contents.
  uint32_t const junction{ static_cast<uint32_t>(StateKind::Junction) };
  scav_profile p{ profile() };
  p.kind_min_w[junction] = 0;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const bare{ build_state(c, root, "J", StateKind::Junction, {}) };
  StateId const owner{ build_state(c, root, "K", StateKind::Junction, {}) };
  SubmachineId const inner{ build_submachine(c, owner, "inner", {}) };
  build_state(c, inner, "M", StateKind::Junction, {});

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, order_submachines(c, g, {}, p), {}, p, z, diags));

  CHECK(z.sub[inner.v].w == 0);
  CHECK(z.sub[inner.v].h == p.kind_min_h[junction]);
  // Contents either way round: the mark with nothing inside it stays bare, the
  // one holding this frame takes the ring.
  CHECK(z.state[bare.v].w == 0);
  CHECK(z.state[owner.v].w == 2 * p.pad);
  CHECK(z.state[owner.v].h == p.kind_min_h[junction] + (2 * p.pad));
}

TEST_CASE("size: orders that name nodes but no rank size nothing") {
  scav_profile const p{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  SubmachineOrders o{ one_frame(c, root, { state_node(a.v, 0, 0) }, {}, {}) };
  o.sub_ranks[root.v] = 0;

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      o,
                      {},
                      p,
                      z,
                      diags));

  // The two are one input: a span of nodes with no rank to lay them on is not
  // a frame, and the frame keeps its zero rect rather than a diagnostic.
  CHECK(diags.empty());
  CHECK(z.sub[root.v].w == 0);
  CHECK(z.sub[root.v].h == 0);
  // The box formula still answers for the state itself, which reads no orders.
  CHECK(z.state[a.v].w == p.kind_min_w[0] + (2 * p.pad));
}

TEST_CASE("size: a fold whose pieces will not pack is dropped for the flat run") {
  // Two tall ranks and two wide ones, and no box packer to lay the pieces in a
  // row: stacked they leave the domain, so the fold is dropped for the flat run
  // rather than diagnosed against a frame that has a shape after all.
  scav_profile p{ profile() };
  p.trybox = 0;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> ids;
  ids.reserve(6);
  for (uint32_t i = 0; i < 6; ++i) {
    ids.push_back(build_state(c, root, "S", StateKind::Normal, {}));
  }
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  for (uint32_t const tall : { 0U, 1U, 3U, 4U }) {
    boxes[ids[tall].v] = { .min_w = 0, .h_before = SPACE_MAX, .h_after = 0 };
  }
  for (uint32_t const wide : { 2U, 5U }) {
    boxes[ids[wide].v] = { .min_w = SPACE_MAX, .h_before = 0, .h_after = 0 };
  }
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };

  std::vector<OrderNode> const nodes{
    state_node(ids[0].v, 0, 0), state_node(ids[1].v, 0, 1), state_node(ids[2].v, 1, 0),
    state_node(ids[3].v, 2, 0), state_node(ids[4].v, 2, 1), state_node(ids[5].v, 3, 0)
  };
  std::vector<OrderEdge> const edges{
    { .src = 0, .dst = 2, .segment = 0, .reversed = 0 },
    { .src = 1, .dst = 2, .segment = 1, .reversed = 0 },
    { .src = 2, .dst = 3, .segment = 2, .reversed = 0 },
    { .src = 2, .dst = 4, .segment = 3, .reversed = 0 },
    { .src = 3, .dst = 5, .segment = 4, .reversed = 0 },
    { .src = 4, .dst = 5, .segment = 5, .reversed = 0 }
  };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, nodes, edges, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());

  // Four ranks end to end, each one rank_sep past the last, which is the flat
  // run rather than any arrangement of pieces.
  int32_t const tall_w{ p.kind_min_w[0] + (2 * p.pad) };
  int32_t const wide_w{ SPACE_MAX + (2 * p.pad) };
  CHECK(z.state[ids[0].v].x == 0);
  CHECK(z.state[ids[2].v].x == (tall_w + p.rank_sep));
  CHECK(z.state[ids[3].v].x == (z.state[ids[2].v].x + wide_w + p.rank_sep));
  CHECK(z.state[ids[5].v].x == (z.state[ids[3].v].x + tall_w + p.rank_sep));
  CHECK(z.sub[root.v].w == (z.state[ids[5].v].x + wide_w));
  CHECK(z.sub[root.v].w <= COORD_MAX);
  CHECK(z.sub[root.v].h <= COORD_MAX);
  // Two tall states one node_sep apart is the whole height, so the flat run is
  // one rank deep and nothing wrapped under anything.
  CHECK(z.sub[root.v].h == ((2 * (SPACE_MAX + (2 * p.pad))) + p.node_sep));
}

TEST_CASE("size: a row that leaves the domain does not displace the column that fits") {
  // Two unconnected states, each half the domain wide and half of it tall. The
  // column of the two fits; the row the box packer offers scales larger and
  // does not, so it is no candidate and the frame is measured on the column.
  scav_profile p{ profile() };
  p.pad = 130800;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  std::vector<OrderNode> const nodes{ state_node(a.v, 0, 0), state_node(b.v, 0, 1) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, nodes, {}, {}),
                      {},
                      p,
                      z,
                      diags));
  CHECK(diags.empty());
  CHECK(z.sub[root.v].w == (p.kind_min_w[0] + (2 * p.pad)));
  CHECK(z.sub[root.v].h == ((2 * (p.kind_min_h[0] + (2 * p.pad))) + p.node_sep));
  // The row it declined is the one that leaves the domain.
  CHECK(((2 * (p.kind_min_w[0] + (2 * p.pad))) + p.node_sep) > COORD_MAX);

  // The same extents with no box packer to offer a row at all, which is what
  // makes them the column's.
  p.trybox = 0;
  SizedLayout column;
  diags.clear();
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, nodes, {}, {}),
                      {},
                      p,
                      column,
                      diags));
  CHECK(column.sub[root.v].w == z.sub[root.v].w);
  CHECK(column.sub[root.v].h == z.sub[root.v].h);
}

namespace {

// A frame of unconnected states of one shape: one component each, so the two
// packers see n equal rects and offer a column n tall against a row n wide.
struct EqualStates {
  std::vector<OrderNode> nodes;
  std::vector<scav_box_space> boxes;
};

EqualStates equal_states(Chart &c, SubmachineId root, uint32_t n, int32_t min_w) {
  EqualStates out;
  for (uint32_t i = 0; i < n; ++i) {
    StateId const s{ build_state(c, root, "S", StateKind::Normal, {}) };
    out.nodes.push_back(state_node(s.v, 0, i));
  }
  out.boxes.assign(c.states.size(), scav_box_space{});
  for (OrderNode const &nd : out.nodes) {
    out.boxes[nd.subject] = { .min_w = min_w, .h_before = SPACE_MAX, .h_after = 0 };
  }
  return out;
}

// No ring and no gap, so a rect's extents are the request; a desired aspect
// this tall keeps the width target under two rects, so `pack_lr` stacks them.
scav_profile stacking() {
  scav_profile p{ profile() };
  p.pad = 0;
  p.node_sep = 0;
  p.dar_num = 1;
  p.dar_den = 1024;
  return p;
}

}  // namespace

TEST_CASE("size: a column that leaves the domain gives way to the row that fits") {
  scav_profile p{ stacking() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  EqualStates const five{ equal_states(c, root, 5, 0) };
  scav_spaces const s{ .box_state = five.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(five.boxes.size()) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, five.nodes, {}, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());

  // Five states side by side at one y, which is the row and not the stack.
  CHECK(z.sub[root.v].w == (5 * p.kind_min_w[0]));
  CHECK(z.sub[root.v].h == SPACE_MAX);
  for (uint32_t i = 0; i < five.nodes.size(); ++i) {
    CHECK(z.state[five.nodes[i].subject].x == (static_cast<int32_t>(i) * p.kind_min_w[0]));
    CHECK(z.state[five.nodes[i].subject].y == z.state[five.nodes[0].subject].y);
  }

  // With no box packer to offer it, the column is all there is, and it is the
  // one that leaves the domain.
  p.trybox = 0;
  SizedLayout stacked;
  diags.clear();
  CHECK_FALSE(size_layout(c,
                          depths(std::vector<uint32_t>(c.states.size(), 0)),
                          one_frame(c, root, five.nodes, {}, {}),
                          s,
                          p,
                          stacked,
                          diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::Submachine);
  CHECK(diags[0].subject.ordinal == root.v);
}

TEST_CASE("size: a frame no packing fits is diagnosed, not saturated") {
  // The same five, each as wide as it is tall: the row leaves the domain along
  // x and the column along y, so the frame has no shape rather than a
  // saturated one.
  scav_profile const p{ stacking() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  EqualStates const five{ equal_states(c, root, 5, SPACE_MAX) };
  scav_spaces const s{ .box_state = five.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(five.boxes.size()) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(size_layout(c,
                          depths(std::vector<uint32_t>(c.states.size(), 0)),
                          one_frame(c, root, five.nodes, {}, {}),
                          s,
                          p,
                          z,
                          diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::Submachine);
  CHECK(diags[0].subject.ordinal == root.v);
  CHECK((5 * SPACE_MAX) > COORD_MAX);
  CHECK(z.sub[root.v].w == 0);
  CHECK(z.sub[root.v].h == 0);
}

namespace {

// One state per rank, chained, so a frame is a single component whose rank run
// is what the fold cuts into pieces.
struct RankRun {
  std::vector<OrderNode> nodes;
  std::vector<OrderEdge> edges;
  std::vector<scav_box_space> boxes;
};

RankRun rank_run(Chart &c,
                 SubmachineId root,
                 uint32_t n,
                 int32_t min_w,
                 int32_t h_before) {
  RankRun out;
  for (uint32_t i = 0; i < n; ++i) {
    StateId const s{ build_state(c, root, "S", StateKind::Normal, {}) };
    out.nodes.push_back(state_node(s.v, i, 0));
    if (i > 0) {
      out.edges.push_back({ .src = i - 1, .dst = i, .segment = i - 1, .reversed = 0 });
    }
  }
  out.boxes.assign(c.states.size(), scav_box_space{});
  for (OrderNode const &nd : out.nodes) {
    out.boxes[nd.subject] = { .min_w = min_w, .h_before = h_before, .h_after = 0 };
  }
  return out;
}

}  // namespace

TEST_CASE("size: a row of fold pieces that leaves the domain keeps the column") {
  // Five ranks end to end are wider than the domain, so the flat run has no
  // shape and the fold is the only one on offer. Its pieces in a row are that
  // same width again; stacked they fit, and the stack is what the frame keeps.
  scav_profile p{ profile() };
  p.pad = 0;
  p.rank_sep = 0;
  p.node_sep = 0;
  p.dar_num = 1024;
  p.dar_den = 1;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  RankRun const run{ rank_run(c, root, 5, SPACE_MAX, 0) };
  scav_spaces const s{ .box_state = run.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(run.boxes.size()) };
  CHECK((5 * SPACE_MAX) > COORD_MAX);

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());

  // Three ranks in the first piece and two in the second, one above the other.
  CHECK(z.sub[root.v].w == (3 * SPACE_MAX));
  CHECK(z.sub[root.v].h == (2 * p.kind_min_h[0]));

  // The same shape with no box packer to offer the row at all, which is what
  // makes it the column's.
  p.trybox = 0;
  SizedLayout column;
  diags.clear();
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      column,
                      diags));
  CHECK(column.sub[root.v].w == z.sub[root.v].w);
  CHECK(column.sub[root.v].h == z.sub[root.v].h);
}

TEST_CASE("size: a column of fold pieces that leaves the domain takes the row") {
  // Five ranks of one tall state each. The fold cuts every rank into its own
  // piece; stacked they are five times the domain's height, in a row they are
  // the flat run less the four rank gaps the cuts removed -- the narrower
  // shape, so the fold is taken rather than dropped.
  scav_profile p{ profile() };
  p.pad = 0;
  p.node_sep = 0;
  p.dar_num = 1;
  p.dar_den = 1024;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  RankRun const run{ rank_run(c, root, 5, 0, SPACE_MAX) };
  scav_spaces const s{ .box_state = run.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(run.boxes.size()) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());
  CHECK(z.sub[root.v].w == (5 * p.kind_min_w[0]));
  CHECK(z.sub[root.v].h == SPACE_MAX);
  CHECK((5 * SPACE_MAX) > COORD_MAX);

  // With no box packer the stack is all the fold has, it leaves the domain,
  // and the fold is dropped for the flat run -- four rank gaps wider.
  p.trybox = 0;
  SizedLayout flat;
  diags.clear();
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      flat,
                      diags));
  CHECK(diags.empty());
  CHECK(flat.sub[root.v].w == ((5 * p.kind_min_w[0]) + (4 * p.rank_sep)));
  CHECK(flat.sub[root.v].h == SPACE_MAX);
}

TEST_CASE("size: a fold no packing of the pieces fits is dropped for the flat run") {
  // A node gap wide enough that the pieces leave the domain either way: in a
  // row it is charged four times between them, stacked it is charged four
  // times under them. The flat run pays it neither way and is what is kept.
  scav_profile const p{ [] {
    scav_profile q{ profile() };
    q.pad = 0;
    q.rank_sep = 0;
    q.node_sep = SPACE_MAX;
    q.dar_num = 1;
    q.dar_den = 1024;
    return q;
  }() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  RankRun const run{ rank_run(c, root, 5, 0, SPACE_MAX) };
  scav_spaces const s{ .box_state = run.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(run.boxes.size()) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(std::vector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());
  // Five ranks end to end with no rank gap, which is neither packing of the
  // pieces: the row would be four node gaps wider and leave the domain.
  CHECK(z.sub[root.v].w == (5 * p.kind_min_w[0]));
  CHECK(z.sub[root.v].h == SPACE_MAX);
  CHECK(((5 * p.kind_min_w[0]) + (4 * p.node_sep)) > COORD_MAX);
}

namespace {

// `subs` sibling submachines under one state, each a chain of `ranks` states of
// one shape, so the state's own packing is over that many equal rects.
struct SiblingSubs {
  StateId owner{ INVALID };
  std::vector<scav_box_space> boxes;
};

SiblingSubs sibling_subs(Chart &c,
                         SubmachineId root,
                         uint32_t subs,
                         uint32_t ranks,
                         int32_t min_w,
                         int32_t h_before) {
  SiblingSubs out;
  out.owner = build_state(c, root, "Outer", StateKind::Normal, {});
  for (uint32_t k = 0; k < subs; ++k) {
    SubmachineId const m{ build_submachine(c, out.owner, "r", {}) };
    StateId prev{ INVALID };
    for (uint32_t i = 0; i < ranks; ++i) {
      StateId const at{ build_state(c, m, "S", StateKind::Normal, {}) };
      if (prev.v != INVALID) { build_trans(c, prev, at, TransKind::External, {}); }
      prev = at;
    }
  }
  out.boxes.assign(c.states.size(), scav_box_space{});
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (i == out.owner.v) { continue; }
    out.boxes[i] = { .min_w = min_w, .h_before = h_before, .h_after = 0 };
  }
  return out;
}

}  // namespace

TEST_CASE("size: a sibling row that leaves the domain does not displace the column") {
  // Two regions, each two ranks wide and one state tall, so side by side they
  // are wider than the domain and stacked they are not.
  scav_profile p{ profile() };
  p.pad = 0;
  p.rank_sep = 64;
  p.node_sep = 0;
  p.sub_sep = 0;
  p.dar_num = 1024;
  p.dar_den = 1;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  SiblingSubs const two{ sibling_subs(c, root, 2, 2, SPACE_MAX, SPACE_MAX) };
  scav_spaces const s{ .box_state = two.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(two.boxes.size()) };
  int32_t const region_w{ (2 * SPACE_MAX) + p.rank_sep };
  CHECK(((2 * region_w) + p.sub_sep) > COORD_MAX);

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, order_submachines(c, g, {}, p), s, p, z, diags));
  CHECK(diags.empty());
  CHECK(z.state[two.owner.v].w == region_w);
  CHECK(z.state[two.owner.v].h == ((2 * SPACE_MAX) + p.sub_sep));

  // The same extents with no box packer to offer the row.
  p.trybox = 0;
  SizedLayout column;
  diags.clear();
  REQUIRE(size_layout(c, g, order_submachines(c, g, {}, p), s, p, column, diags));
  CHECK(column.state[two.owner.v].w == z.state[two.owner.v].w);
  CHECK(column.state[two.owner.v].h == z.state[two.owner.v].h);
}

TEST_CASE("size: a sibling column that leaves the domain gives way to the row") {
  // Five regions of one tall state each: stacked they are five times the
  // domain's height, side by side they are five narrow boxes.
  scav_profile p{ profile() };
  p.pad = 0;
  p.sub_sep = 0;
  p.dar_num = 1;
  p.dar_den = 1024;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  SiblingSubs const five{ sibling_subs(c, root, 5, 1, 0, SPACE_MAX) };
  scav_spaces const s{ .box_state = five.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(five.boxes.size()) };

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, order_submachines(c, g, {}, p), s, p, z, diags));
  CHECK(diags.empty());
  CHECK(z.state[five.owner.v].w == (5 * p.kind_min_w[0]));
  CHECK(z.state[five.owner.v].h == SPACE_MAX);

  // With no box packer the stack is all there is, and the state that holds it
  // cannot exist -- which is what makes the run above the row.
  p.trybox = 0;
  SizedLayout stacked;
  diags.clear();
  CHECK_FALSE(size_layout(c, g, order_submachines(c, g, {}, p), s, p, stacked, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::State);
  CHECK(diags[0].subject.ordinal == five.owner.v);
}

TEST_CASE("size: a state no sibling packing fits is charged the overflow") {
  // The same five, each as wide as it is tall: the row leaves the domain along
  // x and the column along y, so the composed box is charged to the state.
  scav_profile const p{ [] {
    scav_profile q{ profile() };
    q.pad = 0;
    q.sub_sep = 0;
    q.dar_num = 1;
    q.dar_den = 1024;
    return q;
  }() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  SiblingSubs const five{ sibling_subs(c, root, 5, 1, SPACE_MAX, SPACE_MAX) };
  scav_spaces const s{ .box_state = five.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(five.boxes.size()) };

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(size_layout(c, g, order_submachines(c, g, {}, p), s, p, z, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::State);
  CHECK(diags[0].subject.ordinal == five.owner.v);
  CHECK((5 * SPACE_MAX) > COORD_MAX);
  CHECK(z.state[five.owner.v].w == 0);
  CHECK(z.state[five.owner.v].h == 0);
}

TEST_CASE("size: a box formula that leaves the domain is charged to the state") {
  // A ring of the widest padding a profile may ask for, around a submachine
  // already most of the domain wide: the state that holds it cannot exist.
  scav_profile p{ profile() };
  p.pad = SPACE_MAX;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const owner{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, owner, "inner", {}) };
  build_state(c, inner, "A", StateKind::Normal, {});

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(size_layout(c, g, order_submachines(c, g, {}, p), {}, p, z, diags));
  REQUIRE(!diags.empty());
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::State);
  CHECK(diags[0].subject.ordinal == owner.v);
  // The submachine underneath was inside the domain, so the state's own ring is
  // what left it.
  CHECK(z.sub[inner.v].w <= COORD_MAX);
  CHECK((z.sub[inner.v].w + (2 * p.pad)) > COORD_MAX);
}

namespace scav {

// The two halves of the owner-hole ratio `size.cpp` brackets with
// SCAV_INTERNAL, declared here rather than in a header so the shipping build
// keeps them internal.
FrameDar size_hole_ratio(int32_t w, int32_t h);
std::vector<FrameDar> size_owner_holes(Chart const &c, SizedLayout const &z);

}  // namespace scav

namespace {

// A profile whose Choice states reserve a hole a hundred thousand units tall,
// so an owner leaves its frame a shape nothing about the frame produced.
scav_profile tall_choice() {
  scav_profile p{ profile() };
  p.kind_min_h[static_cast<uint32_t>(StateKind::Choice)] = 100000;
  return p;
}

// A Choice state owning `kids` unconnected leaves: one hole, and as many
// components in it as there are leaves.
Chart hole_chart(uint32_t kids, StateId &owner, SubmachineId &frame) {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  owner = build_state(c, root, "Owner", StateKind::Choice, {});
  frame = build_submachine(c, owner, "inner", {});
  for (uint32_t i = 0; i < kids; ++i) {
    build_state(c, frame, "K", StateKind::Normal, {});
  }
  return c;
}

// How many distinct x and y origins the frame's children came out at, which is
// the packing's shape as a reader sees it.
void packing_shape(Chart const &c,
                   SizedLayout const &z,
                   SubmachineId frame,
                   uint32_t &columns,
                   uint32_t &rows) {
  std::vector<int32_t> xs;
  std::vector<int32_t> ys;
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (!(c.states[i].parent == frame)) { continue; }
    bool seen_x{ false };
    bool seen_y{ false };
    for (int32_t const at : xs) { seen_x = seen_x || (at == z.state[i].x); }
    for (int32_t const at : ys) { seen_y = seen_y || (at == z.state[i].y); }
    if (!seen_x) { xs.push_back(z.state[i].x); }
    if (!seen_y) { ys.push_back(z.state[i].y); }
  }
  columns = static_cast<uint32_t>(xs.size());
  rows = static_cast<uint32_t>(ys.size());
}

}  // namespace

TEST_CASE("size: a hole's aspect is a ratio inside the profile's own bounds") {
  // Both fields stay in [1, 1024], because that is where pack.cpp proved its
  // products, and the longer axis is the one that takes the cap.
  CHECK(size_hole_ratio(1000, 1000).num == 1024);
  CHECK(size_hole_ratio(1000, 1000).den == 1024);
  CHECK(size_hole_ratio(2000, 1000).num == 1024);
  CHECK(size_hole_ratio(2000, 1000).den == 512);
  CHECK(size_hole_ratio(1000, 2000).num == 512);
  CHECK(size_hole_ratio(1000, 2000).den == 1024);
  // 16:10 is the readable profile's own ratio, and a hole of that shape reads
  // as the same number.
  CHECK(size_hole_ratio(1600, 1000).num == 1024);
  CHECK(size_hole_ratio(1600, 1000).den == 640);

  // A hole a million times longer than it is wide floors at 1 rather than
  // rounding down to no ratio at all, which no profile would validate.
  CHECK(size_hole_ratio(1000000, 1).num == 1024);
  CHECK(size_hole_ratio(1000000, 1).den == 1);
  CHECK(size_hole_ratio(1, 1000000).num == 1);
  CHECK(size_hole_ratio(1, 1000000).den == 1024);

  // No extent on an axis is no aspect: the caller falls back to the profile's.
  CHECK(size_hole_ratio(0, 500).num == 0);
  CHECK(size_hole_ratio(500, 0).num == 0);
  CHECK(size_hole_ratio(-1, 500).num == 0);
}

TEST_CASE("size: only a state with a live frame in it leaves a hole") {
  StateId owner{ INVALID };
  SubmachineId frame{ INVALID };
  Chart c{ hole_chart(4, owner, frame) };
  scav_profile const p{ tall_choice() };

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, order_submachines(c, g, {}, p), {}, p, z, diags));

  std::vector<FrameDar> const hole{ size_owner_holes(c, z) };
  REQUIRE(hole.size() == c.states.size());
  // The owner reserves 100,000 units of height for a frame a fraction of that
  // tall, so the hole it leaves is far taller than it is wide.
  CHECK(hole[owner.v].num < hole[owner.v].den);
  CHECK(hole[owner.v].den == 1024);
  // A leaf holds no frame, so nothing is packed into it.
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (i != owner.v) { CHECK(hole[i].num == 0); }
  }

  // A tombstoned frame is no hole either, the state having nothing left to pack.
  c.submachines[frame.v].live = 0;
  CHECK(size_owner_holes(c, z)[owner.v].num == 0);
}

TEST_CASE("size: a frame handed its owner's hole packs to that shape") {
  // The hole is tall and narrow and the profile's ratio is 16:10, so the same
  // four components come out two by two at the one and in a column at the other.
  StateId owner{ INVALID };
  SubmachineId frame{ INVALID };
  Chart c{ hole_chart(4, owner, frame) };
  scav_profile const p{ tall_choice() };
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ order_submachines(c, g, {}, p) };

  SizedLayout flat;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, o, {}, p, flat, diags, DarSource::Profile));
  uint32_t columns{ 0 };
  uint32_t rows{ 0 };
  packing_shape(c, flat, frame, columns, rows);
  CHECK(columns == 2);
  CHECK(rows == 2);

  SizedLayout handed;
  REQUIRE(size_layout(c, g, o, {}, p, handed, diags, DarSource::OwnerHole));
  packing_shape(c, handed, frame, columns, rows);
  CHECK(columns == 1);
  CHECK(rows == 4);
  CHECK(handed.sub[frame.v].w < flat.sub[frame.v].w);
  CHECK(handed.sub[frame.v].h > flat.sub[frame.v].h);
}

TEST_CASE("size: a root frame has no owner hole and keeps the profile's ratio") {
  // Every packing in this chart is the root frame's own, so handing the ratios
  // down cannot reach one and both passes agree rect for rect.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  for (uint32_t i = 0; i < 5; ++i) { build_state(c, root, "K", StateKind::Normal, {}); }
  scav_profile const p{ profile() };
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ order_submachines(c, g, {}, p) };

  SizedLayout flat;
  SizedLayout handed;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, o, {}, p, flat, diags, DarSource::Profile));
  REQUIRE(size_layout(c, g, o, {}, p, handed, diags, DarSource::OwnerHole));
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    CHECK((flat.state[i] == handed.state[i]));
  }
  CHECK((flat.chart == handed.chart));
}

TEST_CASE("size: a first pass that leaves the domain ends the handed-down one") {
  // The failure is the same failure and it is reported once: the second pass
  // never runs, so nothing can diagnose the same state twice.
  scav_profile p{ profile() };
  p.pad = SPACE_MAX;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const owner{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, owner, "inner", {}) };
  build_state(c, inner, "A", StateKind::Normal, {});

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(size_layout(c,
                          g,
                          order_submachines(c, g, {}, p),
                          {},
                          p,
                          z,
                          diags,
                          DarSource::OwnerHole));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.ordinal == owner.v);
}
