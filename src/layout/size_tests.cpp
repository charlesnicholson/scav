// `size_layout` tests, most on hand-written orders.

#include "layout/size.h"

#include "layout/decompose.h"
#include "layout/label.h"
#include "layout/order.h"
#include "layout/router.h"
#include "layout/tests/trace_record.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"

#include "doctest.h"
#include "scav_pod_vector.h"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace scav;

constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

scav_profile profile() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

// Rank runs stay flat at a 1024:1 desired aspect.
scav_profile unfolded() {
  scav_profile p{ profile() };
  p.dar_num = 1024;
  p.dar_den = 1;
  return p;
}

// Only `state_depth` set: sizing finds no segments, hence no ports and no label boxes.
SplitGraph depths(PodVector<uint32_t> const &state_depth) {
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
                           PodVector<OrderNode> nodes,
                           PodVector<OrderEdge> edges,
                           PodVector<int32_t> gaps) {
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
  build_trans(c, a, b, TransKind::Default, {});
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
  build_trans(c, a, b, TransKind::Default, {});
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
  // Six chained ranks, sized flat at 1024:1 and folded at the readable 16:10.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  PodVector<OrderNode> nodes;
  PodVector<OrderEdge> edges;
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
      size_layout(c, depths(PodVector<uint32_t>(6, 0)), o, {}, unfolded(), flat, diags));
  SizedLayout folded;
  diags.clear();
  REQUIRE(
      size_layout(c, depths(PodVector<uint32_t>(6, 0)), o, {}, profile(), folded, diags));

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
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, a, d, TransKind::Default, {});
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

TEST_CASE("size: an edge within one rank stacks its ends rather than aligning them") {
  // Both edges join two nodes in rank 0, as a rank pin can leave them.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, a, TransKind::Default, {});
  scav_profile const p{ unfolded() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths({ 0, 0 }),
                      one_frame(c,
                                root,
                                { state_node(a.v, 0, 0), state_node(b.v, 0, 1) },
                                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                                  { .src = 1, .dst = 0, .segment = 1, .reversed = 0 } },
                                { 0 }),
                      {},
                      p,
                      z,
                      diags));
  CHECK(z.state[a.v].x == z.state[b.v].x);
  CHECK(z.state[b.v].y == z.state[a.v].y + z.state[a.v].h + p.node_sep);
}

TEST_CASE("size: states joined inside one column share the widest one's centre line") {
  // The bar `F` keeps the column's leading edge.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const wide{ build_state(c, root, "W", StateKind::Normal, {}) };
  StateId const narrow{ build_state(c, root, "N", StateKind::Normal, {}) };
  StateId const bar{ build_state(c, root, "F", StateKind::Fork, {}) };
  build_trans(c, narrow, wide, TransKind::Default, {});
  build_trans(c, wide, narrow, TransKind::Default, {});
  build_trans(c, wide, bar, TransKind::Default, {});
  scav_profile const p{ unfolded() };
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[wide.v].min_w = 2000;
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()),
                       .box_state_stride = sizeof(scav_box_space) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  TraceRecord t;
  trace_sink_set(&t);
  bool const sized{ size_layout(
      c,
      depths({ 0, 0, 0 }),
      one_frame(c,
                root,
                { state_node(wide.v, 0, 0),
                  state_node(narrow.v, 0, 1),
                  state_node(bar.v, 0, 2) },
                { { .src = 1, .dst = 0, .segment = 0, .reversed = 0 },
                  { .src = 0, .dst = 1, .segment = 1, .reversed = 0 },
                  { .src = 0, .dst = 2, .segment = 2, .reversed = 0 } },
                { 0 }),
      s,
      p,
      z,
      diags) };
  trace_sink_set(nullptr);
  REQUIRE(sized);
  scav_rect const &w{ z.state[wide.v] };
  scav_rect const &n{ z.state[narrow.v] };
  REQUIRE(w.w > (2 * n.w));  // `W` is over twice `N`'s width
  CHECK(n.x + (n.w / 2) == w.x + (w.w / 2));
  CHECK(z.state[bar.v].x == w.x);
  // One ColumnCentred event, for `N`, by its offset from `W`'s leading edge.
  uint32_t centred{ 0 };
  for (TraceEvent const &e : t.events()) {
    if (e.kind != TraceKind::ColumnCentred) { continue; }
    ++centred;
    CHECK(e.shift.state == narrow.v);
    CHECK(e.shift.by == n.x - w.x);
  }
  CHECK(centred == 1);
}

TEST_CASE("size: two labelled edges inside one column widen it for a label either side") {
  // One label takes its room on the trailing side when the leading side is short.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, a, TransKind::Default, {});
  SplitGraph const g{ decompose(c) };
  scav_profile const p{ unfolded() };
  constexpr int32_t LABEL_W{ 1500 };
  std::vector<scav_path_box> boxes{ { .subject = 0, .w = LABEL_W, .h = 200, .order = 0 },
                                    { .subject = 1, .w = LABEL_W, .h = 200, .order = 0 } };
  auto const sized = [&](uint32_t labels, SizedLayout &z) {
    scav_spaces const s{ .path_box = boxes.data(), .n_path_box = labels };
    std::vector<Diagnostic> diags;
    return size_layout(c,
                       g,
                       one_frame(c,
                                 root,
                                 { state_node(a.v, 0, 0), state_node(b.v, 0, 1) },
                                 { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                                   { .src = 1, .dst = 0, .segment = 1, .reversed = 0 } },
                                 { 0 }),
                       s,
                       p,
                       z,
                       diags);
  };

  SizedLayout pair;
  REQUIRE(sized(2, pair));
  int32_t const line{ (2 * (label_leader(p) + LABEL_W)) + p.node_sep };
  scav_rect const &pa{ pair.state[a.v] };
  REQUIRE(line > (2 * pa.w));  // the two-label line is over twice `A`'s width
  CHECK(pair.sub[root.v].w >= line);
  CHECK(pa.x + (pa.w / 2) == pair.state[b.v].x + (pair.state[b.v].w / 2));
  int32_t const off{ (pa.x + (pa.w / 2)) - (line / 2) };
  CHECK(imax(off, -off) <= 1);

  SizedLayout one;
  REQUIRE(sized(1, one));
  scav_rect const &oa{ one.state[a.v] };
  int32_t const room{ label_leader(p) + LABEL_W + (p.node_sep / 2) };
  CHECK(oa.x == one.state[b.v].x);  // one column at the states' width
  CHECK(one.sub[root.v].w == (oa.w / 2) + room);
}

namespace {

// A 1:1024 desired aspect: a fold stacks its pieces one above the other.
scav_profile tall() {
  scav_profile p{ profile() };
  p.dar_num = 1;
  p.dar_den = 1024;
  return p;
}

}  // namespace

TEST_CASE("size: a fold that stacks its pieces carries no label room onto the second") {
  // A cut's label room goes on the next piece's leading edge only when packed beside the
  // one before; stacked, `B` keeps `A`'s x.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  std::vector<scav_path_box> const labels{
    { .subject = 0, .w = 1500, .h = 200, .order = 0 }
  };
  scav_spaces const s{ .path_box = labels.data(), .n_path_box = 1 };
  scav_profile const p{ tall() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  TraceRecord t;
  trace_sink_set(&t);
  bool const sized{ size_layout(
      c,
      decompose(c),
      one_frame(c,
                root,
                { state_node(a.v, 0, 0), state_node(b.v, 1, 0) },
                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } },
                { 1500 }),
      s,
      p,
      z,
      diags,
      DarSource::Profile,
      Compaction::Off,
      Fold::Always) };
  trace_sink_set(nullptr);
  REQUIRE(sized);
  bool cut{ false };
  for (TraceEvent const &e : t.events()) {
    if ((e.kind == TraceKind::FoldCut) && (e.fold.refused == 0)) {
      cut = true;
      CHECK(e.fold.carried == 0);
    }
  }
  REQUIRE(cut);                                             // the fold made a cut
  CHECK(z.state[b.v].y > z.state[a.v].y + z.state[a.v].h);  // stacked
  CHECK(z.state[b.v].x == z.state[a.v].x);
}

TEST_CASE("size: a fold never cuts a boundary node away from the node it joins") {
  // The fold refuses the cut before rank 2; the boundary there stays level with `B`.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[a.v].min_w = 3000;
  boxes[b.v].min_w = 3000;
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()),
                       .box_state_stride = sizeof(scav_box_space) };
  scav_profile const p{ tall() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  TraceRecord t;
  trace_sink_set(&t);
  bool const sized{ size_layout(
      c,
      depths({ 0, 0 }),
      one_frame(c,
                root,
                { state_node(a.v, 0, 0),
                  state_node(b.v, 1, 0),
                  { .kind = OrderKind::Boundary, .subject = 1, .rank = 2, .pos = 0 } },
                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                  { .src = 1, .dst = 2, .segment = 1, .reversed = 0 } },
                { 0, 0 }),
      s,
      p,
      z,
      diags,
      DarSource::Profile,
      Compaction::Off,
      Fold::Always) };
  trace_sink_set(nullptr);
  REQUIRE(sized);
  bool refused{ false };
  for (TraceEvent const &e : t.events()) {
    refused = refused || ((e.kind == TraceKind::FoldCut) && (e.fold.rank == 2) &&
                          (e.fold.refused != 0));
  }
  CHECK(refused);
  CHECK(z.node[2].y == z.state[b.v].y + (z.state[b.v].h / 2));
}

TEST_CASE("size: a labelled pair a fold stacks has room for a label either side") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, a, TransKind::Default, {});
  constexpr int32_t LABEL_W{ 1500 };
  std::vector<scav_path_box> const labels{
    { .subject = 0, .w = LABEL_W, .h = 200, .order = 0 },
    { .subject = 1, .w = LABEL_W, .h = 200, .order = 0 }
  };
  scav_spaces const s{ .path_box = labels.data(), .n_path_box = 2 };
  scav_profile const p{ tall() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      decompose(c),
                      one_frame(c,
                                root,
                                { state_node(a.v, 0, 0), state_node(b.v, 1, 0) },
                                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                                  { .src = 1, .dst = 0, .segment = 1, .reversed = 0 } },
                                { 1500 }),
                      s,
                      p,
                      z,
                      diags,
                      DarSource::Profile,
                      Compaction::Off,
                      Fold::Always));
  scav_rect const &ra{ z.state[a.v] };
  scav_rect const &rb{ z.state[b.v] };
  REQUIRE(rb.y > ra.y + ra.h);  // stacked
  int32_t const lo{ imax(ra.x, rb.x) };
  int32_t const hi{ imin(ra.x + ra.w, rb.x + rb.w) };
  REQUIRE(hi > lo);
  int32_t const leg{ lo + ((hi - lo) / 2) };
  int32_t const room{ label_leader(p) + LABEL_W + (p.node_sep / 2) };
  scav_rect const &frame{ z.sub[root.v] };
  CHECK(leg - frame.x >= room);
  CHECK((frame.x + frame.w) - leg >= room);
}

TEST_CASE(
    "size: a labelled leg passing a column to a stacked piece has its room beside it") {
  // `Clear` over `Tripped` in one column, `Latched` folded under it; the labelled back
  // edge's leg runs up the column's leading side, between it and the initial.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const clear{ build_state(c, root, "Clear", StateKind::Normal, {}) };
  StateId const tripped{ build_state(c, root, "Tripped", StateKind::Normal, {}) };
  StateId const latched{ build_state(c, root, "Latched", StateKind::Normal, {}) };
  build_trans(c, start, clear, TransKind::Default, {});
  build_trans(c, clear, tripped, TransKind::Default, {});
  build_trans(c, tripped, latched, TransKind::Default, {});
  build_trans(c, latched, clear, TransKind::Default, {});
  constexpr int32_t LABEL_W{ 1500 };
  constexpr int32_t INNER_W{ 700 };
  std::vector<scav_path_box> const labels{
    { .subject = 1, .w = INNER_W, .h = 200, .order = 0 },
    { .subject = 3, .w = LABEL_W, .h = 200, .order = 0 }
  };
  scav_spaces const s{ .path_box = labels.data(), .n_path_box = 2 };
  scav_profile const p{ tall() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      decompose(c),
                      one_frame(c,
                                root,
                                { state_node(start.v, 0, 0),
                                  state_node(clear.v, 1, 0),
                                  state_node(tripped.v, 1, 1),
                                  state_node(latched.v, 2, 0) },
                                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                                  { .src = 1, .dst = 2, .segment = 1, .reversed = 0 },
                                  { .src = 3, .dst = 2, .segment = 2, .reversed = 1 },
                                  { .src = 1, .dst = 3, .segment = 3, .reversed = 1 } },
                                { 0, LABEL_W }),
                      s,
                      p,
                      z,
                      diags,
                      DarSource::Profile,
                      Compaction::Off,
                      Fold::Always));
  scav_rect const &rs{ z.state[start.v] };
  scav_rect const &rc{ z.state[clear.v] };
  scav_rect const &rt{ z.state[tripped.v] };
  scav_rect const &rl{ z.state[latched.v] };
  REQUIRE(rt.y > rc.y + rc.h);  // one column
  REQUIRE(rl.y > rt.y + rt.h);  // stacked under it
  int32_t const room{ box_clearance(p) + label_leader(p) + LABEL_W + (p.node_sep / 2) };
  CHECK(imin(rc.x, rt.x) - (rs.x + rs.w) >= room);
  // The labelled edge inside the column has its label's room on the leg's trailing side.
  int32_t const lo{ imax(rc.x, rt.x) };
  int32_t const leg{ lo + ((imin(rc.x + rc.w, rt.x + rt.w) - lo) / 2) };
  scav_rect const &frame{ z.sub[root.v] };
  CHECK((frame.x + frame.w) - leg >= label_leader(p) + INNER_W + (p.node_sep / 2));
}

TEST_CASE("size: a frame turned down runs its ranks top to bottom, and never folds") {
  // The initial, `A`, `B` and `C` run top to bottom on one centre line. A 1024:1 desired
  // aspect under `Fold::Always` leaves the frame one column.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, start, a, TransKind::Default, {});
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, d, TransKind::Default, {});
  scav_profile p{ profile() };
  p.dar_num = 1024;
  p.dar_den = 1;
  SubmachineOrders o{ one_frame(c,
                                root,
                                { state_node(start.v, 0, 0),
                                  state_node(a.v, 1, 0),
                                  state_node(b.v, 2, 0),
                                  state_node(d.v, 3, 0) },
                                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                                  { .src = 1, .dst = 2, .segment = 1, .reversed = 0 },
                                  { .src = 2, .dst = 3, .segment = 2, .reversed = 0 } },
                                { 0, 0, 0 }) };
  o.sub_down.assign(c.submachines.size(), 0);
  o.sub_down[root.v] = 1;

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths({ 0, 0, 0, 0 }),
                      o,
                      {},
                      p,
                      z,
                      diags,
                      DarSource::Profile,
                      Compaction::Off,
                      Fold::Always));
  scav_rect const &dot{ z.state[start.v] };
  scav_rect const &ra{ z.state[a.v] };
  scav_rect const &rb{ z.state[b.v] };
  scav_rect const &rd{ z.state[d.v] };
  CHECK(dot.y + dot.h < ra.y);
  CHECK(ra.y + ra.h < rb.y);
  CHECK(rb.y + rb.h < rd.y);
  CHECK(dot.x + (dot.w / 2) == ra.x + (ra.w / 2));
  CHECK(ra.x + (ra.w / 2) == rb.x + (rb.w / 2));
  CHECK(rb.x + (rb.w / 2) == rd.x + (rd.w / 2));
  CHECK(rb.y - (ra.y + ra.h) == p.rank_sep);
  CHECK(z.sub[root.v].h > (3 * z.sub[root.v].w));  // one column
}

TEST_CASE("size: a bar lies down in a frame running down") {
  // In a frame running down, a bar's kind minimum swaps its width and height.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const bar{ build_state(c, root, "F", StateKind::Fork, {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, bar, a, TransKind::Default, {});
  scav_profile const p{ unfolded() };
  auto const sized = [&](bool down) {
    SubmachineOrders o{ one_frame(c,
                                  root,
                                  { state_node(bar.v, 0, 0), state_node(a.v, 1, 0) },
                                  { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } },
                                  { 0 }) };
    o.sub_down.assign(c.submachines.size(), down ? 1 : 0);
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, depths({ 0, 0 }), o, {}, p, z, diags));
    return z.state[bar.v];
  };
  scav_rect const across{ sized(false) };
  scav_rect const down{ sized(true) };
  REQUIRE(across.h > across.w);  // stood up for a frame running across
  CHECK(down.w == across.h);
  CHECK(down.h == across.w);
}

TEST_CASE("size: a label beside a leg between two ranks has its room on one side") {
  // Running down, the frame grows on its trailing side until that side of the leg holds
  // the whole label.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  constexpr int32_t LABEL_W{ 3000 };
  std::vector<scav_path_box> const labels{
    { .subject = 0, .w = LABEL_W, .h = 200, .order = 0 }
  };
  scav_spaces const s{ .path_box = labels.data(), .n_path_box = 1 };
  scav_profile const p{ unfolded() };
  SplitGraph const g{ decompose(c) };
  SubmachineOrders o{ one_frame(c,
                                root,
                                { state_node(a.v, 0, 0), state_node(b.v, 1, 0) },
                                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } },
                                { 200 }) };
  o.sub_down.assign(c.submachines.size(), 0);
  o.sub_down[root.v] = 1;

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, o, s, p, z, diags));
  scav_rect const &ra{ z.state[a.v] };
  scav_rect const &frame{ z.sub[root.v] };
  int32_t const leg{ ra.x + (ra.w / 2) };
  int32_t const room{ label_leader(p) + LABEL_W + (p.node_sep / 2) };
  CHECK(ra.x == frame.x);  // a node alone in its row takes zero reserve
  CHECK((frame.x + frame.w) - leg >= room);
  CHECK((frame.x + frame.w) - leg < room + ra.w);  // room on one side only
}

TEST_CASE("size: an edge pointing back a rank still aligns its ends") {
  // Both edges run from rank 1 back to rank 0; alignment reads them with ends swapped.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, b, a, TransKind::Default, {});
  build_trans(c, d, a, TransKind::Default, {});
  scav_profile const p{ unfolded() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(
      c,
      depths({ 0, 0, 0 }),
      one_frame(c,
                root,
                { state_node(a.v, 0, 0), state_node(b.v, 1, 0), state_node(d.v, 1, 1) },
                { { .src = 1, .dst = 0, .segment = 0, .reversed = 0 },
                  { .src = 2, .dst = 0, .segment = 1, .reversed = 0 } },
                { 0 }),
      {},
      p,
      z,
      diags));
  // `A`'s centre lies between `B`'s and `D`'s, as with the edges written forward.
  int32_t const mid_a{ z.state[a.v].y + (z.state[a.v].h / 2) };
  int32_t const mid_b{ z.state[b.v].y + (z.state[b.v].h / 2) };
  int32_t const mid_d{ z.state[d.v].y + (z.state[d.v].h / 2) };
  CHECK(mid_a >= mid_b);
  CHECK(mid_a <= mid_d);
  CHECK(z.state[b.v].y + z.state[b.v].h + p.node_sep <= z.state[d.v].y);
}

TEST_CASE("size: an initial pseudostate sits one rank gap before its target") {
  // `W` shares `X`'s height, so the dot clears `W` by half a `node_sep` and the step to
  // `X`'s layer grows to hold it.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const wide{ build_state(c, root, "W", StateKind::Normal, {}) };
  StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, start, x, TransKind::Default, {});
  build_trans(c, wide, x, TransKind::Default, {});
  scav_profile const p{ unfolded() };
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[wide.v].min_w = 2000;
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()),
                       .box_state_stride = sizeof(scav_box_space) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(
      c,
      depths({ 0, 0, 0 }),
      one_frame(
          c,
          root,
          { state_node(start.v, 0, 0), state_node(wide.v, 0, 1), state_node(x.v, 1, 0) },
          { { .src = 0, .dst = 2, .segment = 0, .reversed = 0 },
            { .src = 1, .dst = 2, .segment = 1, .reversed = 0 } },
          { 0 }),
      s,
      p,
      z,
      diags));
  scav_rect const &dot{ z.state[start.v] };
  scav_rect const &w{ z.state[wide.v] };
  CHECK(w.w > (10 * dot.w));  // `W` is over ten dots wide
  CHECK(dot.y > z.state[x.v].y);
  CHECK((dot.y + dot.h) < (z.state[x.v].y + z.state[x.v].h));
  CHECK(dot.x == w.x + w.w + (p.node_sep / 2));
  CHECK(z.state[x.v].x - (dot.x + dot.w) == p.rank_sep);
}

TEST_CASE("size: an initial pseudostate is level with its target where there is room") {
  // `X` has two successors and shares `Z` with `V`; the initial, alone in rank 0, moves
  // level with `X`.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
  StateId const v{ build_state(c, root, "V", StateKind::Normal, {}) };
  StateId const y{ build_state(c, root, "Y", StateKind::Normal, {}) };
  StateId const z2{ build_state(c, root, "Z", StateKind::Normal, {}) };
  build_trans(c, start, x, TransKind::Default, {});
  build_trans(c, x, y, TransKind::Default, {});
  build_trans(c, x, z2, TransKind::Default, {});
  build_trans(c, v, z2, TransKind::Default, {});
  scav_profile const p{ unfolded() };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths({ 0, 0, 0, 0, 0 }),
                      one_frame(c,
                                root,
                                { state_node(start.v, 0, 0),
                                  state_node(x.v, 1, 0),
                                  state_node(v.v, 1, 1),
                                  state_node(y.v, 2, 0),
                                  state_node(z2.v, 2, 1) },
                                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                                  { .src = 1, .dst = 3, .segment = 1, .reversed = 0 },
                                  { .src = 1, .dst = 4, .segment = 2, .reversed = 0 },
                                  { .src = 2, .dst = 4, .segment = 3, .reversed = 0 } },
                                { 0, 0 }),
                      {},
                      p,
                      z,
                      diags));
  scav_rect const &dot{ z.state[start.v] };
  CHECK(dot.y + (dot.h / 2) == z.state[x.v].y + (z.state[x.v].h / 2));
}

TEST_CASE("size: a final pseudostate sits rank_sep after its source, level with it") {
  // The wide `W` shares the source's layer.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
  StateId const wide{ build_state(c, root, "W", StateKind::Normal, {}) };
  StateId const done{ build_state(c, root, {}, StateKind::Final, {}) };
  StateId const y{ build_state(c, root, "Y", StateKind::Normal, {}) };
  build_trans(c, x, done, TransKind::Default, {});
  build_trans(c, wide, y, TransKind::Default, {});
  scav_profile const p{ unfolded() };
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[wide.v].min_w = 2000;
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()),
                       .box_state_stride = sizeof(scav_box_space) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths({ 0, 0, 0, 0 }),
                      one_frame(c,
                                root,
                                { state_node(x.v, 0, 0),
                                  state_node(wide.v, 0, 1),
                                  state_node(done.v, 1, 0),
                                  state_node(y.v, 1, 1) },
                                { { .src = 0, .dst = 2, .segment = 0, .reversed = 0 },
                                  { .src = 1, .dst = 3, .segment = 1, .reversed = 0 } },
                                { 0 }),
                      s,
                      p,
                      z,
                      diags));
  scav_rect const &from{ z.state[x.v] };
  scav_rect const &dot{ z.state[done.v] };
  CHECK(z.state[wide.v].w > (2 * from.w));  // `W` is over twice `X`'s width
  CHECK(dot.x == from.x + from.w + p.rank_sep);
  CHECK(dot.y + (dot.h / 2) == from.y + (from.h / 2));
}

TEST_CASE(
    "size: an initial ranked after its target sits rank_sep after it, level with it") {
  // Its edge reversed, the dot follows `X` as a final follows its source, in the room the
  // wide `W` leaves in their layer.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
  StateId const wide{ build_state(c, root, "W", StateKind::Normal, {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const y{ build_state(c, root, "Y", StateKind::Normal, {}) };
  build_trans(c, start, x, TransKind::Default, {});
  build_trans(c, x, y, TransKind::Default, {});
  build_trans(c, wide, y, TransKind::Default, {});
  scav_profile const p{ unfolded() };
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[wide.v].min_w = 2000;
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()),
                       .box_state_stride = sizeof(scav_box_space) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths({ 0, 0, 0, 0 }),
                      one_frame(c,
                                root,
                                { state_node(x.v, 0, 0),
                                  state_node(wide.v, 0, 1),
                                  state_node(start.v, 1, 0),
                                  state_node(y.v, 1, 1) },
                                { { .src = 0, .dst = 2, .segment = 0, .reversed = 1 },
                                  { .src = 0, .dst = 3, .segment = 1, .reversed = 0 },
                                  { .src = 1, .dst = 3, .segment = 2, .reversed = 0 } },
                                { 0 }),
                      s,
                      p,
                      z,
                      diags));
  scav_rect const &to{ z.state[x.v] };
  scav_rect const &dot{ z.state[start.v] };
  scav_rect const &w{ z.state[wide.v] };
  CHECK(w.x == to.x);
  CHECK(w.w > (2 * to.w));  // `W` is over twice `X`'s width
  CHECK(dot.x == to.x + to.w + p.rank_sep);
  CHECK(dot.y + (dot.h / 2) == to.y + (to.h / 2));
}

TEST_CASE("size: a fold never cuts between an initial pseudostate and its target") {
  // `S0`'s 3000 minimum width makes the 1:1 fold want a cut before it, at rank 1.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  std::vector<StateId> chain;
  chain.reserve(6);
  for (uint32_t i = 0; i < 6; ++i) {
    chain.push_back(build_state(c, root, "S" + std::to_string(i), StateKind::Normal, {}));
  }
  build_trans(c, start, chain[0], TransKind::Default, {});
  PodVector<OrderNode> nodes{ state_node(start.v, 0, 0) };
  PodVector<OrderEdge> edges{ { .src = 0, .dst = 1, .segment = 0, .reversed = 0 } };
  for (uint32_t i = 0; i < chain.size(); ++i) {
    nodes.push_back(state_node(chain[i].v, i + 1, 0));
    if (i + 1 < chain.size()) {
      build_trans(c, chain[i], chain[i + 1], TransKind::Default, {});
      edges.push_back({ .src = i + 1, .dst = i + 2, .segment = i + 1, .reversed = 0 });
    }
  }
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[chain[0].v].min_w = 3000;
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()),
                       .box_state_stride = sizeof(scav_box_space) };
  scav_profile p{ profile() };
  p.dar_num = 1;
  p.dar_den = 1;

  SizedLayout z;
  std::vector<Diagnostic> diags;
  TraceRecord t;
  trace_sink_set(&t);
  bool const sized{ size_layout(c,
                                depths(PodVector<uint32_t>(c.states.size(), 0)),
                                one_frame(c, root, nodes, edges, PodVector<int32_t>(7, 0)),
                                s,
                                p,
                                z,
                                diags,
                                DarSource::Profile,
                                Compaction::Off,
                                Fold::Always) };
  trace_sink_set(nullptr);
  REQUIRE(sized);
  // The fold refused a cut at rank 1, before `S0`.
  bool refused{ false };
  for (TraceEvent const &e : t.events()) {
    refused = refused || ((e.kind == TraceKind::FoldCut) && (e.fold.rank == 1) &&
                          (e.fold.refused != 0));
  }
  CHECK(refused);
  scav_rect const &dot{ z.state[start.v] };
  scav_rect const &a{ z.state[chain[0].v] };
  // The run folds: some chain state's y differs from `S0`'s.
  bool folded{ false };
  for (StateId const st : chain) { folded = folded || (z.state[st.v].y != a.y); }
  CHECK(folded);
  CHECK(a.x - (dot.x + dot.w) == p.rank_sep);
  CHECK(dot.y + (dot.h / 2) == a.y + (a.h / 2));
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
  // Both in rank 0; the packer sets the two components side by side, `node_sep` apart.
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
  // A route leaving `A` makes the boundary a sink, placed on the frame's trailing edge.
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

  // The route arriving instead makes it a source, on the leading edge, left of `A`.
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

TEST_CASE("size: a layer of boundary nodes keeps a route's clearance, not a rank gap") {
  // Boundary nodes at ranks 0 and 3 carry one route through `A` and `B`.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  scav_profile const p{ unfolded() };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(
      c,
      depths({ 0, 0 }),
      one_frame(c,
                root,
                { { .kind = OrderKind::Boundary, .subject = 0, .rank = 0, .pos = 0 },
                  state_node(a.v, 1, 0),
                  state_node(b.v, 2, 0),
                  { .kind = OrderKind::Boundary, .subject = 2, .rank = 3, .pos = 0 } },
                { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
                  { .src = 1, .dst = 2, .segment = 1, .reversed = 0 },
                  { .src = 2, .dst = 3, .segment = 2, .reversed = 0 } },
                { 0, 0, 0 }),
      {},
      p,
      z,
      diags));
  CHECK(z.node[0].x == 0);
  CHECK(z.state[a.v].x == box_clearance(p));
  CHECK(z.state[b.v].x == (z.state[a.v].x + z.state[a.v].w + p.rank_sep));
  CHECK(z.sub[root.v].w == (z.state[b.v].x + z.state[b.v].w + box_clearance(p)));
  CHECK(z.node[3].x == z.sub[root.v].w);
}

TEST_CASE("size: a boundary holds a lane for each edge that turns in it, and no other") {
  // Two states meet the bar's long face straight and take no lane. Of three into one
  // state, the middle runs straight and the two either side turn, holding two lanes.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> ids;
  ids.reserve(5);
  for (uint32_t i = 0; i < 5; ++i) {
    ids.push_back(build_state(c, root, "S", StateKind::Normal, {}));
  }
  StateId const bar{ build_state(c, root, "F", StateKind::Fork, {}) };
  scav_profile const p{ unfolded() };
  auto const x_of = [&](PodVector<OrderNode> const &nodes,
                        PodVector<OrderEdge> const &edges,
                        uint32_t st) {
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c,
                        depths(PodVector<uint32_t>(6, 0)),
                        one_frame(c, root, nodes, edges, { 0 }),
                        {},
                        p,
                        z,
                        diags));
    return z.state[st].x - (z.state[ids[0].v].x + z.state[ids[0].v].w);
  };
  PodVector<OrderNode> const pair{ state_node(ids[0].v, 0, 0),
                                   state_node(ids[1].v, 0, 1),
                                   state_node(bar.v, 1, 0) };
  CHECK(x_of(pair,
             { { .src = 0, .dst = 2, .segment = 0, .reversed = 0 },
               { .src = 1, .dst = 2, .segment = 1, .reversed = 0 } },
             bar.v) == p.rank_sep);
  PodVector<OrderNode> const fan{ state_node(ids[0].v, 0, 0),
                                  state_node(ids[1].v, 0, 1),
                                  state_node(ids[4].v, 0, 2),
                                  state_node(ids[2].v, 1, 0) };
  CHECK(x_of(fan,
             { { .src = 0, .dst = 3, .segment = 0, .reversed = 0 },
               { .src = 1, .dst = 3, .segment = 1, .reversed = 0 },
               { .src = 2, .dst = 3, .segment = 2, .reversed = 0 } },
             ids[2].v) == (p.rank_sep + (2 * label_line_height(p))));
}

TEST_CASE(
    "size: a start state seated inside its target's column takes no room in its own") {
  // `T` centres in its column beside the wide `W`, and its dot `rank_sep` before it lies
  // in that column.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const t{ build_state(c, root, "T", StateKind::Normal, {}) };
  StateId const w{ build_state(c, root, "W", StateKind::Normal, {}) };
  scav_profile const p{ unfolded() };
  for (int32_t const wide : { 3000, 0 }) {
    CAPTURE(wide);
    std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
    boxes[w.v].min_w = wide;
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()),
                         .box_state_stride = sizeof(scav_box_space) };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(
        c,
        depths({ 0, 0, 0 }),
        one_frame(
            c,
            root,
            { state_node(start.v, 0, 0), state_node(t.v, 1, 0), state_node(w.v, 1, 1) },
            { { .src = 0, .dst = 1, .segment = 0, .reversed = 0 },
              { .src = 1, .dst = 2, .segment = 1, .reversed = 0 } },
            { 0 }),
        s,
        p,
        z,
        diags));
    scav_rect const &dot{ z.state[start.v] };
    scav_rect const &at{ z.state[t.v] };
    CHECK(dot.y + (dot.h / 2) == at.y + (at.h / 2));
    CHECK(at.x - (dot.x + dot.w) == p.rank_sep);
    int32_t const column{ imin(at.x, z.state[w.v].x) };
    CHECK(column == ((wide != 0) ? box_clearance(p) : (dot.w + p.rank_sep)));
  }
}

TEST_CASE("size: a port on a cross border sits on the frame's edge over its neighbour") {
  // `A -> B` across two ranks, and a port in `B`'s rank on the top or bottom border,
  // joined to `B` by a flat edge.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  scav_profile const p{ unfolded() };
  for (uint8_t const cross : { uint8_t{ 1 }, uint8_t{ 2 } }) {
    CAPTURE(static_cast<uint32_t>(cross));
    uint32_t const at{ (cross == 1) ? 1U : 2U };
    PodVector<OrderNode> nodes{ state_node(a.v, 0, 0), state_node(b.v, 1, 0) };
    nodes.insert(nodes.begin() + at,
                 { .kind = OrderKind::Boundary, .subject = 1, .rank = 1, .pos = 0 });
    nodes[2].pos = 1;
    uint32_t const mate{ (cross == 1) ? 2U : 1U };
    SubmachineOrders o{ one_frame(
        c,
        root,
        nodes,
        { { .src = 0, .dst = mate, .segment = 0, .reversed = 0 },
          { .src = at, .dst = mate, .segment = 1, .reversed = 0 } },
        { 0 }) };
    o.seg_cross = { 0, cross };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, depths({ 0, 0 }), o, {}, p, z, diags));
    scav_rect const box{ z.state[b.v] };
    CHECK(z.node[at].x == (box.x + (box.w / 2)));
    CHECK(z.node[at].y == ((cross == 1) ? 0 : z.sub[root.v].h));
    CHECK(box.y == z.state[a.v].y);
    CHECK(box.y == 0);
    CHECK(z.sub[root.v].h == box.h);
  }
}

TEST_CASE("size: a folded rank run packs its pieces rather than stacking them") {
  // Nine chained ranks on computed orders; rank 4 is far taller than the rest.
  scav_profile const p{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> chain;
  chain.reserve(9);
  for (uint32_t i = 0; i < 9; ++i) {
    chain.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < chain.size(); ++i) {
    build_trans(c, chain[i - 1], chain[i], TransKind::Default, {});
  }
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

  // A later rank's node above an earlier rank's marks a piece packed beside another.
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

  // Every node lies inside the frame.
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
  // Five states with `SPACE_MAX` bands in rank 1, all joined to one source: one component
  // whose column exceeds the domain.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const source{ build_state(c, root, "S", StateKind::Normal, {}) };
  PodVector<OrderNode> nodes{ state_node(source.v, 0, 0) };
  PodVector<OrderEdge> edges;
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
                          depths(PodVector<uint32_t>(c.states.size(), 0)),
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
  // A junction's kind minimum is narrower than two pads; every band stays inside its box.
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
  // `J` has no submachine, `K` an empty one: both are the bare mark, bands spanning it.
  CHECK(z.state[lone.v].w == p.kind_min_w[junction]);
  CHECK(z.state[empty.v].w == p.kind_min_w[junction]);
  CHECK(z.before[lone.v].w == z.state[lone.v].w);
  CHECK(z.before[empty.v].w == z.state[empty.v].w);
  banded(lone);
  banded(empty);

  // `L` rings its child submachine; `N`, a Normal state, takes the ring while empty.
  CHECK(z.state[holding.v].w == z.sub[inner.v].w + (2 * p.pad));
  CHECK(z.before[holding.v].w == z.state[holding.v].w - (2 * p.pad));
  CHECK(z.state[ordinary.v].w == p.kind_min_w[0] + (2 * p.pad));
  CHECK(z.before[ordinary.v].w == z.state[ordinary.v].w - (2 * p.pad));
  banded(holding);
  banded(ordinary);
}

TEST_CASE("size: a boundary node sits on the frame's border, not on its piece's") {
  // Nine chained ranks, rank 4 far taller than the rest; a sink boundary shares rank 7
  // with `chain[7]`.
  scav_profile const pf{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> chain;
  chain.reserve(9);
  for (uint32_t i = 0; i < 9; ++i) {
    chain.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  PodVector<OrderNode> nodes;
  PodVector<OrderEdge> edges;
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
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, nodes, edges, { 0, 0, 0, 0, 0, 0, 0, 0 }),
                      s,
                      pf,
                      z,
                      diags));

  // `chain[7]` ends inside the frame; the boundary sits on the frame's trailing edge.
  scav_rect const mate{ z.state[chain[7].v] };
  REQUIRE((mate.x + mate.w) < z.sub[root.v].w);
  CHECK(z.node[boundary].x == z.sub[root.v].w);
  // Its y, from the cross-axis assignment, lies inside the frame.
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

  // `K`, with a 24-unit after band, takes the padding ring around its kind minimum.
  uint32_t const junction{ static_cast<uint32_t>(StateKind::Junction) };
  CHECK(z.state[bare.v].w == p.kind_min_w[junction]);
  CHECK(z.state[trailing.v].w == p.kind_min_w[junction] + (2 * p.pad));
  CHECK(z.state[trailing.v].h == p.kind_min_h[junction] + (2 * p.pad));
  CHECK(z.after[trailing.v].w == z.state[trailing.v].w - (2 * p.pad));
  CHECK(z.after[trailing.v].h == 24);
}

TEST_CASE(
    "size: a band keeps the sub_sep from the contents any other interior piece does") {
  // `C` holds `A` under a header and beside a leading side band; `L` has a header and no
  // contents, so nothing sits beside its band.
  scav_profile const p{ profile() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const leaf{ build_state(c, root, "L", StateKind::Normal, {}) };
  std::vector<scav_box_space> boxes(c.states.size(), scav_box_space{});
  boxes[comp.v] = { .min_w = 0, .h_before = 300, .h_after = 0, .w_before = 200 };
  boxes[leaf.v] = { .min_w = 0, .h_before = 300, .h_after = 0 };
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };

  SplitGraph const g{ decompose(c) };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, g, order_submachines(c, g, s, p), s, p, z, diags));

  scav_rect const &head{ z.before[comp.v] };
  scav_rect const &side{ z.lead[comp.v] };
  scav_rect const &kid{ z.state[a.v] };
  CHECK(kid.y == (head.y + head.h + p.sub_sep));
  CHECK(kid.x == (side.x + side.w + p.sub_sep));
  // The side band runs from the header down, with no seam between them.
  CHECK(side.y == (head.y + head.h));
  uint32_t const normal{ static_cast<uint32_t>(StateKind::Normal) };
  CHECK(z.state[leaf.v].h == imax(300, p.kind_min_h[normal]) + (2 * p.pad));
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

  // Tombstoned, the frame keeps a zero rect and the mark is bare.
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
  // Junctions 0 wide: the inner frame is 0 wide, and its height alone gives `K` the ring.
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
  // `J` stays bare; `K`, holding the zero-width frame, takes the ring.
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
  REQUIRE(
      size_layout(c, depths(PodVector<uint32_t>(c.states.size(), 0)), o, {}, p, z, diags));

  CHECK(diags.empty());
  CHECK(z.sub[root.v].w == 0);
  CHECK(z.sub[root.v].h == 0);
  // The box formula, which reads no orders, sizes the state itself.
  CHECK(z.state[a.v].w == p.kind_min_w[0] + (2 * p.pad));
}

TEST_CASE("size: a fold whose pieces will not pack is dropped for the flat run") {
  // Ranks 0 and 2 hold two `SPACE_MAX`-tall states each, ranks 1 and 3 one
  // `SPACE_MAX`-wide state; `trybox` is 0, and the fold's stacked pieces leave the domain.
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

  PodVector<OrderNode> const nodes{
    state_node(ids[0].v, 0, 0), state_node(ids[1].v, 0, 1), state_node(ids[2].v, 1, 0),
    state_node(ids[3].v, 2, 0), state_node(ids[4].v, 2, 1), state_node(ids[5].v, 3, 0)
  };
  PodVector<OrderEdge> const edges{ { .src = 0, .dst = 2, .segment = 0, .reversed = 0 },
                                    { .src = 1, .dst = 2, .segment = 1, .reversed = 0 },
                                    { .src = 2, .dst = 3, .segment = 2, .reversed = 0 },
                                    { .src = 2, .dst = 4, .segment = 3, .reversed = 0 },
                                    { .src = 3, .dst = 5, .segment = 4, .reversed = 0 },
                                    { .src = 4, .dst = 5, .segment = 5, .reversed = 0 } };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, nodes, edges, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());

  // The flat run: four ranks end to end, each `rank_sep` past the last plus 0 or 2 lanes
  // where two tall states turn into one short one.
  int32_t const tall_w{ p.kind_min_w[0] + (2 * p.pad) };
  int32_t const wide_w{ SPACE_MAX + (2 * p.pad) };
  int32_t const two{ 2 * label_line_height(p) };
  CHECK(z.state[ids[0].v].x == 0);
  int32_t const at2{ z.state[ids[2].v].x - (tall_w + p.rank_sep) };
  int32_t const at3{ z.state[ids[3].v].x - (z.state[ids[2].v].x + wide_w + p.rank_sep) };
  int32_t const at5{ z.state[ids[5].v].x - (z.state[ids[3].v].x + tall_w + p.rank_sep) };
  for (int32_t const lanes : { at2, at3, at5 }) {
    CAPTURE(lanes);
    CHECK(((lanes == 0) || (lanes == two)));
  }
  CHECK(z.sub[root.v].w == (z.state[ids[5].v].x + wide_w));
  CHECK(z.sub[root.v].w <= COORD_MAX);
  CHECK(z.sub[root.v].h <= COORD_MAX);
  // The height is two tall states and one `node_sep`: one row.
  CHECK(z.sub[root.v].h == ((2 * (SPACE_MAX + (2 * p.pad))) + p.node_sep));
}

TEST_CASE("size: a row that leaves the domain does not displace the column that fits") {
  // Two unconnected states, each about half the domain wide and tall.
  scav_profile p{ profile() };
  p.pad = 130800;
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  PodVector<OrderNode> const nodes{ state_node(a.v, 0, 0), state_node(b.v, 0, 1) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, nodes, {}, {}),
                      {},
                      p,
                      z,
                      diags));
  CHECK(diags.empty());
  CHECK(z.sub[root.v].w == (p.kind_min_w[0] + (2 * p.pad)));
  CHECK(z.sub[root.v].h == ((2 * (p.kind_min_h[0] + (2 * p.pad))) + p.node_sep));
  // A row of the two exceeds the domain.
  CHECK(((2 * (p.kind_min_w[0] + (2 * p.pad))) + p.node_sep) > COORD_MAX);

  // The same extents with no box packer give the same column.
  p.trybox = 0;
  SizedLayout column;
  diags.clear();
  REQUIRE(size_layout(c,
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, nodes, {}, {}),
                      {},
                      p,
                      column,
                      diags));
  CHECK(column.sub[root.v].w == z.sub[root.v].w);
  CHECK(column.sub[root.v].h == z.sub[root.v].h);
}

namespace {

// `n` unconnected states of one shape in rank 0: `n` equal components for the packers.
struct EqualStates {
  PodVector<OrderNode> nodes;
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

// Zero `pad` and `node_sep`: a rect is its request. At 1:1024 `pack_lr` stacks them.
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
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, five.nodes, {}, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());

  // The row: five states side by side at one y.
  CHECK(z.sub[root.v].w == (5 * p.kind_min_w[0]));
  CHECK(z.sub[root.v].h == SPACE_MAX);
  for (uint32_t i = 0; i < five.nodes.size(); ++i) {
    CHECK(z.state[five.nodes[i].subject].x == (static_cast<int32_t>(i) * p.kind_min_w[0]));
    CHECK(z.state[five.nodes[i].subject].y == z.state[five.nodes[0].subject].y);
  }

  // With no box packer, `pack_lr`'s column leaves the domain and the frame is diagnosed.
  p.trybox = 0;
  SizedLayout stacked;
  diags.clear();
  CHECK_FALSE(size_layout(c,
                          depths(PodVector<uint32_t>(c.states.size(), 0)),
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
  // Five `SPACE_MAX` squares: the row exceeds the domain in x and the column in y.
  scav_profile const p{ stacking() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  EqualStates const five{ equal_states(c, root, 5, SPACE_MAX) };
  scav_spaces const s{ .box_state = five.boxes.data(),
                       .n_box_state = static_cast<uint32_t>(five.boxes.size()) };

  SizedLayout z;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(size_layout(c,
                          depths(PodVector<uint32_t>(c.states.size(), 0)),
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

// `n` chained states, one per rank: one component whose run the fold cuts into pieces.
struct RankRun {
  PodVector<OrderNode> nodes;
  PodVector<OrderEdge> edges;
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
  // The flat run, five `SPACE_MAX` ranks end to end, exceeds the domain; so does a row of
  // the fold's pieces, and the frame keeps the stack.
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
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());

  // Three ranks in the first piece and two in the second, one above the other.
  CHECK(z.sub[root.v].w == (3 * SPACE_MAX));
  CHECK(z.sub[root.v].h == (2 * p.kind_min_h[0]));

  // The same shape with no box packer gives the same stack.
  p.trybox = 0;
  SizedLayout column;
  diags.clear();
  REQUIRE(size_layout(c,
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      column,
                      diags));
  CHECK(column.sub[root.v].w == z.sub[root.v].w);
  CHECK(column.sub[root.v].h == z.sub[root.v].h);
}

TEST_CASE("size: a fold whose pieces pack back into a row is the flat run") {
  // Five ranks of one `SPACE_MAX`-tall state: the fold's stack exceeds the domain and its
  // row repacks the pieces into one line, so the flat run is kept.
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
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());
  CHECK(z.sub[root.v].w == ((5 * p.kind_min_w[0]) + (4 * p.rank_sep)));
  CHECK(z.sub[root.v].h == SPACE_MAX);
  CHECK((5 * SPACE_MAX) > COORD_MAX);

  // With no box packer the fold's stack leaves the domain, so the flat run is kept.
  p.trybox = 0;
  SizedLayout flat;
  diags.clear();
  REQUIRE(size_layout(c,
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
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
  // `node_sep` at `SPACE_MAX`: the pieces exceed the domain in a row or a stack, four
  // `node_sep` gaps either way. The flat run spaces ranks by `rank_sep`, 0.
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
                      depths(PodVector<uint32_t>(c.states.size(), 0)),
                      one_frame(c, root, run.nodes, run.edges, {}),
                      s,
                      p,
                      z,
                      diags));
  CHECK(diags.empty());
  // The flat run: five ranks end to end with no gap between.
  CHECK(z.sub[root.v].w == (5 * p.kind_min_w[0]));
  CHECK(z.sub[root.v].h == SPACE_MAX);
  CHECK(((5 * p.kind_min_w[0]) + (4 * p.node_sep)) > COORD_MAX);
}

namespace {

// `subs` sibling submachines under one state, each a chain of `ranks` states of one
// shape: the state packs `subs` equal rects.
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
      if (prev.v != INVALID) { build_trans(c, prev, at, TransKind::Default, {}); }
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
  // Two regions, each two `SPACE_MAX` ranks wide and one state tall: side by side they
  // exceed the domain, stacked they fit.
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

  // The same extents with no box packer give the same column.
  p.trybox = 0;
  SizedLayout column;
  diags.clear();
  REQUIRE(size_layout(c, g, order_submachines(c, g, {}, p), s, p, column, diags));
  CHECK(column.state[two.owner.v].w == z.state[two.owner.v].w);
  CHECK(column.state[two.owner.v].h == z.state[two.owner.v].h);
}

TEST_CASE("size: a sibling column that leaves the domain gives way to the row") {
  // Five regions of one `SPACE_MAX`-tall state each: stacked they exceed the domain, side
  // by side they are five narrow boxes.
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

  // With no box packer the stack leaves the domain and the owner state is diagnosed.
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
  // Five `SPACE_MAX` squares: the row exceeds the domain in x and the column in y.
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
  // `pad` at `SPACE_MAX`: the inner frame fits the domain; its owner's ring exceeds it.
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
  CHECK(z.sub[inner.v].w <= COORD_MAX);
  CHECK((z.sub[inner.v].w + (2 * p.pad)) > COORD_MAX);
}

namespace scav {

// Test-only declarations of `size.cpp`'s SCAV_INTERNAL owner-hole functions.
FrameDar size_hole_ratio(int32_t w, int32_t h);
void size_owner_holes(Chart const &c,
                      SizedLayout const &z,
                      int32_t sep,
                      PodVector<FrameDar> &hole);

}  // namespace scav

namespace {

PodVector<FrameDar> size_owner_holes(Chart const &c, SizedLayout const &z) {
  PodVector<FrameDar> hole;
  scav::size_owner_holes(c, z, 0, hole);
  return hole;
}

// Choice states at least 100000 units tall: an owner's hole is far taller than its frame.
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

// Counts the distinct x and y origins of the frame's children: its columns and rows.
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
  // Both fields stay in [1, 1024]; the longer axis takes 1024.
  CHECK(size_hole_ratio(1000, 1000).num == 1024);
  CHECK(size_hole_ratio(1000, 1000).den == 1024);
  CHECK(size_hole_ratio(2000, 1000).num == 1024);
  CHECK(size_hole_ratio(2000, 1000).den == 512);
  CHECK(size_hole_ratio(1000, 2000).num == 512);
  CHECK(size_hole_ratio(1000, 2000).den == 1024);
  // A 16:10 hole gives 1024:640, the readable profile's ratio.
  CHECK(size_hole_ratio(1600, 1000).num == 1024);
  CHECK(size_hole_ratio(1600, 1000).den == 640);

  // A hole a million times longer than wide floors its short field at 1.
  CHECK(size_hole_ratio(1000000, 1).num == 1024);
  CHECK(size_hole_ratio(1000000, 1).den == 1);
  CHECK(size_hole_ratio(1, 1000000).num == 1);
  CHECK(size_hole_ratio(1, 1000000).den == 1024);

  // A zero or negative extent gives `num` 0, and the profile's ratio applies.
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

  PodVector<FrameDar> const hole{ size_owner_holes(c, z) };
  REQUIRE(hole.size() == c.states.size());
  // The 100000-tall owner around a short frame leaves a hole far taller than wide.
  CHECK(hole[owner.v].num < hole[owner.v].den);
  CHECK(hole[owner.v].den == 1024);
  // A leaf holds no frame: `num` 0.
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (i != owner.v) { CHECK(hole[i].num == 0); }
  }

  // A tombstoned frame leaves no hole: `num` 0.
  c.submachines[frame.v].live = 0;
  CHECK(size_owner_holes(c, z)[owner.v].num == 0);
}

TEST_CASE("size: a frame handed its owner's hole packs to that shape") {
  // Four components pack two by two at the profile's 16:10 and in one column in the tall,
  // narrow hole.
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
  // The root frame has no owner, so `OwnerHole` and `Profile` give identical rects.
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
  // An overflow in the first pass ends sizing with one diagnostic, on the owner.
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

TEST_CASE("size: whitespace elimination grows a sibling submachine's own rect") {
  // Frames of 896 x 896, 896 x 640 and 896 x 640 at `sub_sep`: the first two side by side,
  // the third wrapped under them.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const owner{ build_state(c, root, "P", StateKind::Normal, {}) };
  std::vector<SubmachineId> subs;
  std::vector<StateId> kids;
  for (uint32_t i = 0; i < 3; ++i) {
    subs.push_back(build_submachine(c, owner, {}, {}));
    kids.push_back(build_state(c, subs.back(), "K", StateKind::Normal, {}));
  }
  scav_profile const p{ profile() };

  std::vector<scav_box_space> spaces(c.states.size(), scav_box_space{});
  spaces[kids[0].v] = { .min_w = 0, .h_before = 640, .h_after = 0 };
  scav_spaces const sp{ .box_state = spaces.data(),
                        .n_box_state = static_cast<uint32_t>(spaces.size()) };

  SubmachineOrders o;
  o.sub_nodes.assign(c.submachines.size(), Span{});
  o.sub_edges.assign(c.submachines.size(), Span{});
  o.sub_ranks.assign(c.submachines.size(), 0);
  o.sub_gaps.assign(c.submachines.size(), Span{});
  o.state_node.assign(c.states.size(), INVALID);
  for (uint32_t i = 0; i < 3; ++i) {
    o.nodes.push_back(state_node(kids[i].v, 0, 0));
    o.state_node[kids[i].v] = i;
    o.sub_nodes[subs[i].v] = make_span(i, 1);
    o.sub_ranks[subs[i].v] = 1;
  }
  o.nodes.push_back(state_node(owner.v, 0, 0));
  o.state_node[owner.v] = 3;
  o.sub_nodes[root.v] = make_span(3, 1);
  o.sub_ranks[root.v] = 1;

  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c, depths({ 0, 1, 1, 1 }), o, sp, p, z, diags));
  CHECK(z.sub[subs[0].v].w == 896);
  CHECK(z.sub[subs[0].v].h == 896);
  CHECK(z.sub[subs[1].v].w == 896);
  CHECK(z.sub[subs[1].v].h == 896);   // 640 as sized, then its subrow's height
  CHECK(z.sub[subs[2].v].w == 1984);  // 896 as sized, then its block's width
  CHECK(z.sub[subs[2].v].h == 640);
  // Every gap stays `sub_sep`.
  CHECK(z.sub[subs[1].v].x == (z.sub[subs[0].v].x + 896 + p.sub_sep));
  CHECK(z.sub[subs[1].v].y == z.sub[subs[0].v].y);
  CHECK(z.sub[subs[2].v].x == z.sub[subs[0].v].x);
  CHECK(z.sub[subs[2].v].y == (z.sub[subs[0].v].y + 896 + p.sub_sep));
  // The owner's box is the packing's extents, 1984 x 1728, plus its ring.
  CHECK(z.state[owner.v].w == (1984 + (2 * p.pad)));
  CHECK(z.state[owner.v].h == (1728 + (2 * p.pad)));
}

namespace {

// A corpus chart with a band on every state, a label box on every live transition, and
// its orders.
struct Sample {
  Chart c;
  std::vector<scav_box_space> box;
  std::vector<scav_path_box> path;
  SplitGraph g;
  SubmachineOrders o;
  [[nodiscard]] scav_spaces spaces() const {
    return { .box_state = box.data(),
             .n_box_state = static_cast<uint32_t>(box.size()),
             .box_state_stride = sizeof(scav_box_space),
             .path_box = path.data(),
             .n_path_box = static_cast<uint32_t>(path.size()),
             .path_box_stride = sizeof(scav_path_box) };
  }
};

Sample sample(char const *name, scav_profile const &p) {
  Sample x;
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, x.c, diags, failed));
  for (uint32_t i = 0; i < x.c.states.size(); ++i) {
    x.box.push_back({ .min_w = 40 + static_cast<int32_t>((i % 5U) * 10U),
                      .h_before = static_cast<int32_t>((i % 3U) * 8U),
                      .h_after = static_cast<int32_t>((i % 2U) * 6U) });
  }
  for (uint32_t t = 0; t < x.c.transitions.size(); ++t) {
    if (x.c.transitions[t].live == 0) { continue; }
    x.path.push_back({ .subject = t,
                       .w = 30 + static_cast<int32_t>((t % 4U) * 12U),
                       .h = 12 + static_cast<int32_t>((t % 3U) * 4U),
                       .order = 0 });
  }
  x.g = decompose(x.c);
  x.o = order_submachines(x.c, x.g, x.spaces(), p, 1, {});
  return x;
}

uint32_t largest_frame(Sample const &x) {
  uint32_t most{ 0 };
  for (Span const &s : x.o.sub_nodes) { most = (s.len > most) ? s.len : most; }
  return most;
}

// A sizing and its trace; a traced run lays out every frame, bypassing the memo.
struct Traced {
  SizedLayout z;
  std::vector<char> trace;
  bool ok{ false };
};

Traced traced(Sample const &x, scav_profile const &p, Fold fold) {
  Traced out;
  TraceRecord t{ x.c };
  trace_sink_set(&t);
  std::vector<Diagnostic> diags;
  out.ok = size_layout(x.c,
                       x.g,
                       x.o,
                       x.spaces(),
                       p,
                       out.z,
                       diags,
                       DarSource::OwnerHole,
                       Compaction::On,
                       fold);
  trace_sink_set(nullptr);
  out.trace = t.json();
  return out;
}

bool same(Traced const &a, Traced const &b) {
  auto const rects = [](PodVector<scav_rect> const &u, PodVector<scav_rect> const &v) {
    if (u.size() != v.size()) { return false; }
    for (uint32_t i = 0; i < u.size(); ++i) {
      if (!(u[i] == v[i])) { return false; }
    }
    return true;
  };
  if (a.z.node.size() != b.z.node.size()) { return false; }
  for (uint32_t i = 0; i < a.z.node.size(); ++i) {
    if ((a.z.node[i].x != b.z.node[i].x) || (a.z.node[i].y != b.z.node[i].y)) {
      return false;
    }
  }
  return (a.ok == b.ok) && rects(a.z.state, b.z.state) && rects(a.z.before, b.z.before) &&
         rects(a.z.after, b.z.after) && rects(a.z.sub, b.z.sub) &&
         (a.z.lean == b.z.lean) && (a.z.folded == b.z.folded) &&
         (a.z.chart == b.z.chart) && (a.trace == b.trace);
}

}  // namespace

// Each fresh result is the first sizing on a new thread.
TEST_CASE("size: a sizing is the same whatever its thread sized before") {
  scav_profile const p{ profile() };
  Sample const small{ sample("led.scav", p) };
  Sample const large{ sample("bottler.scav", p) };
  REQUIRE(largest_frame(large) > largest_frame(small));
  for (Fold const fold : { Fold::Scale, Fold::Always }) {
    CAPTURE(static_cast<uint32_t>(fold));
    auto const fresh = [&](Sample const &x) {
      Traced out;
      std::thread([&] { out = traced(x, p, fold); }).join();
      return out;
    };
    Traced const small_fresh{ fresh(small) };
    Traced const large_fresh{ fresh(large) };
    REQUIRE(small_fresh.ok);
    REQUIRE(large_fresh.ok);
    Traced small_first;
    Traced large_after;
    Traced small_after;
    std::thread([&] {
      small_first = traced(small, p, fold);
      large_after = traced(large, p, fold);
      small_after = traced(small, p, fold);
    }).join();
    CHECK(same(small_first, small_fresh));
    CHECK(same(large_after, large_fresh));
    CHECK(same(small_after, small_fresh));
  }
}

namespace {

// What sizing `c` reads of a row's knobs, ordered with no pins.
RowReads reads_of(Chart const &c) {
  SplitGraph const g{ decompose(c) };
  return size_row_reads(c, order_submachines(c, g, {}, profile()));
}

}  // namespace

TEST_CASE("size: a lone state reads no row knob, and an edge reads all but the hole") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  RowReads const lone{ reads_of(c) };
  CHECK(!lone.trybox);
  CHECK(!lone.pack);
  CHECK(!lone.dar);
  CHECK(!lone.fold);
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  RowReads const edged{ reads_of(c) };
  CHECK(edged.trybox);
  CHECK(edged.pack);
  CHECK(!edged.dar);  // no composite owns a frame
  CHECK(edged.fold);
}

TEST_CASE("size: two unjoined states pack, and so read the packer and compaction") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  build_state(c, root, "B", StateKind::Normal, {});
  RowReads const r{ reads_of(c) };
  CHECK(r.trybox);
  CHECK(r.pack);
  CHECK(!r.dar);
  CHECK(!r.fold);
}

TEST_CASE("size: a composite reads its hole only where its packings can hold two rects") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const owner{ build_state(c, root, "O", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, owner, "r", {}) };
  StateId const x{ build_state(c, inner, "X", StateKind::Normal, {}) };
  CHECK(!reads_of(c).dar);  // one region of one state
  StateId const y{ build_state(c, inner, "Y", StateKind::Normal, {}) };
  build_trans(c, x, y, TransKind::Default, {});
  CHECK(reads_of(c).dar);  // its region can fold
  Chart two;
  SubmachineId const top{ build_chart(two, "t", {}) };
  StateId const both{ build_state(two, top, "O", StateKind::Normal, {}) };
  build_state(two, build_submachine(two, both, "r1", {}), "X", StateKind::Normal, {});
  build_state(two, build_submachine(two, both, "r2", {}), "Y", StateKind::Normal, {});
  RowReads const regions{ reads_of(two) };
  CHECK(regions.dar);  // two regions pack in its hole
  CHECK(regions.pack);
  CHECK(!regions.fold);
}

TEST_CASE(
    "size: a knob no sizing reads takes row 0's value, one that is read keeps its own") {
  scav_profile const p{ profile() };
  Row flipped{ .knobs = p,
               .dar = DarSource::OwnerHole,
               .pack = Compaction::On,
               .fold = Fold::Always };
  flipped.knobs.trybox = 1 - p.trybox;
  Row const none{ size_row_canonical(flipped, {}, p) };
  CHECK(none.knobs.trybox == p.trybox);
  CHECK(none.dar == DarSource::Profile);
  CHECK(none.pack == Compaction::Off);
  CHECK(none.fold == Fold::Scale);
  RowReads const all{ .trybox = true, .pack = true, .dar = true, .fold = true };
  Row const kept{ size_row_canonical(flipped, all, p) };
  CHECK(kept.knobs.trybox == flipped.knobs.trybox);
  CHECK(kept.dar == DarSource::OwnerHole);
  CHECK(kept.pack == Compaction::On);
  CHECK(kept.fold == Fold::Always);
}

namespace {

// `one_frame` over a lone state `a`, with a grow pin's `seats` on it.
SubmachineOrders grown_one(Chart const &c, SubmachineId root, StateId a, FaceSeats seats) {
  SubmachineOrders o{ one_frame(c, root, { state_node(a.v, 0, 0) }, {}, {}) };
  o.state_grow.assign(c.states.size(), FaceSeats{});
  o.state_grow[a.v] = seats;
  return o;
}

}  // namespace

TEST_CASE("size: a grow pin floors each face to hold its seats a line of text apart") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  scav_profile const p{ profile() };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths({ 0 }),
                      grown_one(c, root, a, { .w = 7, .h = 5 }),
                      {},
                      p,
                      z,
                      diags));
  // The corner at its largest is `pad`, past the route clearance at `readable`.
  int32_t const inset{ imax(route_clearance(p), p.pad) };
  int32_t const pitch{ imax(route_clearance(p), label_line_height(p)) };
  CHECK(z.state[a.v].w == ((6 * pitch) + (2 * inset)));
  CHECK(z.state[a.v].h == ((4 * pitch) + (2 * inset)));
  int32_t const arc{ state_corner_radius(StateKind::Normal, z.state[a.v], p.pad) };
  CHECK(face_capacity(z.state[a.v].w, arc, p) == 7);
  CHECK(face_capacity(z.state[a.v].h, arc, p) == 5);
}

TEST_CASE("size: a grow pin its natural box already holds leaves the box as it is") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  scav_profile const p{ profile() };
  SizedLayout z;
  std::vector<Diagnostic> diags;
  REQUIRE(size_layout(c,
                      depths({ 0 }),
                      grown_one(c, root, a, { .w = 2, .h = 0 }),
                      {},
                      p,
                      z,
                      diags));
  CHECK(z.state[a.v].w == p.kind_min_w[0] + (2 * p.pad));
  CHECK(z.state[a.v].h == p.kind_min_h[0] + (2 * p.pad));
}

TEST_CASE("size: the last grow pin naming a state decides its box") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  scav_profile const p{ profile() };
  SplitGraph const g{ decompose(c) };
  auto const sized = [&](SearchPins const &pins) {
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, order_submachines(c, g, {}, p, 1, pins), {}, p, z, diags));
    return z.state[a.v];
  };
  scav_rect const natural{ sized({}) };
  SearchPins const wide{ .grows = { { .state = a, .w = 2, .h = 1 },
                                    { .state = a, .w = 9, .h = 1 } } };
  SearchPins const back{ .grows = { { .state = a, .w = 9, .h = 1 },
                                    { .state = a, .w = 2, .h = 1 } } };
  CHECK(sized(wide).w == face_length(9, p.pad, p));
  CHECK(sized(wide).h == natural.h);
  CHECK(sized(back).w == natural.w);
}

TEST_CASE("size: a box a grow pin raises is traced with its seats and both extents") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  scav_profile const p{ profile() };
  auto const grown = [&](FaceSeats seats) {
    TraceRecord t{ c };
    trace_sink_set(&t);
    SizedLayout z;
    std::vector<Diagnostic> diags;
    bool const ok{
      size_layout(c, depths({ 0 }), grown_one(c, root, a, seats), {}, p, z, diags)
    };
    trace_sink_set(nullptr);
    REQUIRE(ok);
    std::vector<TraceEvent> out;
    for (TraceEvent const &e : t.events()) {
      if (e.kind == TraceKind::StateGrown) { out.push_back(e); }
    }
    return out;
  };
  CHECK(grown({ .w = 2, .h = 1 }).empty());
  std::vector<TraceEvent> const up{ grown({ .w = 7, .h = 1 }) };
  REQUIRE(up.size() == 1);
  CHECK(up[0].pass == 1);  // pinned
  CHECK(up[0].grow.state == a.v);
  CHECK(up[0].grow.seats_w == 7);
  CHECK(up[0].grow.seats_h == 1);
  CHECK(up[0].grow.from_w == p.kind_min_w[0] + (2 * p.pad));
  CHECK(up[0].grow.from_h == p.kind_min_h[0] + (2 * p.pad));
  CHECK(up[0].grow.to_w == face_length(7, p.pad, p));
  CHECK(up[0].grow.to_h == up[0].grow.from_h);
}
