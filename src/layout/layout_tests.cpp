// Exact rects from the box formula, stacking, routes, placed boxes, the
// columns, the hash split, and a geometry invariant sweep.

#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_core_c.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_c_handles.h"

#include "doctest.h"

#include "scav_int.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace scav;

// The C structs carry no operators; the tests compare them field-wise.
constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

// The default router and no thread request, which is every test that does not
// say otherwise.
scav_layout_opts opts(scav_profile const &p) {
  return { .profile = p, .router = 0, .threads = 0 };
}

// A run expected to succeed, returning its placed boxes.
std::vector<scav_placed> run(Chart &c, scav_spaces const &s, scav_profile const &p) {
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  bool const ok{ layout_run(c, s, opts(p), placed, diags) };
  std::string why;
  for (Diagnostic const &d : diags) {
    why += diag_message(d.code);
    why += "; ";
  }
  CAPTURE(why);
  REQUIRE(ok);
  return placed;
}

// Typed column reads over the public accessors -- what any consumer writes.
template <typename T>
T row_of(Chart const &c, char const *name, uint32_t row) {
  ColumnId const id{ column_find(c, name) };
  REQUIRE(id.v != INVALID);
  REQUIRE(row < column_count(c, id));
  T out{};
  std::memcpy(&out,
              column_data(c, id) + (static_cast<size_t>(row) * sizeof(T)),
              sizeof(T));
  return out;
}

scav_rect state_rect(Chart const &c, StateId s) {
  return row_of<scav_rect>(c, "scav.geom.state", s.v);
}
scav_rect sub_rect(Chart const &c, SubmachineId m) {
  return row_of<scav_rect>(c, "scav.geom.sub", m.v);
}

bool inside(scav_rect const &inner, scav_rect const &outer) {
  return (inner.x >= outer.x) && (inner.y >= outer.y) &&
         ((inner.x + inner.w) <= (outer.x + outer.w)) &&
         ((inner.y + inner.h) <= (outer.y + outer.h));
}

bool overlap(scav_rect const &a, scav_rect const &b) {
  return (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) && (a.y < (b.y + b.h)) &&
         (b.y < (a.y + a.h));
}

bool on_border(scav_point pt, scav_rect const &r) {
  bool const x_edge{ (pt.x == r.x) || (pt.x == (r.x + r.w)) };
  bool const y_edge{ (pt.y == r.y) || (pt.y == (r.y + r.h)) };
  bool const x_in{ (pt.x >= r.x) && (pt.x <= (r.x + r.w)) };
  bool const y_in{ (pt.y >= r.y) && (pt.y <= (r.y + r.h)) };
  return (x_edge && y_in) || (y_edge && x_in);
}

}  // namespace

TEST_CASE("layout: an empty chart lays out to nothing, hashed and stable") {
  Chart c;
  std::vector<scav_placed> const placed{ run(c, {}, readable()) };
  CHECK(placed.empty());
  CHECK((row_of<scav_rect>(c, "scav.geom.chart", 0) == scav_rect{}));
  CHECK(row_of<uint32_t>(c, "scav.geom.gen", 0) == 1);
  CHECK(layout_structural_hash(c) == layout_structural_hash(c));
  CHECK(layout_coordinate_hash(c) == layout_coordinate_hash(c));
}

TEST_CASE("layout: a lone leaf sizes to its kind minimum plus padding") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  scav_profile const p{ readable() };
  run(c, {}, p);

  int32_t const w{ p.kind_min_w[0] + (2 * p.pad) };
  int32_t const h{ p.kind_min_h[0] + (2 * p.pad) };
  CHECK((state_rect(c, a) == scav_rect{ .x = 0, .y = 0, .w = w, .h = h }));
  CHECK((row_of<scav_rect>(c, "scav.geom.chart", 0) ==
         scav_rect{ .x = 0, .y = 0, .w = w, .h = h }));
  // Zero-height bands still land at their offsets inside the padding.
  CHECK((row_of<scav_rect>(c, "scav.geom.state_before", a.v) ==
         scav_rect{ .x = p.pad, .y = p.pad, .w = p.kind_min_w[0], .h = 0 }));
}

TEST_CASE("layout: each arm of the box formula can dominate") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const by_min_w{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const by_kind{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const by_subs{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, by_subs, {}, {}) };
  build_state(c, inner, "K", StateKind::Normal, {});

  scav_profile const p{ readable() };
  std::vector<scav_box_space> boxes(c.states.size());
  boxes[by_min_w.v] = { .min_w = 9000, .h_before = 100, .h_after = 50 };
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };
  run(c, s, p);

  // min_w dominates A's width; its height stacks the bands over kind_min_h.
  int32_t const bands{ 100 + 50 };
  int32_t const a_h{ ((bands > p.kind_min_h[0]) ? bands : p.kind_min_h[0]) + 2 * p.pad };
  CHECK(state_rect(c, by_min_w).w == 9000 + (2 * p.pad));
  CHECK(state_rect(c, by_min_w).h == a_h);

  // B has nothing: kind minimum alone.
  CHECK(state_rect(c, by_kind).w == p.kind_min_w[0] + (2 * p.pad));

  // C wraps its child submachine: child leaf width plus two pads.
  int32_t const leaf_w{ p.kind_min_w[0] + (2 * p.pad) };
  CHECK(sub_rect(c, inner).w == leaf_w);
  CHECK(state_rect(c, by_subs).w == leaf_w + (2 * p.pad));
}

TEST_CASE("layout: interior bands and submachines stack from the top") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const m1{ build_submachine(c, comp, {}, {}) };
  SubmachineId const m2{ build_submachine(c, comp, {}, {}) };
  build_state(c, m1, "A", StateKind::Normal, {});
  build_state(c, m2, "B", StateKind::Normal, {});

  scav_profile const p{ readable() };
  std::vector<scav_box_space> boxes(c.states.size());
  boxes[comp.v] = { .min_w = 0, .h_before = 40, .h_after = 24 };
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };
  run(c, s, p);

  scav_rect const outer{ state_rect(c, comp) };
  scav_rect const before{ row_of<scav_rect>(c, "scav.geom.state_before", comp.v) };
  scav_rect const r1{ sub_rect(c, m1) };
  scav_rect const r2{ sub_rect(c, m2) };
  scav_rect const after{ row_of<scav_rect>(c, "scav.geom.state_after", comp.v) };

  CHECK(before.y == outer.y + p.pad);
  CHECK(before.h == 40);
  // The two regions are packed, not stacked, so what holds is that they sit
  // in the band between the two reserved ones and share no point.
  CHECK(r1.y == before.y + before.h);
  CHECK(r2.y >= r1.y);
  CHECK_FALSE(overlap(r1, r2));
  CHECK(after.y == imax(r1.y + r1.h, r2.y + r2.h));
  CHECK(after.h == 24);
  CHECK(inside(before, outer));
  CHECK(inside(r1, outer));
  CHECK(inside(r2, outer));
  CHECK(inside(after, outer));
}

TEST_CASE("layout: unconnected siblings are packed, each its own component") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  scav_profile const p{ readable() };
  run(c, {}, p);

  // Nothing joins them, so each is a component of one node and the frame is a
  // packing rather than a rank: two across, the third below the first.
  int32_t const w{ p.kind_min_w[0] + (2 * p.pad) };
  int32_t const h{ p.kind_min_h[0] + (2 * p.pad) };
  CHECK((state_rect(c, a) == scav_rect{ .x = 0, .y = 0, .w = w, .h = h }));
  CHECK((state_rect(c, b) == scav_rect{ .x = w + p.node_sep, .y = 0, .w = w, .h = h }));
  CHECK((state_rect(c, d) == scav_rect{ .x = 0, .y = h + p.node_sep, .w = w, .h = h }));
  CHECK(
      (row_of<scav_rect>(c, "scav.geom.chart", 0) ==
       scav_rect{ .x = 0, .y = 0, .w = (2 * w) + p.node_sep, .h = (2 * h) + p.node_sep }));
}

TEST_CASE("layout: components pack to the aspect-ratio target") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> kids;
  kids.reserve(9);
  for (uint32_t i = 0; i < 9; ++i) {
    kids.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  scav_profile const p{ readable() };
  run(c, {}, p);

  // Nine identical leaves, so nine one-node components. The target width is the
  // same isqrt the packer runs, over the area each rect occupies with its gap.
  int32_t const w{ p.kind_min_w[0] + (2 * p.pad) };
  int32_t const h{ p.kind_min_h[0] + (2 * p.pad) };
  int32_t const sep{ p.node_sep };
  Wide const target{ static_cast<Wide>(
      isqrt(static_cast<uint64_t>(9LL * (w + sep) * (h + sep) * p.dar_num / p.dar_den))) };
  REQUIRE(target >= (3 * w) + (2 * sep));  // three fit
  REQUIRE(target < (4 * w) + (3 * sep));   // four do not

  for (uint32_t i = 0; i < 9; ++i) {
    CHECK(
        (state_rect(c, kids[i]) == scav_rect{ .x = static_cast<int32_t>(i % 3) * (w + sep),
                                              .y = static_cast<int32_t>(i / 3) * (h + sep),
                                              .w = w,
                                              .h = h }));
  }
  CHECK((row_of<scav_rect>(c, "scav.geom.chart", 0) ==
         scav_rect{ .x = 0, .y = 0, .w = (3 * w) + (2 * sep), .h = (3 * h) + (2 * sep) }));
}

TEST_CASE("layout: routes are orthogonal, meet borders, and skip internal loops") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s1{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const d1{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, s1, d1, TransKind::External, {});  // t0: one port on comp
  build_trans(c, d1, d1, TransKind::External, {});  // t1: loop outside
  build_trans(c, d1, d1, TransKind::Internal, {});  // t2: no route
  scav_profile const p{ readable() };
  run(c, {}, p);

  scav_span const r0{ row_of<scav_span>(c, "scav.geom.route", 0) };
  REQUIRE(r0.len >= 2);
  std::vector<scav_point> route;
  route.reserve(r0.len);
  for (uint32_t k = 0; k < r0.len; ++k) {
    route.push_back(row_of<scav_point>(c, "scav.geom.point", r0.off + k));
  }
  // Axis-aligned is the hard constraint the router exists to keep (11.5), and
  // the ends meet their boxes' borders rather than sitting at their centres.
  for (uint32_t k = 0; (k + 1) < r0.len; ++k) {
    CAPTURE(k);
    bool const square{ (route[k].x == route[k + 1].x) || (route[k].y == route[k + 1].y) };
    CHECK(square);
  }
  CHECK(on_border(route.front(), state_rect(c, s1)));
  CHECK(on_border(route.back(), state_rect(c, d1)));

  scav_span const ports{ row_of<scav_span>(c, "scav.geom.port", 0) };
  REQUIRE(ports.len == 1);
  scav_port_slot const slot{ row_of<scav_port_slot>(c, "scav.geom.portslot", ports.off) };
  CHECK(on_border({ .x = slot.x, .y = slot.y }, state_rect(c, comp)));
  CHECK(slot.boundary_depth == 0);
  CHECK(slot.side <= 3);

  // Through the port it was split at, along a leg rather than as a vertex: a port
  // a route runs straight through is exactly what the polyline drops.
  bool through_port{ false };
  for (uint32_t k = 0; (k + 1) < r0.len; ++k) {
    scav_point const a{ route[k] };
    scav_point const b{ route[k + 1] };
    bool const along_y{ (a.x == b.x) && (slot.x == a.x) && (slot.y >= imin(a.y, b.y)) &&
                        (slot.y <= imax(a.y, b.y)) };
    bool const along_x{ (a.y == b.y) && (slot.y == a.y) && (slot.x >= imin(a.x, b.x)) &&
                        (slot.x <= imax(a.x, b.x)) };
    if (along_x || along_y) { through_port = true; }
  }
  CHECK(through_port);

  scav_span const r1{ row_of<scav_span>(c, "scav.geom.route", 1) };
  REQUIRE(r1.len == 2);
  scav_rect const rd{ state_rect(c, d1) };
  scav_point const lip{ row_of<scav_point>(c, "scav.geom.point", r1.off) };
  CHECK(lip.x == rd.x + rd.w);  // leaves through the right border
  CHECK(row_of<scav_point>(c, "scav.geom.point", r1.off + 1).x == lip.x + (2 * p.pad));

  CHECK(row_of<scav_span>(c, "scav.geom.route", 2).len == 0);
  CHECK(row_of<scav_span>(c, "scav.geom.port", 2).len == 0);
}

TEST_CASE("layout: path clears trim the route ends by exact integers") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  scav_profile const p{ readable() };

  std::vector<scav_path_clear> const clears{ { .src = 10, .dst = 6 } };
  scav_spaces const s{ .path_clear = clears.data(), .n_path_clear = 1 };
  run(c, s, p);

  // A ranks before B with nothing in the way, so the route is one horizontal leg
  // between their facing borders and the trims apply to x alone.
  scav_span const r{ row_of<scav_span>(c, "scav.geom.route", 0) };
  REQUIRE(r.len == 2);
  scav_point const p0{ row_of<scav_point>(c, "scav.geom.point", r.off) };
  scav_point const p1{ row_of<scav_point>(c, "scav.geom.point", r.off + 1) };
  scav_rect const ra{ state_rect(c, a) };
  scav_rect const rb{ state_rect(c, b) };
  CHECK(ra.x < rb.x);
  CHECK(p0.y == ra.y + (ra.h / 2));
  CHECK(p0.y == p1.y);
  // Trimmed inward from the borders the router attached to, not from centres.
  CHECK(p0.x == (ra.x + ra.w) + 10);
  CHECK(p1.x == rb.x - 6);
}

TEST_CASE("layout: the chart rect bounds every point and every placed box") {
  // A consumer sizes its viewport from this rect (11.7a); bounding only the root
  // submachine clips whatever reached past it.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const z{ build_state(c, root, "Z", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, z, TransKind::External, {});
  build_trans(c, z, a, TransKind::External, {});

  // Path boxes wide enough that wherever they land they push the extent.
  std::vector<scav_path_box> const wide{
    { .subject = 0, .w = 4000, .h = 300, .order = 0 },
    { .subject = 2, .w = 4000, .h = 300, .order = 0 },
  };
  scav_spaces const s{ .path_box = wide.data(), .n_path_box = 2 };
  std::vector<scav_placed> const placed{ run(c, s, readable()) };

  scav_rect const chart{ row_of<scav_rect>(c, "scav.geom.chart", 0) };
  for (scav_placed const &at : placed) {
    CAPTURE(at.x);
    CAPTURE(at.y);
    CHECK(at.x >= chart.x);
    CHECK(at.y >= chart.y);
    CHECK((at.x + at.w) <= (chart.x + chart.w));
    CHECK((at.y + at.h) <= (chart.y + chart.h));
  }
  ColumnId const pts{ column_find(c, "scav.geom.point") };
  REQUIRE(pts.v != INVALID);
  for (uint32_t i = 0; i < column_count(c, pts); ++i) {
    scav_point const at{ row_of<scav_point>(c, "scav.geom.point", i) };
    CAPTURE(i);
    CHECK(at.x >= chart.x);
    CHECK(at.y >= chart.y);
    CHECK(at.x <= (chart.x + chart.w));
    CHECK(at.y <= (chart.y + chart.h));
  }
}

TEST_CASE("layout: a wide placed box is slid inside rather than hung off") {
  // A box is as wide as its text and its leg can be far shorter near a frame's
  // edge, so centring alone puts half the label outside and grows the canvas.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  scav_profile const p{ readable() };
  std::vector<scav_placed> const bare{ run(c, {}, p) };
  scav_rect const before{ row_of<scav_rect>(c, "scav.geom.chart", 0) };

  // Wider than the route it rides on, but still narrower than the chart.
  std::vector<scav_path_box> const boxes{
    { .subject = 0, .w = before.w - 1, .h = 8, .order = 0 }
  };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 1 };
  std::vector<scav_placed> const placed{ run(c, s, p) };
  REQUIRE(placed.size() == 1);
  scav_rect const after{ row_of<scav_rect>(c, "scav.geom.chart", 0) };

  CHECK(placed[0].x >= after.x);
  CHECK(placed[0].y >= after.y);
  CHECK((placed[0].x + placed[0].w) <= (after.x + after.w));
  CHECK((placed[0].y + placed[0].h) <= (after.y + after.h));
}

TEST_CASE("layout: a placed box rides a leg of its own route, clear of every other") {
  // Phase 1 widens a rank boundary by the widest box crossing it (11.3), and
  // placement puts the box on a strip beside one of that route's legs — the one
  // that leaves it nearer its own line than any stranger's (11.6).
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, d, TransKind::External, {});
  build_trans(c, d, b, TransKind::External, {});

  std::vector<scav_path_box> const boxes{
    { .subject = 2, .w = 240, .h = 80, .order = 0 }
  };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 1 };
  std::vector<scav_placed> const placed{ run(c, s, readable()) };
  REQUIRE(placed.size() == 1);

  // The nearest leg of each route, found here rather than trusted from the
  // implementation.
  struct Near {
    int64_t away{ -1 };
    scav_rect leg{};
  };
  auto const nearest = [&c, &placed](uint32_t trans) {
    Near out;
    scav_span const r{ row_of<scav_span>(c, "scav.geom.route", trans) };
    for (uint32_t k = 0; (k + 1) < r.len; ++k) {
      scav_point const p0{ row_of<scav_point>(c, "scav.geom.point", r.off + k) };
      scav_point const p1{ row_of<scav_point>(c, "scav.geom.point", r.off + k + 1) };
      scav_rect const leg{ span_rect(p0, p1) };
      int64_t const away{ chebyshev_gap(placed[0], leg) };
      if ((out.away < 0) || (away < out.away)) { out = { .away = away, .leg = leg }; }
    }
    return out;
  };

  Near const own{ nearest(2) };
  REQUIRE(own.away >= 0);
  // A strip is a whole box height off the leg, and there are five of them.
  CHECK((own.away % placed[0].h) == 0);
  CHECK(own.away < (5 * placed[0].h));
  // Beside the leg's run rather than off one of its ends, which is what makes
  // the gap above the perpendicular one the strips are cut on.
  if (own.leg.h == 0) {
    CHECK(placed[0].x <= (own.leg.x + own.leg.w));
    CHECK(own.leg.x <= (placed[0].x + placed[0].w));
  } else {
    CHECK(placed[0].y <= (own.leg.y + own.leg.h));
    CHECK(own.leg.y <= (placed[0].y + placed[0].h));
  }

  // One line of its own text nearer its own route than either stranger's, which
  // is what `label_near` charges for and what placement minimises first.
  int64_t const other{ imin(nearest(0).away, nearest(1).away) };
  REQUIRE(other >= 0);
  CHECK((own.away + placed[0].h) <= other);
}

TEST_CASE("layout: reruns rewrite in place, bumping only the generation") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  scav_profile const p{ readable() };

  run(c, {}, p);
  uint32_t const columns_after_first{ static_cast<uint32_t>(c.columns.size()) };
  uint32_t const coord{ layout_coordinate_hash(c) };
  uint32_t const structure{ layout_structural_hash(c) };

  run(c, {}, p);
  CHECK(static_cast<uint32_t>(c.columns.size()) == columns_after_first);
  CHECK(row_of<uint32_t>(c, "scav.geom.gen", 0) == 2);
  CHECK(layout_coordinate_hash(c) == coord);
  CHECK(layout_structural_hash(c) == structure);
}

TEST_CASE("layout: the hash split separates size changes from shape changes") {
  auto build = [](int32_t min_w) {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
    build_trans(c, a, b, TransKind::External, {});
    std::vector<scav_box_space> boxes(c.states.size());
    boxes[0].min_w = min_w;
    boxes[1].min_w = min_w;
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()) };
    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    REQUIRE(layout_run(c, s, opts(readable()), placed, diags));
    return c;
  };

  // A self-loop's route leaves and re-enters one border, so its shape cannot
  // depend on how wide the box is: widening moves every coordinate and no turn.
  auto loop = [](int32_t min_w) {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
    build_trans(c, a, a, TransKind::External, {});
    std::vector<scav_box_space> boxes(c.states.size());
    boxes[0].min_w = min_w;
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()) };
    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    REQUIRE(layout_run(c, s, opts(readable()), placed, diags));
    return c;
  };
  CHECK(layout_coordinate_hash(loop(0)) != layout_coordinate_hash(loop(4000)));
  CHECK(layout_structural_hash(loop(0)) == layout_structural_hash(loop(4000)));

  Chart const narrow{ build(0) };
  Chart const wide{ build(4000) };
  // A size change that reflows the ranks moves the structural hash too, the
  // honest limit of the split: sizing feeds back into shape through the fold.
  CHECK(layout_coordinate_hash(narrow) != layout_coordinate_hash(wide));
  CHECK(layout_structural_hash(narrow) != layout_structural_hash(wide));

  Chart more{ build(0) };
  build_trans(more, { 1 }, { 0 }, TransKind::External, {});
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  REQUIRE(layout_run(more, {}, opts(p), placed, diags));
  CHECK(layout_structural_hash(more) != layout_structural_hash(narrow));
}

TEST_CASE("layout: the inputs digest hears every input that is not the model") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  CHECK(layout_inputs_digest(c) == 0);  // never laid out

  run(c, {}, readable());
  uint32_t const base{ layout_inputs_digest(c) };
  CHECK(base != 0);
  run(c, {}, readable());
  CHECK(layout_inputs_digest(c) == base);  // same inputs, same digest

  // A profile knob layout does read, and one it does not: both are inputs a
  // golden was produced under, so both move the digest.
  scav_profile moved{ readable() };
  moved.pad += 1;
  run(c, {}, moved);
  CHECK(layout_inputs_digest(c) != base);

  scav_profile renamed{ readable() };
  renamed.profile_version += 1;
  run(c, {}, renamed);
  CHECK(layout_inputs_digest(c) != base);

  // The space tables ride in, which is how the font reaches a digest it can
  // never be an argument to.
  std::vector<scav_box_space> boxes(c.states.size());
  boxes[0].min_w = 64;
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };
  run(c, s, readable());
  CHECK(layout_inputs_digest(c) != base);

  // And it is a third value, not a seed: a space change that leaves the shape
  // alone must not disturb the structural hash.
  run(c, {}, readable());
  uint32_t const structure{ layout_structural_hash(c) };
  run(c, s, readable());
  CHECK(layout_structural_hash(c) == structure);
  CHECK(layout_inputs_digest(c) != base);
}

TEST_CASE("layout: the router carries a version, and both stop at the end") {
  scav_byte const *name{ nullptr };
  uint32_t len{ 0 };
  uint32_t version{ 0 };
  REQUIRE(router_name(0, name, len));
  REQUIRE(router_version(0, version));
  CHECK(version >= 1);
  CHECK(!router_name(router_count(), name, len));
  CHECK(!router_version(router_count(), version));
}

TEST_CASE("layout: composed geometry past the domain is rejected, columns kept") {
  // Maximal bands at every nesting level compound the enclosing heights past
  // COORD_MAX in three levels: legal inputs, illegal composition.
  Chart c;
  SubmachineId parent{ build_chart(c, "t", {}) };
  for (uint32_t i = 0; i < 3; ++i) {
    StateId const comp{ build_state(c, parent, {}, StateKind::Normal, {}) };
    parent = build_submachine(c, comp, {}, {});
  }
  std::vector<scav_box_space> boxes(
      c.states.size(),
      { .min_w = 0, .h_before = SPACE_MAX, .h_after = SPACE_MAX });
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  CHECK(!layout_run(c, s, opts(readable()), placed, diags));
  REQUIRE(!diags.empty());
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::State);
  CHECK(column_find(c, "scav.geom.state").v == INVALID);  // nothing written
}

TEST_CASE("layout: a rank taller than the domain is rejected") {
  // A fan puts five maximal-height states in one rank, which is the axis no
  // fold reclaims, and the root submachine is the frame no state bounds.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const source{ build_state(c, root, {}, StateKind::Normal, {}) };
  for (uint32_t i = 0; i < 5; ++i) {
    build_trans(c,
                source,
                build_state(c, root, {}, StateKind::Normal, {}),
                TransKind::External,
                {});
  }
  std::vector<scav_box_space> const boxes(
      c.states.size(),
      { .min_w = 0, .h_before = SPACE_MAX, .h_after = 0 });
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  CHECK(!layout_run(c, s, opts(readable()), placed, diags));
  REQUIRE(!diags.empty());
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::Submachine);
  CHECK(diags[0].subject.ordinal == root.v);
  CHECK(column_find(c, "scav.geom.state").v == INVALID);
}

TEST_CASE("layout: a geometry column of another shape stops the run") {
  scav_profile const p{ readable() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});

  // Four bytes a row where layout writes a sixteen-byte rect: a run that wrote
  // through this column would leave three quarters of every row past its bytes.
  REQUIRE(
      column_register(c, "scav.geom.state", ElemKind::State, ValueKind::U32, 4, 4, 0).v !=
      INVALID);
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  CHECK(!layout_run(c, {}, opts(p), placed, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::GeometryColumnClash);
  CHECK(diags[0].subject.kind == ElemKind::Chart);

  // The clash was found before any geometry: the column still holds the zeros
  // registration gave it, and no other geometry column exists at all.
  ColumnId const clashing{ column_find(c, "scav.geom.state") };
  REQUIRE(clashing.v != INVALID);
  REQUIRE(column_count(c, clashing) == 1);
  uint32_t row{ INVALID };
  std::memcpy(&row, column_data(c, clashing), 4);
  CHECK(row == 0);
  CHECK(column_find(c, "scav.geom.chart").v == INVALID);
  CHECK(column_find(c, "scav.geom.sub").v == INVALID);

  // The entity is checked as well as the width, an application indexing the
  // same rects by submachine being the likelier collision.
  Chart by_entity;
  SubmachineId const other{ build_chart(by_entity, "t", {}) };
  build_state(by_entity, other, "A", StateKind::Normal, {});
  REQUIRE(column_register(by_entity,
                          "scav.geom.state",
                          ElemKind::Submachine,
                          ValueKind::Pod,
                          sizeof(scav_rect),
                          4,
                          COLUMN_DERIVED)
              .v != INVALID);
  diags.clear();
  CHECK(!layout_run(by_entity, {}, opts(p), placed, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::GeometryColumnClash);

  // Layout's own shape under layout's own name is layout's own column to
  // overwrite, however it got there.
  Chart same;
  SubmachineId const same_root{ build_chart(same, "t", {}) };
  StateId const b{ build_state(same, same_root, "A", StateKind::Normal, {}) };
  REQUIRE(column_register(same,
                          "scav.geom.state",
                          ElemKind::State,
                          ValueKind::Pod,
                          sizeof(scav_rect),
                          4,
                          COLUMN_DERIVED)
              .v != INVALID);
  run(same, {}, p);
  CHECK(state_rect(same, b).w == (p.kind_min_w[0] + (2 * p.pad)));
}

TEST_CASE("layout: invalid profiles and spaces fail before any geometry") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_profile bad{ readable() };
  bad.dar_den = 0;
  CHECK(!layout_run(c, {}, opts(bad), placed, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::ProfileOutOfRange);

  diags.clear();
  std::vector<scav_box_space> boxes{ { .min_w = -1, .h_before = 0, .h_after = 0 } };
  scav_spaces const s{ .box_state = boxes.data(), .n_box_state = 1 };
  CHECK(!layout_run(c, s, opts(readable()), placed, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::SpaceOutOfRange);
  CHECK(column_find(c, "scav.geom.state").v == INVALID);
}

TEST_CASE("layout: tombstones leave zero rects and no routes") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  c.states[b.v].live = 0;
  c.transitions[0].live = 0;
  run(c, {}, readable());

  CHECK((state_rect(c, b) == scav_rect{}));
  CHECK(row_of<scav_span>(c, "scav.geom.route", 0).len == 0);
  // The dead sibling neither occupies a slot nor leaves a gap: two live
  // components pack side by side with nothing between them.
  CHECK(state_rect(c, a).x == 0);
  CHECK(state_rect(c, d).x == state_rect(c, a).w + readable().node_sep);
  CHECK(state_rect(c, d).y == 0);
}

TEST_CASE("layout: the C surface runs, queries, and reports end to end") {
  scav_load *loader{ nullptr };
  REQUIRE(scav_load_begin(&loader) == SCAV_OK);
  std::string_view const text{ R"(chart t { state A, state B, trans A -> B, })" };
  REQUIRE(scav_load_add(loader,
                        reinterpret_cast<scav_byte const *>(text.data()),
                        static_cast<uint32_t>(text.size()),
                        "t.scav") == SCAV_OK);
  scav_chart *chart{ nullptr };
  REQUIRE(scav_load_finish(loader, &chart) == SCAV_OK);

  scav_layout_opts opts{};
  REQUIRE(scav_profile_named("compact", &opts.profile) == SCAV_OK);

  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 10, .h = 4, .order = 0 } };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 1 };

  // The out-param protocol: query, too small, filled.
  uint32_t count{ 0 };
  REQUIRE(scav_layout_run(chart, &s, &opts, nullptr, 0, &count) == SCAV_OK);
  CHECK(count == 1);
  scav_placed one{};
  CHECK(scav_layout_run(chart, &s, &opts, &one, 1, &count) == SCAV_OK);
  CHECK(one.w == 10);

  // Geometry reads back through the three-call accessor.
  scav_column_id id{ 0 };
  REQUIRE(scav_column_find(chart, "scav.geom.state", &id) == SCAV_OK);
  uint32_t rows{ 0 };
  uint32_t stride{ 0 };
  scav_byte const *data{ nullptr };
  REQUIRE(scav_column_count(chart, id, &rows) == SCAV_OK);
  REQUIRE(scav_column_data(chart, id, &data, &stride) == SCAV_OK);
  CHECK(stride == sizeof(scav_rect));
  CHECK(rows == chart->chart.states.size());

  // A bad router id is an argument error; a bad profile is a diagnosed one.
  opts.router = 99;
  CHECK(scav_layout_run(chart, &s, &opts, &one, 1, &count) == SCAV_E_INVALID_ARG);
  opts.router = 0;
  opts.profile.trybox = 7;
  CHECK(scav_layout_run(chart, &s, &opts, &one, 1, &count) == SCAV_E_LAYOUT);
  uint32_t n_diags{ 0 };
  REQUIRE(scav_chart_diag_count(chart, &n_diags) == SCAV_OK);
  REQUIRE(n_diags == 1);
  scav_diag d{};
  REQUIRE(scav_chart_diag(chart, 0, &d) == SCAV_OK);
  CHECK(d.code == static_cast<uint32_t>(DiagCode::ProfileOutOfRange));

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}

namespace {

// The invariants any laid-out chart owes, checked from the columns alone.
void check_geometry(Chart const &c) {
  SplitGraph const g{ decompose(c) };
  scav_rect const chart_box{ row_of<scav_rect>(c, "scav.geom.chart", 0) };

  for (uint32_t i = 0; i < c.states.size(); ++i) {
    scav_rect const r{ state_rect(c, { i }) };
    if (c.states[i].live == 0) {
      CHECK((r == scav_rect{}));
      continue;
    }
    CHECK(r.w > 0);
    CHECK(inside(r, sub_rect(c, c.states[i].parent)));
    CHECK(inside(row_of<scav_rect>(c, "scav.geom.state_before", i), r));
    CHECK(inside(row_of<scav_rect>(c, "scav.geom.state_after", i), r));
    CHECK(static_cast<Wide>(r.x) + r.w <= COORD_MAX);
    CHECK(static_cast<Wide>(r.y) + r.h <= COORD_MAX);
  }
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    StateId const owner{ c.submachines[m].owner };
    if (owner.v != INVALID) {
      CHECK(inside(sub_rect(c, { m }), state_rect(c, owner)));
    } else {
      CHECK(inside(sub_rect(c, { m }), chart_box));
    }
  }
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    scav_span const route{ row_of<scav_span>(c, "scav.geom.route", t) };
    scav_span const ports{ row_of<scav_span>(c, "scav.geom.port", t) };
    Span const segs{ g.trans_segments[t] };
    if (segs.len == 0) {
      CHECK(route.len == 0);
      CHECK(ports.len == 0);
      continue;
    }
    Transition const &tr{ c.transitions[t] };
    // One point per endpoint and per crossing, plus a bend wherever the
    // layering put one between two ranks, which is why this is a floor.
    // `served` below takes off the crossings an endpoint already stands on.
    if (tr.src == tr.dst) {
      CHECK(route.len == 2);
    } else {
      // A crossing on the source's or the destination's own border is not a
      // second point: the route reaches that state by reaching that border, and
      // 11.5's aimed attachment puts both on the same place rather than running
      // a leg along the border between them.
      uint32_t served{ 0 };
      for (uint32_t k = 0; (k + 1) < segs.len; ++k) {
        StateId const on{ g.ports[g.segments[segs.off + k].dst_port].state };
        served += ((on.v == tr.src.v) || (on.v == tr.dst.v)) ? 1U : 0U;
      }
      CHECK(route.len + served >= segs.len + 1);
    }
    CHECK(ports.len == segs.len - 1);
    for (uint32_t k = 0; k < ports.len; ++k) {
      scav_port_slot const slot{
        row_of<scav_port_slot>(c, "scav.geom.portslot", ports.off + k)
      };
      SplitPort const &port{ g.ports[g.segments[segs.off + k].dst_port] };
      scav_rect const boundary{ (port.state.v != INVALID) ? state_rect(c, port.state)
                                                          : sub_rect(c, port.sub) };
      CHECK(on_border({ .x = slot.x, .y = slot.y }, boundary));
      CHECK(slot.side <= 3);
    }
  }
}

}  // namespace

TEST_CASE("layout: geometry invariants hold across topologies and spaces") {
  // The split sweep's fixture: mixed depth, concurrency, nesting in a region.
  auto fixture = [] {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    build_state(c, root, "A", StateKind::Normal, {});
    StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
    SubmachineId const b_m{ build_submachine(c, b, {}, {}) };
    build_state(c, b_m, "B1", StateKind::Normal, {});
    StateId const b2{ build_state(c, b_m, "B2", StateKind::Normal, {}) };
    SubmachineId const b2_m{ build_submachine(c, b2, {}, {}) };
    build_state(c, b2_m, "B21", StateKind::Normal, {});
    StateId const o{ build_state(c, root, "O", StateKind::Normal, {}) };
    SubmachineId const m1{ build_submachine(c, o, "m1", {}) };
    SubmachineId const m2{ build_submachine(c, o, "m2", {}) };
    StateId const p{ build_state(c, m1, "P", StateKind::Normal, {}) };
    SubmachineId const p_m{ build_submachine(c, p, {}, {}) };
    build_state(c, p_m, "P1", StateKind::Normal, {});
    build_state(c, m2, "Q", StateKind::Normal, {});
    return c;
  };

  Chart c{ fixture() };
  uint32_t const n{ static_cast<uint32_t>(c.states.size()) };
  for (uint32_t src = 0; src < n; ++src) {
    for (uint32_t dst = 0; dst < n; ++dst) {
      for (TransKind const kind :
           { TransKind::External, TransKind::Internal, TransKind::Local }) {
        build_trans(c, { src }, { dst }, kind, {});
      }
    }
  }

  SUBCASE("with no space requests") { run(c, {}, readable()); }
  SUBCASE("with fabricated measurement") {
    // A pure integer function of the model, the way a real app measures.
    std::vector<scav_box_space> boxes(c.states.size());
    for (uint32_t i = 0; i < c.states.size(); ++i) {
      boxes[i] = { .min_w = static_cast<int32_t>(200 + (i * 40)),
                   .h_before = static_cast<int32_t>(30 + (i % 3) * 10),
                   .h_after = static_cast<int32_t>((i % 2) * 20) };
    }
    std::vector<scav_path_clear> clears(c.transitions.size(), { .src = 4, .dst = 8 });
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()),
                         .path_clear = clears.data(),
                         .n_path_clear = static_cast<uint32_t>(clears.size()) };
    run(c, s, readable());
  }
  check_geometry(c);
}

namespace {

// The scale target: depth 16, ~2k states, ~3.7k transitions including one
// long hierarchical edge per level.
Chart two_k_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  for (uint32_t r = 0; r < 8; ++r) {
    SubmachineId parent{ root };
    StateId last{ INVALID };
    for (uint32_t d = 0; d < 16; ++d) {
      std::vector<StateId> level;
      level.reserve(15);
      for (uint32_t k = 0; k < 15; ++k) {
        level.push_back(build_state(c, parent, {}, StateKind::Normal, {}));
      }
      StateId const comp{ build_state(c, parent, {}, StateKind::Normal, {}) };
      for (uint32_t k = 1; k < level.size(); ++k) {
        build_trans(c, level[k - 1], level[k], TransKind::External, {});
        build_trans(c, level[k], comp, TransKind::External, {});
      }
      if (last.v != INVALID) { build_trans(c, comp, last, TransKind::External, {}); }
      last = comp;
      parent = build_submachine(c, comp, {}, {});
    }
  }
  return c;
}

}  // namespace

TEST_CASE("layout: two thousand states lay out, and quickly") {
  Chart c{ two_k_chart() };
  REQUIRE(c.states.size() >= 2000);
  REQUIRE(c.transitions.size() >= 3500);

  auto const t0{ std::chrono::steady_clock::now() };
  run(c, {}, readable());
  auto const t1{ std::chrono::steady_clock::now() };
  auto const us{ std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() };
  MESSAGE("layout_run over ",
          c.states.size(),
          " states / ",
          c.transitions.size(),
          " transitions: ",
          us,
          " us");
#if SCAV_PERF_ASSERT_FLOOR == 1
  // A floor, not a time: named machines only, never instrumented. ~47 ms here
  // against P6's 8 ms, the difference being a graph and a search per net. It
  // catches an accidental quadratic, not that number.
  CHECK(us < 200000);
#endif
  check_geometry(c);
}

TEST_CASE("layout: the flat two thousand lay out too, and quickly") {
  // One submachine holding the whole scale target is legal input, and the shape
  // the per-submachine cost bounds assume away (11.3).
  Chart c;
  SubmachineId const root{ build_chart(c, "flat", {}) };
  std::vector<StateId> all;
  all.reserve(2048);
  for (uint32_t i = 0; i < 2048; ++i) {
    all.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < all.size(); ++i) {
    build_trans(c, all[i - 1], all[i], TransKind::External, {});
    if ((i % 16) == 0) { build_trans(c, all[i], all[i - 16], TransKind::External, {}); }
  }
  REQUIRE(c.transitions.size() >= 2000);

  auto const t0{ std::chrono::steady_clock::now() };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  bool const laid{ layout_run(c, {}, opts(readable()), placed, diags) };
  auto const t1{ std::chrono::steady_clock::now() };
  auto const us{ std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() };
  MESSAGE("flat layout_run over ",
          c.states.size(),
          " states / ",
          c.transitions.size(),
          " transitions: ",
          us,
          " us, laid out: ",
          laid);
  // Whether it fits the domain is the model's business; that it terminates
  // without a quadratic is this phase's.
  if (!laid) {
    REQUIRE(!diags.empty());
    CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  }
#if SCAV_PERF_ASSERT_FLOOR == 1
  // Every one of the 2,048 boxes in one frame, so the largest single routing
  // graph any chart produces. ~215 ms against the nested shape's ~47 ms.
  CHECK(us < 500000);
#endif
}

TEST_CASE("layout: no corpus chart runs a route flush along a box") {
  // The router's own suite proves clearance over the graph; what it cannot see is
  // a box flush against the *frame's* edge, which has no room for a lane. Phase 3
  // owns the margin, and `brew` is where the shape occurs.
  scav_profile const p{ readable() };
  std::string report;
  uint32_t reseated{ 0 };
  for (char const *name : { "axis.scav",
                            "bottler.scav",
                            "brew.scav",
                            "dock.scav",
                            "estop.scav",
                            "led.scav",
                            "mill.scav",
                            "ota.scav",
                            "tcp.scav",
                            "toolchanger.scav",
                            "vac.scav" }) {
    CAPTURE(name);
    std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
    path += name;
    Loader loader;
    Chart c;
    std::vector<Diagnostic> diags;
    std::string failed;
    REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
    run(c, {}, p);
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ phase1_order(c, g, {}, p) };
    SizedLayout z;
    REQUIRE(phase2_size(c, g, o, {}, p, z, diags));
    Routes const routes{ phase3_route(c, g, o, z, {}, p, *router_at(0)) };
    reseated += routes.reseated;

    for (uint32_t t = 0; t < c.transitions.size(); ++t) {
      scav_span const route{ row_of<scav_span>(c, "scav.geom.route", t) };
      for (uint32_t k = 0; (k + 1) < route.len; ++k) {
        scav_point const a{ row_of<scav_point>(c, "scav.geom.point", route.off + k) };
        scav_point const b{ row_of<scav_point>(c, "scav.geom.point", route.off + k + 1) };
        for (uint32_t st = 0; st < c.states.size(); ++st) {
          if (c.states[st].live == 0) { continue; }
          scav_rect const r{ row_of<scav_rect>(c, "scav.geom.state", st) };
          bool const along_cap{ (a.y == b.y) && ((a.y == r.y) || (a.y == (r.y + r.h))) &&
                                (imin(a.x, b.x) < (r.x + r.w)) && (imax(a.x, b.x) > r.x) };
          bool const along_side{ (a.x == b.x) && ((a.x == r.x) || (a.x == (r.x + r.w))) &&
                                 (imin(a.y, b.y) < (r.y + r.h)) &&
                                 (imax(a.y, b.y) > r.y) };
          if (!along_cap && !along_side) { continue; }
          report += name;
          report += " transition ";
          report += std::to_string(t);
          report += " (";
          report += std::to_string(a.x);
          report += ",";
          report += std::to_string(a.y);
          report += ")-(";
          report += std::to_string(b.x);
          report += ",";
          report += std::to_string(b.y);
          report += ") along state ";
          report += std::to_string(st);
          report += "\n";
        }
      }
    }
  }
  // No net gave up its clearance, so what is left is not 11.5's degradation.
  CHECK(reseated == 0);
  MESSAGE("routes flush against a box:\n", report);
  // All three are separator ports: such a port sits on a submachine rect flush
  // with a child's border and lays a lane there. Same cause as the stubs below,
  // same fix -- 11.5's LCA-owned separator channel, P7c's. Pinned so it cannot grow.
  uint32_t lines{ 0 };
  for (char const ch : report) {
    if (ch == '\n') { ++lines; }
  }
  CHECK(lines <= 3);
}

TEST_CASE("layout: Tier 0 at the scale target, and where the grid gives out") {
  // The corpus fits the budget; these two are the shapes that might not, and
  // which of them still routes is the finding rather than an assumption.
  {
    Chart c{ two_k_chart() };
    scav_profile const p{ readable() };
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ phase1_order(c, g, {}, p) };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(phase2_size(c, g, o, {}, p, z, diags));
    Routes const r{ phase3_route(c, g, o, z, {}, p, *router_at(0)) };
    CostTerms const t{ cost_terms(c, g, z, r, {}, p) };
    MESSAGE("nested 2k: through_box ",
            t.through_box,
            ", outside ",
            r.outside_region,
            ", unreachable ",
            r.unreachable,
            ", too large ",
            r.too_large,
            " of ",
            g.segments.size());
    // No net degrades: every frame here fits the budget and every end is
    // reachable, so the router routed all 3,704 of them.
    CHECK(r.degraded() == 0);
    // A separator port sits inside its owner and 11.5 gives the segment to the
    // parent frame, where the owner is an obstacle walling off its own port. The
    // stub out of it used to cross whatever lay between, 496 times on this shape;
    // leaving through the face the flow runs through crosses nothing (11.5).
    CHECK(t.through_box == 0);
  }
  {
    Chart c;
    SubmachineId const root{ build_chart(c, "flat", {}) };
    std::vector<StateId> all;
    all.reserve(2048);
    for (uint32_t i = 0; i < 2048; ++i) {
      all.push_back(build_state(c, root, {}, StateKind::Normal, {}));
    }
    for (uint32_t i = 0; (i + 1) < all.size(); ++i) {
      build_trans(c, all[i], all[i + 1], TransKind::External, {});
    }
    scav_profile const p{ readable() };
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ phase1_order(c, g, {}, p) };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    if (phase2_size(c, g, o, {}, p, z, diags)) {
      Routes const r{ phase3_route(c, g, o, z, {}, p, *router_at(0)) };
      CostTerms const t{ cost_terms(c, g, z, r, {}, p) };
      // The grid is the product of two line sets, not a function of box count, and a
      // packed grid shares columns and rows -- so this fits. What blows the budget is
      // boxes at distinct offsets, which the router's own suite builds.
      MESSAGE("flat 2k: through_box ", t.through_box, ", too large ", r.too_large);
      CHECK(r.degraded() == 0);
      CHECK(t.through_box == 0);
    }
  }
}

namespace {

// The bar ranks before `outer` and stands taller than it, so under the profile
// below it spans `outer`'s frame and walls off the route into `deep`.
Chart sealed_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const bar{ build_state(c, root, "S", StateKind::Fork, {}) };
  StateId const outer{ build_state(c, root, "P", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, {}, {}) };
  StateId const deep{ build_state(c, inner, "C", StateKind::Normal, {}) };
  build_trans(c, bar, deep, TransKind::External, {});
  return c;
}

// Clearance is a third of `node_sep`, so this asks for 192 grid units of it
// across a rank boundary nothing wide.
scav_profile sealed_profile() {
  scav_profile p{ readable() };
  p.pad = 16;
  p.rank_sep = 0;
  p.node_sep = 576;
  return p;
}

scav_point last_point(Chart const &c, uint32_t trans) {
  scav_span const route{ row_of<scav_span>(c, "scav.geom.route", trans) };
  REQUIRE(route.len >= 2);
  return row_of<scav_point>(c, "scav.geom.point", (route.off + route.len) - 1);
}

}  // namespace

namespace scav {

// The decision `layout.cpp` brackets with SCAV_INTERNAL, declared here rather
// than in a header so the shipping build keeps it internal.
bool inflation_done(uint32_t fewest, uint32_t degraded, uint32_t unreachable, bool &keep);

}  // namespace scav

TEST_CASE("layout: only a kept inflation attempt ends the retry loop") {
  // The loop exists to remove unreachable ends, and the geometry that ships is
  // whichever attempt was kept. An attempt is reachable-but-worse when a wider
  // spacing trades unreachable nets for `outside_region` or `too_large` ones --
  // no chart drives the shipped router there, which is why this asks the
  // decision directly rather than through a fixture.
  bool keep{ true };

  // The case the loop got wrong: every end reached, but more degraded overall,
  // so the attempt is discarded -- and a discarded attempt cannot stop the loop
  // or the run ships the unreachable ends it was retrying to remove.
  CHECK(!inflation_done(3, 4, 0, keep));
  CHECK(!keep);
  // Equal is not better, so it is discarded on the same grounds.
  CHECK(!inflation_done(3, 3, 0, keep));
  CHECK(!keep);

  // Better and complete: kept, and the loop is finished.
  CHECK(inflation_done(3, 0, 0, keep));
  CHECK(keep);

  // Better but still short: kept, and the loop runs on to widen again.
  CHECK(!inflation_done(3, 1, 1, keep));
  CHECK(keep);

  // Worse and still short: neither kept nor finished.
  CHECK(!inflation_done(1, 2, 2, keep));
  CHECK(!keep);
}

TEST_CASE("layout: a sealed channel is opened by inflating the spacing") {
  Chart c{ sealed_chart() };
  scav_profile const p{ sealed_profile() };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 0 };
  REQUIRE(layout_run(c, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 3);
  CHECK(diags.empty());
  // A routed end sits on C's border; the straight-line fallback leaves it at
  // C's centre instead.
  CHECK(on_border(last_point(c, 0), state_rect(c, { 2 })));
}

TEST_CASE("layout: at the inflation cap the degraded transition is diagnosed") {
  Chart c{ sealed_chart() };
  scav_profile p{ sealed_profile() };
  p.spacing_inflation_cap = 1;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 0 };
  REQUIRE(layout_run(c, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 0);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::RouteDegraded);
  CHECK(diags[0].subject.kind == ElemKind::Transition);
  CHECK(diags[0].subject.ordinal == 0);

  // The geometry is written all the same, with the straight line in it.
  scav_rect const deep{ state_rect(c, { 2 }) };
  scav_point const end{ last_point(c, 0) };
  CHECK(end.x == (deep.x + (deep.w / 2)));
  CHECK(end.y == (deep.y + (deep.h / 2)));
  CHECK(inside(deep, state_rect(c, { 1 })));
  CHECK(inside(state_rect(c, { 1 }), row_of<scav_rect>(c, "scav.geom.chart", 0)));
  CHECK(row_of<uint32_t>(c, "scav.geom.gen", 0) == 1);
}

TEST_CASE("layout: a cap or an increment of zero never retries") {
  Chart capped{ sealed_chart() };
  scav_profile p{ sealed_profile() };
  p.spacing_inflation_cap = 0;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 0 };
  REQUIRE(layout_run(capped, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 0);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::RouteDegraded);

  // One retry is one attempt and this chart needs three, so both runs write the
  // same straight line.
  Chart once{ sealed_chart() };
  p.spacing_inflation_cap = 1;
  std::vector<Diagnostic> again;
  REQUIRE(layout_run(once, {}, opts(p), placed, again, &inflations));
  CHECK(inflations == 0);
  CHECK(layout_coordinate_hash(once) == layout_coordinate_hash(capped));

  // Eight retries that widen nothing are eight copies of the first attempt.
  Chart flat_increment{ sealed_chart() };
  p.spacing_inflation_cap = 8;
  p.spacing_inflation_increment = 0;
  std::vector<Diagnostic> unwidened;
  REQUIRE(layout_run(flat_increment, {}, opts(p), placed, unwidened, &inflations));
  CHECK(inflations == 0);
  CHECK(unwidened.size() == 1);
  CHECK(layout_coordinate_hash(flat_increment) == layout_coordinate_hash(capped));
}

TEST_CASE("layout: an increment the validator refuses ends the retries") {
  scav_profile p{ sealed_profile() };
  p.spacing_inflation_increment = SPACE_MAX;
  REQUIRE(profile_validate(p));
  scav_profile widened{ p };
  widened.rank_sep += p.spacing_inflation_increment;
  widened.node_sep += p.spacing_inflation_increment;
  widened.sub_sep += p.spacing_inflation_increment;
  REQUIRE(!profile_validate(widened));

  Chart c{ sealed_chart() };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 9 };
  REQUIRE(layout_run(c, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 0);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::RouteDegraded);

  // The first attempt's geometry, which is what a run that never retried writes.
  Chart never{ sealed_chart() };
  scav_profile no_retry{ sealed_profile() };
  no_retry.spacing_inflation_cap = 0;
  std::vector<Diagnostic> other;
  REQUIRE(layout_run(never, {}, opts(no_retry), placed, other, &inflations));
  CHECK(layout_coordinate_hash(c) == layout_coordinate_hash(never));
}

namespace {

// The sealed chart with a rank chain beside it: wide enough that a retry's
// spacing composes past the coordinate domain even though the profile is legal.
Chart sealed_with_chain() {
  Chart c{ sealed_chart() };
  SubmachineId const root{ 0 };
  std::vector<StateId> chain;
  chain.reserve(16);
  for (uint32_t i = 0; i < 16; ++i) {
    chain.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < chain.size(); ++i) {
    build_trans(c, chain[i - 1], chain[i], TransKind::External, {});
  }
  return c;
}

}  // namespace

TEST_CASE("layout: a retry whose sizing leaves the domain ends them too") {
  scav_profile p{ sealed_profile() };
  p.spacing_inflation_increment = 120000;
  REQUIRE(profile_validate(p));
  scav_profile widened{ p };
  widened.rank_sep += p.spacing_inflation_increment;
  widened.node_sep += p.spacing_inflation_increment;
  widened.sub_sep += p.spacing_inflation_increment;
  REQUIRE(profile_validate(widened));  // the validator lets this copy through

  Chart c{ sealed_with_chain() };
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ phase1_order(c, g, {}, widened) };
  SizedLayout z;
  std::vector<Diagnostic> spilled;
  REQUIRE(!phase2_size(c, g, o, {}, widened, z, spilled));  // sizing is what refuses

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 9 };
  REQUIRE(layout_run(c, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 0);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::RouteDegraded);
  CHECK(diags[0].subject.ordinal == 0);
}

TEST_CASE("layout: an attempt that degrades no less than the best is not taken") {
  // Two retries that neither settle nor improve: the count stays at zero and the
  // geometry is the first attempt's, not the widest one tried.
  Chart twice{ sealed_chart() };
  scav_profile p{ sealed_profile() };
  p.spacing_inflation_cap = 2;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 9 };
  REQUIRE(layout_run(twice, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 0);
  CHECK(diags.size() == 1);

  Chart never{ sealed_chart() };
  p.spacing_inflation_cap = 0;
  std::vector<Diagnostic> other;
  REQUIRE(layout_run(never, {}, opts(p), placed, other, &inflations));
  CHECK(layout_coordinate_hash(twice) == layout_coordinate_hash(never));
}

TEST_CASE("layout: the inflation count is optional") {
  Chart c{ sealed_chart() };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  REQUIRE(layout_run(c, {}, opts(sealed_profile()), placed, diags, nullptr));
  CHECK(diags.empty());

  Chart counted{ sealed_chart() };
  uint32_t inflations{ 0 };
  std::vector<Diagnostic> other;
  REQUIRE(layout_run(counted, {}, opts(sealed_profile()), placed, other, &inflations));
  CHECK(inflations == 3);
  CHECK(layout_coordinate_hash(c) == layout_coordinate_hash(counted));
}

TEST_CASE("layout: a transition every net of which fell back is diagnosed once") {
  // Two boundaries between the bar and its target, so the transition has three
  // nets and the bar seals two of them off.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const bar{ build_state(c, root, "S", StateKind::Fork, {}) };
  StateId const outer{ build_state(c, root, "P", StateKind::Normal, {}) };
  SubmachineId const mid{ build_submachine(c, outer, {}, {}) };
  StateId const inner{ build_state(c, mid, "I", StateKind::Normal, {}) };
  SubmachineId const under{ build_submachine(c, inner, {}, {}) };
  StateId const deep{ build_state(c, under, "C", StateKind::Normal, {}) };
  StateId const beside{ build_state(c, under, "D", StateKind::Normal, {}) };
  build_trans(c, bar, deep, TransKind::External, {});
  build_trans(c, deep, beside, TransKind::External, {});  // routes, so nothing to say

  scav_profile p{ sealed_profile() };
  p.spacing_inflation_cap = 0;
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ phase1_order(c, g, {}, p) };
  SizedLayout z;
  std::vector<Diagnostic> spilled;
  REQUIRE(phase2_size(c, g, o, {}, p, z, spilled));
  Routes const r{ phase3_route(c, g, o, z, {}, p, *router_at(0)) };
  REQUIRE(g.trans_segments[0].len == 3);
  REQUIRE(r.unreachable == 2);
  REQUIRE(r.failed[0] == 1);

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 9 };
  REQUIRE(layout_run(c, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 0);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::RouteDegraded);
  CHECK(diags[0].subject.kind == ElemKind::Transition);
  CHECK(diags[0].subject.ordinal == 0);
}

TEST_CASE("layout: a graph past the router's budget is not a spacing problem") {
  // Cycle breaking turns a thirteen-rank skip into a long one, and the frame that
  // holds them all is the shape whose grid the router refuses.
  Chart c;
  SubmachineId const root{ build_chart(c, "wide", {}) };
  std::vector<StateId> all;
  all.reserve(256);
  for (uint32_t i = 0; i < 256; ++i) {
    all.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < all.size(); ++i) {
    build_trans(c, all[i - 1], all[i], TransKind::External, {});
    build_trans(c, all[i], all[(i + 13) % all.size()], TransKind::External, {});
  }

  scav_profile const p{ readable() };
  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ phase1_order(c, g, {}, p) };
  SizedLayout z;
  std::vector<Diagnostic> spilled;
  REQUIRE(phase2_size(c, g, o, {}, p, z, spilled));
  Routes const r{ phase3_route(c, g, o, z, {}, p, *router_at(0)) };
  REQUIRE(r.too_large > 0);
  REQUIRE(r.unreachable == 0);
  uint32_t marked{ 0 };
  for (uint8_t const one : r.failed) { marked += (one != 0) ? 1U : 0U; }

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 9 };
  REQUIRE(layout_run(c, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 0);  // only an unreachable end moves with spacing
  CHECK(diags.size() == marked);
  uint32_t last{ 0 };
  for (uint32_t i = 0; i < diags.size(); ++i) {
    CAPTURE(i);
    CHECK(diags[i].code == DiagCode::RouteDegraded);
    CHECK(diags[i].subject.kind == ElemKind::Transition);
    bool const ascends{ (i == 0) || (diags[i].subject.ordinal > last) };
    CHECK(ascends);
    last = diags[i].subject.ordinal;
  }
}

TEST_CASE("layout: inflating the profile leaves the inputs digest alone") {
  scav_profile const p{ sealed_profile() };
  Chart sealed{ sealed_chart() };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 0 };
  REQUIRE(layout_run(sealed, {}, opts(p), placed, diags, &inflations));
  REQUIRE(inflations == 3);

  // The digest reads the profile, the router and the spaces and nothing else,
  // so the same caller profile over a chart that never widened lands here too.
  Chart plain;
  SubmachineId const root{ build_chart(plain, "t", {}) };
  build_state(plain, root, "A", StateKind::Normal, {});
  std::vector<Diagnostic> clean;
  REQUIRE(layout_run(plain, {}, opts(p), placed, clean, &inflations));
  CHECK(inflations == 0);
  CHECK(layout_inputs_digest(sealed) == layout_inputs_digest(plain));

  scav_profile q{ p };
  q.rank_sep += 3 * p.spacing_inflation_increment;
  q.node_sep += 3 * p.spacing_inflation_increment;
  q.sub_sep += 3 * p.spacing_inflation_increment;
  Chart widened;
  SubmachineId const other{ build_chart(widened, "t", {}) };
  build_state(widened, other, "A", StateKind::Normal, {});
  std::vector<Diagnostic> spread;
  REQUIRE(layout_run(widened, {}, opts(q), placed, spread, &inflations));
  CHECK(layout_inputs_digest(widened) != layout_inputs_digest(sealed));
}

TEST_CASE("layout: nothing in the corpus or at the scale target inflates") {
  scav_profile const p{ readable() };
  for (char const *name : { "axis.scav",
                            "bottler.scav",
                            "brew.scav",
                            "dock.scav",
                            "estop.scav",
                            "led.scav",
                            "mill.scav",
                            "ota.scav",
                            "tcp.scav",
                            "toolchanger.scav",
                            "vac.scav" }) {
    CAPTURE(name);
    std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
    path += name;
    Loader loader;
    Chart c;
    std::vector<Diagnostic> diags;
    std::string failed;
    REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
    std::vector<scav_placed> placed;
    std::vector<Diagnostic> laid;
    uint32_t inflations{ 1 };
    REQUIRE(layout_run(c, {}, opts(p), placed, laid, &inflations));
    CHECK(inflations == 0);
    CHECK(laid.empty());
  }

  Chart nested{ two_k_chart() };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  uint32_t inflations{ 1 };
  REQUIRE(layout_run(nested, {}, opts(p), placed, diags, &inflations));
  CHECK(inflations == 0);
  CHECK(diags.empty());

  Chart flat;
  SubmachineId const root{ build_chart(flat, "flat", {}) };
  std::vector<StateId> all;
  all.reserve(2048);
  for (uint32_t i = 0; i < 2048; ++i) {
    all.push_back(build_state(flat, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 0; (i + 1) < all.size(); ++i) {
    build_trans(flat, all[i], all[i + 1], TransKind::External, {});
  }
  inflations = 1;
  std::vector<Diagnostic> wide;
  REQUIRE(layout_run(flat, {}, opts(p), placed, wide, &inflations));
  CHECK(inflations == 0);
  CHECK(wide.empty());
}

TEST_CASE("layout: a frame full of long edges terminates, expensively") {
  // The shape the case above is not. Cycle breaking reverses in node order, and a
  // reversed chain edge turns a thirteen-rank skip into a thousand-rank one.
  Chart c;
  SubmachineId const root{ build_chart(c, "wide", {}) };
  std::vector<StateId> all;
  all.reserve(512);
  for (uint32_t i = 0; i < 512; ++i) {
    all.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < all.size(); ++i) {
    build_trans(c, all[i - 1], all[i], TransKind::External, {});
    build_trans(c, all[i], all[(i + 13) % all.size()], TransKind::External, {});
  }

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const o{ phase1_order(c, g, {}, readable()) };
  MESSAGE("512 states with wrapping skips: ",
          o.nodes.size(),
          " ordering nodes, ",
          o.sub_ranks[root.v],
          " ranks");
  CHECK(o.nodes.size() > c.states.size());
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  CHECK((layout_run(c, {}, opts(readable()), placed, diags) || !diags.empty()));
}

TEST_CASE("layout: the coordinate extent estimate holds under fat text") {
  // Generous stand-ins for measured text, so the grid decision errs conservative:
  // twenty wide glyphs, two title lines, a compartment.
  int32_t lo{ 0 };
  int32_t hi{ 8192 };
  scav_rect best{};
  while ((hi - lo) > 16) {
    int32_t const mid{ lo + ((hi - lo) / 2) };
    Chart c{ two_k_chart() };
    std::vector<scav_box_space> const boxes(
        c.states.size(),
        { .min_w = mid, .h_before = 448, .h_after = 96 });
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()) };
    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    if (layout_run(c, s, opts(readable()), placed, diags)) {
      lo = mid;
      best = row_of<scav_rect>(c, "scav.geom.chart", 0);
    } else {
      REQUIRE(!diags.empty());
      CHECK(diags[0].code == DiagCode::CoordinateOverflow);
      hi = mid;
    }
  }
  MESSAGE("widest fabricated min_w the 2k chart lays out: ",
          lo,
          ", at ",
          best.w,
          " x ",
          best.h,
          " of ",
          COORD_MAX);
  CHECK(lo >= 1024);
  CHECK(best.w <= COORD_MAX);
  CHECK(best.h <= COORD_MAX);
}

TEST_CASE("layout: corpus charts hash to the committed golden") {
  // Three columns per chart: the inputs digest naming the measurement policy,
  // then the structural and coordinate hashes it produced.
  std::string actual;
  for (char const *name : { "axis.scav",
                            "bottler.scav",
                            "brew.scav",
                            "dock.scav",
                            "estop.scav",
                            "led.scav",
                            "mill.scav",
                            "ota.scav",
                            "tcp.scav",
                            "toolchanger.scav",
                            "vac.scav" }) {
    CAPTURE(name);
    std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
    path += name;
    Loader loader;
    Chart c;
    std::vector<Diagnostic> diags;
    std::string failed;
    REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
    run(c, {}, readable());
    check_geometry(c);  // the invariants, over real charts and not only fuzz
    actual += name;
    actual += ' ';
    string_append_hex32(actual, layout_inputs_digest(c));
    actual += ' ';
    string_append_hex32(actual, layout_structural_hash(c));
    actual += ' ';
    string_append_hex32(actual, layout_coordinate_hash(c));
    actual += '\n';
  }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/layout/corpus_hashes.txt", golden));
  std::string const want{ reinterpret_cast<char const *>(golden.data()), golden.size() };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_hashes.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_hashes.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("layout: the corpus cost vector is committed, term by term") {
  // The gate's numbers in the open: 11.6's terms with no space requests and the
  // readable profile, so a later phase is compared against a row not a claim.
  scav_profile const p{ readable() };
  std::string actual;
  for (char const *name : { "axis.scav",
                            "bottler.scav",
                            "brew.scav",
                            "dock.scav",
                            "estop.scav",
                            "led.scav",
                            "mill.scav",
                            "ota.scav",
                            "tcp.scav",
                            "toolchanger.scav",
                            "vac.scav" }) {
    CAPTURE(name);
    std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
    path += name;
    Loader loader;
    Chart c;
    std::vector<Diagnostic> diags;
    std::string failed;
    REQUIRE(load_file(path.c_str(), loader, c, diags, failed));

    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ phase1_order(c, g, {}, p) };
    SizedLayout z;
    REQUIRE(phase2_size(c, g, o, {}, p, z, diags));
    Routes const r{ phase3_route(c, g, o, z, {}, p, *router_at(0)) };
    CostTerms const t{ cost_terms(c, g, z, r, {}, p) };
    Cost const scored{ cost_of(t, p) };

    actual += name;
    for (int64_t const term : { int64_t{ scored.t0_violations },
                                t.bends,
                                t.corridor,
                                t.crossings,
                                t.excess_len,
                                t.adjacency,
                                t.label,
                                t.label_near,
                                t.aspect,
                                t.area,
                                scored.t2 }) {
      actual += ' ';
      actual += std::to_string(term);
    }
    actual += '\n';
  }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/layout/corpus_cost.txt", golden));
  std::string const want{ reinterpret_cast<char const *>(golden.data()), golden.size() };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_cost.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_cost.txt:\n", actual);
  }
  CHECK(want == actual);
}

namespace {

// The Tier-0 predicate, rewritten rather than read from `cost_terms`: a gate
// that asks the scorer whether the scorer is happy is worth nothing.
Wide gate_orient(scav_point a, scav_point b, scav_point c) {
  return ((Wide{ b.x } - a.x) * (Wide{ c.y } - a.y)) -
         ((Wide{ b.y } - a.y) * (Wide{ c.x } - a.x));
}

bool gate_crosses(scav_point a, scav_point b, scav_point c, scav_point d) {
  Wide const d1{ gate_orient(a, b, c) };
  Wide const d2{ gate_orient(a, b, d) };
  Wide const d3{ gate_orient(c, d, a) };
  Wide const d4{ gate_orient(c, d, b) };
  if ((d1 == 0) || (d2 == 0) || (d3 == 0) || (d4 == 0)) { return false; }
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

bool gate_inside(scav_point pt, scav_rect const &r) {
  return (pt.x > r.x) && (pt.x < (r.x + r.w)) && (pt.y > r.y) && (pt.y < (r.y + r.h));
}

bool gate_enters(scav_point a, scav_point b, scav_rect const &r) {
  if (gate_inside(a, r) || gate_inside(b, r)) { return true; }
  scav_point const tl{ .x = r.x, .y = r.y };
  scav_point const tr{ .x = r.x + r.w, .y = r.y };
  scav_point const bl{ .x = r.x, .y = r.y + r.h };
  scav_point const br{ .x = r.x + r.w, .y = r.y + r.h };
  return gate_crosses(a, b, tl, tr) || gate_crosses(a, b, bl, br) ||
         gate_crosses(a, b, tl, bl) || gate_crosses(a, b, tr, br);
}

bool gate_ancestor(Chart const &c, StateId maybe, StateId of) {
  for (StateId at{ of }; at.v != INVALID;
       at = c.submachines[c.states[at.v].parent.v].owner) {
    if (at == maybe) { return true; }
  }
  return false;
}

}  // namespace

TEST_CASE("layout: no corpus chart routes an edge through a box") {
  // P7's gate, and the precondition for blind review (11.12): both incumbents sit
  // at zero, so one violation settles the comparison on the first tier.
  scav_profile const p{ readable() };
  std::string report;
  uint32_t uturns{ 0 };
  uint32_t collapsed{ 0 };
  for (char const *name : { "axis.scav",
                            "bottler.scav",
                            "brew.scav",
                            "dock.scav",
                            "estop.scav",
                            "led.scav",
                            "mill.scav",
                            "ota.scav",
                            "tcp.scav",
                            "toolchanger.scav",
                            "vac.scav" }) {
    CAPTURE(name);
    std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
    path += name;
    Loader loader;
    Chart c;
    std::vector<Diagnostic> diags;
    std::string failed;
    REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
    run(c, {}, p);

    for (uint32_t t = 0; t < c.transitions.size(); ++t) {
      scav_span const route{ row_of<scav_span>(c, "scav.geom.route", t) };
      if (route.len < 2) { continue; }
      Transition const &tr{ c.transitions[t] };
      // Nudging displaces a segment and drags the legs either end (11.5), and a
      // displacement past a leg's own length turns that leg round.
      for (uint32_t k = 0; (k + 1) < route.len; ++k) {
        scav_point const a{ row_of<scav_point>(c, "scav.geom.point", route.off + k) };
        scav_point const b{ row_of<scav_point>(c, "scav.geom.point", route.off + k + 1) };
        if ((a.x == b.x) && (a.y == b.y)) { ++collapsed; }
        if ((k + 2) >= route.len) { continue; }
        scav_point const d{ row_of<scav_point>(c, "scav.geom.point", route.off + k + 2) };
        Wide const cross{ ((Wide{ b.x } - a.x) * (Wide{ d.y } - b.y)) -
                          ((Wide{ b.y } - a.y) * (Wide{ d.x } - b.x)) };
        Wide const dot{ ((Wide{ b.x } - a.x) * (Wide{ d.x } - b.x)) +
                        ((Wide{ b.y } - a.y) * (Wide{ d.y } - b.y)) };
        if ((cross == 0) && (dot < 0)) { ++uturns; }
      }
      for (uint32_t k = 0; (k + 1) < route.len; ++k) {
        scav_point const a{ row_of<scav_point>(c, "scav.geom.point", route.off + k) };
        scav_point const b{ row_of<scav_point>(c, "scav.geom.point", route.off + k + 1) };
        for (uint32_t st = 0; st < c.states.size(); ++st) {
          if (c.states[st].live == 0) { continue; }
          // 11.14's carve-out: an edge may occupy the interior of a state it
          // is an endpoint of or a descendant of, and only that one.
          if (gate_ancestor(c, { st }, tr.src) || gate_ancestor(c, { st }, tr.dst)) {
            continue;
          }
          if (!gate_enters(a, b, row_of<scav_rect>(c, "scav.geom.state", st))) {
            continue;
          }
          report += name;
          report += " transition ";
          report += std::to_string(t);
          report += " segment ";
          report += std::to_string(k);
          report += " (";
          report += std::to_string(a.x);
          report += ",";
          report += std::to_string(a.y);
          report += ")-(";
          report += std::to_string(b.x);
          report += ",";
          report += std::to_string(b.y);
          report += ") through state ";
          report += std::to_string(st);
          report += " rect(";
          scav_rect const box{ row_of<scav_rect>(c, "scav.geom.state", st) };
          report += std::to_string(box.x);
          report += ",";
          report += std::to_string(box.y);
          report += " ";
          report += std::to_string(box.w);
          report += "x";
          report += std::to_string(box.h);
          report += ") | src=";
          report += std::to_string(tr.src.v);
          report += " parent=";
          report += std::to_string(c.states[tr.src.v].parent.v);
          report += " dst=";
          report += std::to_string(tr.dst.v);
          report += " parent=";
          report += std::to_string(c.states[tr.dst.v].parent.v);
          report += " | state parent=";
          report += std::to_string(c.states[st].parent.v);
          report += " owner=";
          report += std::to_string(c.submachines[c.states[st].parent.v].owner.v);
          report += "\n";
        }
      }
    }
  }
  if (!report.empty()) { MESSAGE("edges through boxes:\n", report); }
  CHECK(report.empty());
  CHECK(uturns == 0);
  CHECK(collapsed == 0);
}

TEST_CASE("layout: fuzzed charts and spaces either lay out or diagnose") {
  uint64_t rng{ 0x5CA7'F00D'0123'4567ULL };
  auto const next = [&rng](uint32_t bound) {
    rng = (rng * 6364136223846793005ULL) + 1442695040888963407ULL;
    return static_cast<uint32_t>((rng >> 33U) % bound);
  };

  for (uint32_t iter = 0; iter < 150; ++iter) {
    CAPTURE(iter);
    Chart c;
    SubmachineId const root{ build_chart(c, "f", {}) };
    std::vector<SubmachineId> frames{ root };
    uint32_t const n_states{ 2 + next(10) };
    for (uint32_t i = 0; i < n_states; ++i) {
      SubmachineId const parent{ frames[next(static_cast<uint32_t>(frames.size()))] };
      StateId const st{ build_state(c, parent, {}, StateKind::Normal, {}) };
      if (next(3) == 0) { frames.push_back(build_submachine(c, st, {}, {})); }
      if (next(4) == 0) { frames.push_back(build_submachine(c, st, {}, {})); }
    }
    uint32_t const total{ static_cast<uint32_t>(c.states.size()) };
    for (uint32_t i = next(12); i-- > 0;) {
      build_trans(c,
                  { next(total) },
                  { next(total) },
                  static_cast<TransKind>(next(3)),
                  {});
    }
    if (!c.transitions.empty() && (next(4) == 0)) {
      c.transitions[next(static_cast<uint32_t>(c.transitions.size()))].live = 0;
    }

    // Hostile by construction: fields wander outside the domain, subjects
    // outside the transition array, orders colliding.
    std::vector<scav_box_space> boxes(c.states.size());
    for (scav_box_space &b : boxes) {
      b = { .min_w = static_cast<int32_t>(next(200000)) - 20000,
            .h_before = static_cast<int32_t>(next(150000)) - 10000,
            .h_after = static_cast<int32_t>(next(150000)) - 10000 };
    }
    std::vector<scav_path_box> path_boxes(next(4));
    for (scav_path_box &b : path_boxes) {
      b = { .subject = next(static_cast<uint32_t>(c.transitions.size()) + 2),
            .w = static_cast<int32_t>(next(150000)) - 10000,
            .h = static_cast<int32_t>(next(150000)) - 10000,
            .order = next(3) };
    }
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()),
                         .path_box = path_boxes.data(),
                         .n_path_box = static_cast<uint32_t>(path_boxes.size()) };

    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    if (layout_run(c, s, opts(readable()), placed, diags)) {
      // A success carries marks, never rejections.
      for (Diagnostic const &d : diags) { CHECK(d.code == DiagCode::RouteDegraded); }
      CHECK(placed.size() == path_boxes.size());
      check_geometry(c);
    } else {
      CHECK(!diags.empty());
    }
  }
}
