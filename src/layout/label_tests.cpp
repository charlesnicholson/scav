// Placement against hand-written geometry, then the pruned and memoized searches against
// the exhaustive one over seeded, crowded scenes.

#include "layout/label.h"
#include "layout/tests/pod_eq.h"

#include "layout/geom.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace scav {

// The placement with its search selectable, internal to `label.cpp`.
uint32_t place_labels_by(Chart const &c,
                         SizedLayout const &z,
                         scav_spaces const &s,
                         std::vector<scav_span> const &route,
                         std::vector<scav_point> const &points,
                         scav_profile const &p,
                         LabelSearch search,
                         std::vector<scav_rect> &out);

}  // namespace scav

namespace {

// A profile scaled to these tests' geometry: `font_size_grid` 20 gives a leader of 10.
scav_profile tiny() {
  scav_profile out{};
  out.font_size_grid = 20;
  return out;
}

using namespace scav;

SizedLayout blank(Chart const &c, scav_rect chart) {
  SizedLayout z;
  z.state.assign(c.states.size(), scav_rect{});
  z.before.assign(c.states.size(), scav_rect{});
  z.after.assign(c.states.size(), scav_rect{});
  z.sub.assign(c.submachines.size(), scav_rect{});
  z.chart = chart;
  return z;
}

// One polyline per transition, in transition order, as `Routes` holds them.
struct Lines {
  std::vector<scav_point> points;
  std::vector<scav_span> route;
};

Lines lines_of(std::vector<std::vector<scav_point>> const &polys) {
  Lines out;
  for (std::vector<scav_point> const &poly : polys) {
    out.route.push_back({ .off = static_cast<uint32_t>(out.points.size()),
                          .len = static_cast<uint32_t>(poly.size()) });
    for (scav_point const &pt : poly) { out.points.push_back(pt); }
  }
  return out;
}

scav_spaces boxes_of(std::vector<scav_path_box> const &boxes) {
  return { .path_box = boxes.data(), .n_path_box = static_cast<uint32_t>(boxes.size()) };
}

// One box on one hand-written route, with stranger states carving the strips:
// every strip, slide and tie-break case below is this call with other rects.
scav_rect on_route(std::vector<scav_point> const &poly,
                   std::vector<scav_rect> const &strangers,
                   scav_rect chart,
                   scav_path_box box,
                   uint32_t &fell) {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  for (uint32_t i = 0; i < strangers.size(); ++i) {
    build_state(c, root, "X" + std::to_string(i), StateKind::Normal, {});
  }
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, chart) };
  // The endpoints keep zero rects, which no candidate can overlap; only the
  // strangers block, and they land after A and B in state order.
  for (uint32_t i = 0; i < strangers.size(); ++i) { z.state[i + 2] = strangers[i]; }
  Lines const l{ lines_of({ poly }) };
  std::vector<scav_path_box> const boxes{ box };
  std::vector<scav_rect> placed;
  fell = place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed);
  return placed[0];
}

// The leader `tiny()` gives: half an em of the 20-unit em above (11.9.4).
constexpr int32_t LEADER{ 10 };

// The gap from a placed box to the nearest leg of one polyline, and which leg
// that is. Together they are what "beside its own line" means without naming a
// coordinate: the strip grid's exact rects were an artefact of five fixed
// offsets, and the anchor has no such grid to read off.
Wide own_gap(scav_rect const &at, std::vector<scav_point> const &poly) {
  Wide nearest{ -1 };
  for (uint32_t k = 0; (k + 1) < poly.size(); ++k) {
    Wide const away{ chebyshev_gap(at, span_rect(poly[k], poly[k + 1])) };
    nearest = (nearest < 0) ? away : imin(nearest, away);
  }
  return nearest;
}

uint32_t nearest_leg(scav_rect const &at, std::vector<scav_point> const &poly) {
  uint32_t which{ INVALID };
  Wide nearest{ -1 };
  for (uint32_t k = 0; (k + 1) < poly.size(); ++k) {
    Wide const away{ chebyshev_gap(at, span_rect(poly[k], poly[k + 1])) };
    if ((nearest < 0) || (away < nearest)) {
      nearest = away;
      which = k;
    }
  }
  return which;
}

// 11.9.4's invariant: one of the box's eight points is exactly `LEADER` from a
// point on its own polyline, so the box's nearest edge is at most that far. A
// bound rather than an equality, because a corner attachment is held by a
// corner and not by the edge facing the leg.
bool anchored(scav_rect const &at, std::vector<scav_point> const &poly) {
  Wide const away{ own_gap(at, poly) };
  return (away >= 0) && (away <= Wide{ LEADER });
}

// No leg of its own route may cross it, the ridden one included: an
// axis-aligned leader off a corner can still put the box across the line it is
// anchored to, which is the slice the anchor exists to stop (11.9.4).
bool uncut(scav_rect const &at, std::vector<scav_point> const &poly) {
  for (uint32_t k = 0; (k + 1) < poly.size(); ++k) {
    if (overlaps(at, span_rect(poly[k], poly[k + 1]))) { return false; }
  }
  return true;
}

// A subject's polyline read back out of the `Lines` a test built, so a
// property can be asserted without naming the points twice.
std::vector<scav_point> poly_of(Lines const &l, uint32_t subject) {
  scav_span const r{ l.route[subject] };
  return { l.points.begin() + r.off, l.points.begin() + r.off + r.len };
}

// The two together, which is what every placement below owes.
bool placed_well(scav_rect const &at, std::vector<scav_point> const &poly) {
  return anchored(at, poly) && uncut(at, poly);
}

constexpr scav_path_box LABEL{ .subject = 0, .w = 60, .h = 20, .order = 0 };
constexpr scav_rect CHART{ .x = 0, .y = 0, .w = 600, .h = 400 };

}  // namespace

TEST_CASE("label: a box sits beside its route's longest horizontal leg") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 400 }) };
  z.state[a.v] = { .x = 0, .y = 100, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 100, .w = 100, .h = 100 };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  // Beside the leg, which is the whole of the claim: anchored to it, not cut
  // by it, and on the low side, which wins the tie with the high one. The rect
  // itself was an artefact of the strip grid's five fixed offsets (11.9.4).
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  CHECK((placed[0].y + placed[0].h) <= 150);
}

namespace {

// `L` beside a composite `R` holding `T`, one transition `L -> R/T`, and `strangers` in
// the root: the route's first legs lie in the root and its last inside `R`.
struct Crossing {
  Chart c;
  StateId l, r, t;
};

void crossing(Crossing &out, uint32_t strangers) {
  SubmachineId const root{ build_chart(out.c, "t", {}) };
  out.l = build_state(out.c, root, "L", StateKind::Normal, {});
  out.r = build_state(out.c, root, "R", StateKind::Normal, {});
  SubmachineId const inner{ build_submachine(out.c, out.r, {}, {}) };
  out.t = build_state(out.c, inner, "T", StateKind::Normal, {});
  for (uint32_t i = 0; i < strangers; ++i) {
    build_state(out.c, root, "S" + std::to_string(i), StateKind::Normal, {});
  }
  build_trans(out.c, out.l, out.t, TransKind::External, {});
}

constexpr std::array SEARCHES{ LabelSearch::Exhaustive,
                               LabelSearch::Pruned,
                               LabelSearch::Memoized };

}  // namespace

TEST_CASE("label: an out-of-machine box is anchored on the leg the root holds") {
  // The leg inside `R` is the route's longest; the anchor, taken over the root's legs
  // alone, centres the box on the root's leg.
  Crossing x;
  crossing(x, 0);
  SizedLayout z{ blank(x.c, { .x = 0, .y = -200, .w = 2000, .h = 600 }) };
  z.state[x.l.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[x.r.v] = { .x = 300, .y = -100, .w = 800, .h = 300 };
  z.state[x.t.v] = { .x = 1000, .y = 30, .w = 100, .h = 40 };
  z.sub[1] = { .x = 310, .y = -90, .w = 780, .h = 280 };
  Lines const l{ lines_of(
      { { { .x = 100, .y = 50 }, { .x = 300, .y = 50 }, { .x = 1000, .y = 50 } } }) };
  std::vector<scav_path_box> const boxes{ LABEL };
  for (LabelSearch const search : SEARCHES) {
    CAPTURE(static_cast<uint32_t>(search));
    std::vector<scav_rect> placed;
    CHECK(place_labels_by(x.c,
                          z,
                          boxes_of(boxes),
                          l.route,
                          l.points,
                          tiny(),
                          search,
                          placed) == 0);
    CHECK(placed_well(placed[0], poly_of(l, 0)));
    CHECK(nearest_leg(placed[0], poly_of(l, 0)) == 0);
    // Centred on the root's leg to within one slide, half the box's height.
    CHECK(imax(placed[0].x + (placed[0].w / 2) - 200,
               200 - placed[0].x - (placed[0].w / 2)) <= (LABEL.h / 2));
  }
}

TEST_CASE("label: the fallback keeps an out-of-machine box out of the state it enters") {
  // The root's leg runs between `R` and a stranger, so no candidate is clear and the
  // fallback tier chooses; above the leg lies `R`, below it the stranger in the root.
  Crossing x;
  crossing(x, 1);
  StateId const s{ 3 };
  SizedLayout z{ blank(x.c, { .x = 0, .y = 0, .w = 2000, .h = 1000 }) };
  z.state[x.l.v] = { .x = 0, .y = 280, .w = 100, .h = 30 };
  z.state[x.r.v] = { .x = 100, .y = 0, .w = 1100, .h = 290 };
  z.state[x.t.v] = { .x = 850, .y = 180, .w = 100, .h = 40 };
  z.sub[1] = { .x = 110, .y = 10, .w = 1080, .h = 270 };
  z.state[s.v] = { .x = 100, .y = 300, .w = 1100, .h = 300 };
  Lines const l{ lines_of({ { { .x = 100, .y = 295 },
                              { .x = 900, .y = 295 },
                              { .x = 900, .y = 290 },
                              { .x = 900, .y = 220 } } }) };
  std::vector<scav_path_box> const boxes{ LABEL };
  for (LabelSearch const search : SEARCHES) {
    CAPTURE(static_cast<uint32_t>(search));
    std::vector<scav_rect> placed;
    CHECK(place_labels_by(x.c,
                          z,
                          boxes_of(boxes),
                          l.route,
                          l.points,
                          tiny(),
                          search,
                          placed) == 0);
    CHECK(placed_well(placed[0], poly_of(l, 0)));
    CHECK_FALSE(overlaps(placed[0], z.state[x.r.v]));
    CHECK_FALSE(overlaps(placed[0], z.state[x.l.v]));
    CHECK(overlaps(placed[0], z.state[s.v]));  // the collision the fallback accepts
  }
}

TEST_CASE("label: a box slides along its leg to clear a state it is not under") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 400 }) };
  z.state[a.v] = { .x = 0, .y = 100, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 100, .w = 100, .h = 100 };
  z.state[other.v] = { .x = 200, .y = 20, .w = 100, .h = 260 };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(!overlaps(placed[0], z.state[other.v]));
  CHECK(placed_well(placed[0], poly_of(l, 0)));  // still against the leg
  // Slid along it rather than pushed off it: the box clears the stranger by
  // moving in x, so it stays within the leader in y.
  CHECK(own_gap(placed[0], poly_of(l, 0)) <= Wide{ LEADER });
}

TEST_CASE("label: the composite a transition runs in holds the box, its band does not") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 300 }) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 600, .h = 300 };
  z.before[outer.v] = { .x = 10, .y = 10, .w = 580, .h = 40 };
  z.state[a.v] = { .x = 50, .y = 100, .w = 100, .h = 60 };
  z.state[b.v] = { .x = 400, .y = 100, .w = 100, .h = 60 };
  Lines const l{ lines_of({ { { .x = 150, .y = 130 }, { .x = 400, .y = 130 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));

  // The band grew over everything within the leader of the only leg there is,
  // so no candidate is clear -- and the box keeps its anchor and takes the
  // collision rather than the centred placement, which would ride its own leg
  // (11.9.4). Nothing fell back: a placement that is anchored is a placement.
  z.before[outer.v] = { .x = 10, .y = 0, .w = 580, .h = 300 };
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  CHECK(overlaps(placed[0], z.before[outer.v]));
}

TEST_CASE("label: a box takes the side clear of another transition's route") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, a, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 400 }) };
  z.state[a.v] = { .x = 0, .y = 100, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 100, .w = 100, .h = 100 };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 400, .y = 140 }, { .x = 100, .y = 140 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  // The low side would be struck through by the other route, so the high one.
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  // The side clear of the stranger, which here is the low one.
  CHECK(placed[0].y >= 150);
}

TEST_CASE("label: another route's leg counts wherever along that route it lies") {
  // The stranger's first leg runs beside the box's and its last lies out of the box's
  // reach.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, a, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 1200, .h = 800 }) };
  z.state[a.v] = { .x = 0, .y = 100, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 100, .w = 100, .h = 100 };
  Lines const l{ lines_of(
      { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
        { { .x = 100, .y = 140 }, { .x = 1100, .y = 140 }, { .x = 1100, .y = 700 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  for (LabelSearch const search :
       { LabelSearch::Exhaustive, LabelSearch::Pruned, LabelSearch::Memoized }) {
    std::vector<scav_rect> placed;
    CHECK(place_labels_by(c,
                          z,
                          boxes_of(boxes),
                          l.route,
                          l.points,
                          tiny(),
                          search,
                          placed) == 0);
    CHECK(placed_well(placed[0], poly_of(l, 0)));
    CHECK(placed[0].y >= 150);
  }
}

TEST_CASE("label: a box crosses to the far side of its own leg to keep its distance") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, a, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 400 }) };
  z.state[a.v] = { .x = 0, .y = 100, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 100, .w = 100, .h = 100 };
  // Clear of the low side by half a line, so that side stays feasible and only
  // the distance to the stranger tells the two apart.
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 400, .y = 120 }, { .x = 100, .y = 120 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  // Both sides are equally far from the centred placement and the low one wins
  // that tie, so the far side is the shortfall's doing and nothing else.
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  // Below its own leg, which is the side away from the other transition's.
  CHECK(placed[0].y >= 150);
}

TEST_CASE("label: a transition's second box goes past its first") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 1000, .h = 300 }) };
  z.state[a.v] = { .x = 0, .y = 80, .w = 40, .h = 40 };
  z.state[b.v] = { .x = 960, .y = 80, .w = 40, .h = 40 };
  Lines const l{ lines_of({ { { .x = 40, .y = 100 }, { .x = 960, .y = 100 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 },
                                          { .subject = 0, .w = 60, .h = 20, .order = 1 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  CHECK(placed_well(placed[1], poly_of(l, 0)));
  CHECK(!overlaps(placed[0], placed[1]));
}

TEST_CASE("label: a second box goes past the first along a right-to-left leg") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 1000, .h = 300 }) };
  z.state[a.v] = { .x = 960, .y = 80, .w = 40, .h = 40 };
  z.state[b.v] = { .x = 0, .y = 80, .w = 40, .h = 40 };
  Lines const l{ lines_of({ { { .x = 960, .y = 100 }, { .x = 40, .y = 100 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 },
                                          { .subject = 0, .w = 60, .h = 20, .order = 1 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  // Further along a leg running leftwards is the smaller x, the mirror of what
  // the left-to-right case above asks for.
  CHECK(placed_well(placed[1], poly_of(l, 0)));
  // **`x` is no longer the measure of "further along".** The ordering rule is
  // on the anchor, and two anchors at different points along the leg can land
  // a box at one `x` through different attachment points -- the second here
  // moves along and attaches by a side rather than the bottom. The rect does
  // not expose the anchor, so what is checkable is that the two are distinct
  // and clear of each other (11.9.4, owed: the anchor as an output).
  CHECK_FALSE((placed[0] == placed[1]));
  CHECK(!overlaps(placed[0], placed[1]));
}

TEST_CASE("label: a second box goes past the first along a bottom-to-top leg") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 300, .h = 1000 }) };
  z.state[a.v] = { .x = 80, .y = 960, .w = 40, .h = 40 };
  z.state[b.v] = { .x = 80, .y = 0, .w = 40, .h = 40 };
  Lines const l{ lines_of({ { { .x = 100, .y = 960 }, { .x = 100, .y = 40 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 },
                                          { .subject = 0, .w = 60, .h = 20, .order = 1 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  CHECK(placed_well(placed[1], poly_of(l, 0)));
  // As above: the anchor moved along the upright leg, and `y` is not what says
  // so once the attachment point is free to differ (11.9.4).
  CHECK_FALSE((placed[0] == placed[1]));
  CHECK(!overlaps(placed[0], placed[1]));
}

TEST_CASE("label: a request with no route at all takes the centred fallback") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 400 }) };
  Lines const l{ lines_of({ {} }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 1);
  CHECK((placed[0] == scav_rect{ .x = 0, .y = 0, .w = 60, .h = 20 }));
}

TEST_CASE("label: a diagonal leg offers no strip") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 400 }) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 200, .w = 100, .h = 100 };
  Lines const l{ lines_of({ { { .x = 100, .y = 50 }, { .x = 400, .y = 250 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 1);
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 140, .w = 60, .h = 20 }));
}

TEST_CASE(
    "label: a candidate flush with the chart's edge is inside it, one unit out is not") {
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  uint32_t fell{ 0 };
  // The low side's first strip runs 130..150, so a chart starting at 130 holds it.
  CHECK((on_route(leg, {}, { .x = 0, .y = 130, .w = 600, .h = 270 }, LABEL, fell) ==
         scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
  CHECK(fell == 0);
  // One unit of chart taken away and the high side is no longer inside it, so
  // the box goes to the low one; both are anchored either way.
  scav_rect const tight{
    on_route(leg, {}, { .x = 0, .y = 131, .w = 600, .h = 269 }, LABEL, fell)
  };
  CHECK(placed_well(tight, leg));
  CHECK(tight.y >= 150);
  CHECK(fell == 0);
}

TEST_CASE(
    "label: a stranger's rect blocks a candidate it overlaps and not one it touches") {
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  uint32_t fell{ 0 };
  // Touching is free, so a rect whose bottom edge is the box's top edge leaves
  // the high side usable; one unit lower overlaps and drives the box across.
  scav_rect const flush{ .x = 200, .y = 130 - 100, .w = 100, .h = 100 };
  scav_rect const touched{ on_route(leg, { flush }, CHART, LABEL, fell) };
  CHECK(placed_well(touched, leg));
  CHECK(!overlaps(touched, flush));
  CHECK((touched.y + touched.h) <= 150);
  scav_rect const over{ .x = 200, .y = 131 - 100, .w = 100, .h = 100 };
  scav_rect const pushed{ on_route(leg, { over }, CHART, LABEL, fell) };
  CHECK(placed_well(pushed, leg));
  CHECK(!overlaps(pushed, over));
  CHECK(pushed.y >= 150);
  CHECK(fell == 0);
}

TEST_CASE(
    "label: the band an ancestor reserved after its submachine blocks the strips over "
    "it") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 300 }) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 600, .h = 300 };
  z.state[a.v] = { .x = 50, .y = 100, .w = 100, .h = 60 };
  z.state[b.v] = { .x = 400, .y = 100, .w = 100, .h = 60 };
  Lines const l{ lines_of({ { { .x = 150, .y = 130 }, { .x = 400, .y = 130 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));

  // The band an ancestor reserved is an obstacle even though the ancestor's own
  // rect is not: the box clears the band and stays anchored.
  z.after[outer.v] = { .x = 10, .y = 100, .w = 580, .h = 40 };
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  CHECK(!overlaps(placed[0], z.after[outer.v]));
}

TEST_CASE("label: a box already placed is an obstacle to the next transition's") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, CHART) };
  // Two transitions laid on one line: every candidate carries the same shortfall,
  // so only the first box's rect tells the second's candidates apart.
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 },
                                          { .subject = 1, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  CHECK(placed_well(placed[1], poly_of(l, 1)));
  CHECK(!overlaps(placed[0], placed[1]));
}

TEST_CASE("label: a candidate over a leg of its own route it does not ride is refused") {
  // The elbow's own upright leg sits under the nearest candidate of the leg the
  // box rides, and under the nearest candidate of the upright leg itself.
  std::vector<scav_point> const elbow{ { .x = 350, .y = 150 },
                                       { .x = 400, .y = 150 },
                                       { .x = 400, .y = 0 } };
  uint32_t fell{ 0 };
  // The upright leg is refused like any other obstacle, and `uncut` is the
  // whole of that claim now: no leg of its own route may cross it, the ridden
  // one included (11.9.4).
  scav_rect const at{ on_route(elbow, {}, CHART, LABEL, fell) };
  CHECK(placed_well(at, elbow));
  CHECK(fell == 0);
}

TEST_CASE("label: the leader is the only offset, and blocking it is the fallback") {
  // What replaced "five strips and no sixth": the anchor has one distance, not
  // a grid of them (11.9.4). A wall over one side leaves the other, and a wall
  // over everything within the leader leaves nothing.
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  uint32_t fell{ 0 };

  scav_rect const high{ .x = 0, .y = 150 - 100, .w = 600, .h = 100 };
  scav_rect const low{ on_route(leg, { high }, CHART, LABEL, fell) };
  CHECK(placed_well(low, leg));
  CHECK(low.y >= 150);
  CHECK(fell == 0);

  // Both sides for the leader's whole reach: no attachment point can be held at
  // that distance and stay clear, so what is left is a collision -- taken with
  // the anchor kept, because a box over an obstacle still reads as its own
  // transition's and a box on its own line does not (11.9.4).
  scav_rect const all{ .x = 0, .y = 150 - 100, .w = 600, .h = 200 };
  scav_rect const stuck{ on_route(leg, { all }, CHART, LABEL, fell) };
  CHECK(fell == 0);
  CHECK(placed_well(stuck, leg));
  CHECK(uncut(stuck, leg));
  CHECK(overlaps(stuck, all));
}

TEST_CASE("label: the anchor slides to either end of its leg") {
  // What replaced "low end, high end, and exact centre": the slide steps half a
  // box height and clamps to both ends, and the centre is no longer a slot the
  // enumeration adds -- a step that fine reaches it or near enough (11.9.4).
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  uint32_t fell{ 0 };

  // Everything but the leg's low end walled off, both sides.
  scav_rect const past_low{ .x = 130, .y = 50, .w = 470, .h = 200 };
  scav_rect const at_low{ on_route(leg, { past_low }, CHART, LABEL, fell) };
  CHECK(placed_well(at_low, leg));
  CHECK(!overlaps(at_low, past_low));
  CHECK(at_low.x < 130);
  CHECK(fell == 0);

  // And everything but its high end.
  scav_rect const before_high{ .x = 0, .y = 50, .w = 370, .h = 200 };
  scav_rect const at_high{ on_route(leg, { before_high }, CHART, LABEL, fell) };
  CHECK(placed_well(at_high, leg));
  CHECK(!overlaps(at_high, before_high));
  CHECK((at_high.x + at_high.w) > 370);
  CHECK(fell == 0);
}

TEST_CASE("label: a leg no whole number of steps long still reaches its far end") {
  // 295 long against a 10-unit step, so the last whole step falls short of the
  // end and the clamp is what reaches it.
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 395, .y = 150 } };
  uint32_t fell{ 0 };
  scav_rect const before_high{ .x = 0, .y = 50, .w = 365, .h = 200 };
  scav_rect const at_end{ on_route(leg, { before_high }, CHART, LABEL, fell) };
  CHECK(placed_well(at_end, leg));
  CHECK(!overlaps(at_end, before_high));
  CHECK((at_end.x + at_end.w) > 365);
  CHECK(fell == 0);
}

TEST_CASE("label: an earlier leg of the route outranks a later one") {
  // `seg` is the key's first component after the shortfall and the distance,
  // and it is the one component the anchor left unchanged: `side` and `strip`
  // became `attach` and `lead`, whose precedence is `better`'s lexicographic
  // construction rather than a shape anybody can draw (11.9.4).
  std::vector<scav_point> const elbow{ { .x = 100, .y = 150 },
                                       { .x = 400, .y = 150 },
                                       { .x = 400, .y = 350 } };
  uint32_t fell{ 0 };
  scav_rect const at{ on_route(elbow, {}, CHART, LABEL, fell) };
  CHECK(placed_well(at, elbow));
  CHECK(fell == 0);
  CHECK(nearest_leg(at, elbow) == 0);
}

TEST_CASE("label: the anchor distance outranks the leg a candidate rides") {
  // The anchor is the horizontal leg's midpoint, so the second leg holds every
  // near candidate and the upright first leg holds none.
  std::vector<scav_point> const bend{ { .x = 100, .y = 300 },
                                      { .x = 100, .y = 150 },
                                      { .x = 400, .y = 150 } };
  uint32_t fell{ 0 };
  scav_rect const at{ on_route(bend, {}, CHART, LABEL, fell) };
  CHECK(placed_well(at, bend));
  CHECK(fell == 0);
  // The horizontal leg, which is the one the anchor is nearest, not the
  // upright leg that comes first in the route.
  CHECK(nearest_leg(at, bend) == 1);
}

// `side` and `strip` are gone with the strip grid, and with them the three
// cases that drew a shape for each component's precedence. What survives is
// stated where it is observable: the leg's precedence has its own case above,
// and `attach` and `lead` are ordered by `better`'s lexicographic construction
// in label.cpp, which no geometry distinguishes from any other total order
// (11.9.4).

TEST_CASE("label: two runs over one input place the box identically") {
  // What "candidates alike go to the lower one" is about, stated where it is
  // observable: the key is a total order over integers, so a tie has exactly
  // one winner and the same input cannot come out twice (6). A wall taking the
  // centre slide alone leaves the two either side of it tied on everything but
  // the slide, which is the case that would wobble if the order were partial.
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  scav_rect const wall{ .x = 235, .y = 90, .w = 20, .h = 120 };
  scav_path_box const tall{ .subject = 0, .w = 20, .h = 60, .order = 0 };
  uint32_t fell{ 0 };
  uint32_t again{ 0 };
  scav_rect const first{ on_route(leg, { wall }, CHART, tall, fell) };
  scav_rect const second{ on_route(leg, { wall }, CHART, tall, again) };
  CHECK((first == second));
  CHECK(fell == again);
  CHECK(fell == 0);
  CHECK(placed_well(first, leg));
  CHECK(!overlaps(first, wall));
}

TEST_CASE("label: the shortfall is measured across the axis the leg does not run on") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const p{ build_state(c, root, "P", StateKind::Normal, {}) };
  StateId const q{ build_state(c, root, "Q", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, p, q, TransKind::External, {});

  SizedLayout z{ blank(c, { .x = 0, .y = 0, .w = 600, .h = 500 }) };
  // Two upright legs five units apart on x: the near side clears the stranger by
  // less than one line of its own text, so the box goes round to the far side.
  Lines const l{ lines_of({ { { .x = 250, .y = 100 }, { .x = 250, .y = 400 } },
                            { { .x = 185, .y = 100 }, { .x = 185, .y = 400 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
}

TEST_CASE("label: a candidate past the first strip is scored against the stranger too") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const p{ build_state(c, root, "P", StateKind::Normal, {}) };
  StateId const q{ build_state(c, root, "Q", StateKind::Normal, {}) };
  StateId const wall{ build_state(c, root, "W", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, p, q, TransKind::External, {});

  SizedLayout z{ blank(c, CHART) };
  // The high side within the leader and nothing else: with one distance rather
  // than five offsets, a wall over both sides leaves no candidate at all.
  z.state[wall.v] = { .x = 0, .y = 130, .w = 600, .h = 20 };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 100, .y = 95 }, { .x = 400, .y = 95 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  // The high side is walled off, so the box goes below -- and it is scored
  // against the stranger there, which is what the per-leg prefilter has to
  // preserve now that there are no per-strip bands (11.9.4).
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  CHECK(placed[0].y >= 150);
  CHECK(!overlaps(placed[0], z.state[wall.v]));
}

TEST_CASE(
    "label: a foreign segment blocks the candidates it crosses, not only the near ones") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout const z{ blank(c, CHART) };
  // A stranger laid along the leg gives every candidate the same shortfall, so
  // the crossing stranger decides by blocking rather than by distance.
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 250, .y = 120 }, { .x = 250, .y = 180 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
}

TEST_CASE("label: the shortfall outranks the anchor distance") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const p{ build_state(c, root, "P", StateKind::Normal, {}) };
  StateId const q{ build_state(c, root, "Q", StateKind::Normal, {}) };
  StateId const nick{ build_state(c, root, "N", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, p, q, TransKind::External, {});

  SizedLayout z{ blank(c, CHART) };
  // The nick takes the high side's nearest slide, so the shortfall's own side is
  // the further one and only the key order decides between them.
  z.state[nick.v] = { .x = 272, .y = 150, .w = 4, .h = 20 };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 100, .y = 125 }, { .x = 400, .y = 125 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
}

TEST_CASE("label: a box clear of everything stays where the distance put it") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const p{ build_state(c, root, "P", StateKind::Normal, {}) };
  StateId const q{ build_state(c, root, "Q", StateKind::Normal, {}) };
  StateId const far{ build_state(c, root, "F", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, p, q, TransKind::External, {});

  SizedLayout z{ blank(c, CHART) };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 100, .y = 350 }, { .x = 400, .y = 350 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  scav_rect const alone{ .x = 220, .y = 130, .w = 60, .h = 20 };
  CHECK((placed[0] == alone));

  // Another route and another box's worth of state, both out of reach: neither
  // the shortfall nor the sweep has anything to say, so nothing moves.
  z.state[far.v] = { .x = 0, .y = 300, .w = 600, .h = 100 };
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK((placed[0] == alone));
}

TEST_CASE("label: a subject past the route table takes the centred fallback") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, CHART) };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 7, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 1);
  // No route and no transition to read endpoints off: the anchor is the origin
  // and the chart rect slides the box back inside.
  CHECK((placed[0] == scav_rect{ .x = 0, .y = 0, .w = 60, .h = 20 }));
}

TEST_CASE("label: a subject with a route but no transition rides the route anyway") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout const z{ blank(c, CHART) };
  // A route table longer than the transition table: there is no pair of
  // endpoints to excuse a state with, and the strips are matched all the same.
  Lines const l{ lines_of({ { { .x = 100, .y = 350 }, { .x = 400, .y = 350 } },
                            { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 1, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 1)));  // the box's own subject is 1
}

TEST_CASE("label: a second box may ride a leg after the one the first took") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout const z{ blank(c, CHART) };
  Lines const l{ lines_of(
      { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 }, { .x = 400, .y = 350 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 },
                                          { .subject = 0, .w = 60, .h = 20, .order = 1 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
  // The upright leg is enumerated for the second box too, and loses on distance
  // rather than on being out of bounds.
  CHECK(placed_well(placed[1], poly_of(l, 0)));
  CHECK(nearest_leg(placed[1], poly_of(l, 0)) >= nearest_leg(placed[0], poly_of(l, 0)));
}

TEST_CASE("label: a route of one point takes the centred fallback on that point") {
  std::vector<scav_point> const dot{ { .x = 250, .y = 150 } };
  uint32_t fell{ 0 };
  CHECK((on_route(dot, {}, CHART, LABEL, fell) ==
         scav_rect{ .x = 220, .y = 140, .w = 60, .h = 20 }));
  CHECK(fell == 1);
}

TEST_CASE("label: a request of no boxes at all places nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout const z{ blank(c, CHART) };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };
  // A live pointer with a count of zero, which is what an empty table looks like
  // when the caller keeps its storage.
  scav_spaces const none{ .path_box = boxes.data(), .n_path_box = 0 };

  std::vector<scav_rect> placed{ scav_rect{ .x = 1, .y = 2, .w = 3, .h = 4 } };
  CHECK(place_labels(c, z, none, l.route, l.points, tiny(), placed) == 0);
  CHECK(placed.empty());
}

TEST_CASE("label: a tombstoned state is not an obstacle") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const gone{ build_state(c, root, "G", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, CHART) };
  // The high side within the leader: live it drives the box below, tombstoned
  // it is not there and the box takes the side it prefers.
  z.state[gone.v] = { .x = 0, .y = 130, .w = 600, .h = 20 };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  scav_rect const with_tomb{ placed[0] };

  c.states[gone.v].live = 0;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  // Live, the same rect is refused; tombstoned it was never an obstacle.
  CHECK_FALSE((placed[0] == with_tomb));
}

TEST_CASE("label: the placement does not depend on the path box row order") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const p{ build_state(c, root, "P", StateKind::Normal, {}) };
  StateId const q{ build_state(c, root, "Q", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, p, q, TransKind::External, {});

  SizedLayout const z{ blank(c, CHART) };
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 100, .y = 250 }, { .x = 400, .y = 250 } } }) };
  std::vector<scav_path_box> const forward{
    { .subject = 0, .w = 60, .h = 20, .order = 0 },
    { .subject = 1, .w = 60, .h = 20, .order = 0 }
  };
  std::vector<scav_path_box> const backward{
    { .subject = 1, .w = 60, .h = 20, .order = 0 },
    { .subject = 0, .w = 60, .h = 20, .order = 0 }
  };

  std::vector<scav_rect> first;
  std::vector<scav_rect> second;
  CHECK(place_labels(c, z, boxes_of(forward), l.route, l.points, tiny(), first) == 0);
  CHECK(place_labels(c, z, boxes_of(backward), l.route, l.points, tiny(), second) == 0);
  CHECK((first[0] == second[1]));
  CHECK((first[1] == second[0]));
}

TEST_CASE("label: a transition's second box never goes back to an earlier leg") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  // No walls: the rule under test is the chaining itself, and the anchor gives
  // every leg candidates, so the old construction's job -- starving the first
  // leg so the first box had to take the second -- is not what says the rule
  // holds. What says it is that the second box's leg is never the earlier one
  // (11.9.4).
  SizedLayout z{ blank(c, CHART) };
  Lines const l{ lines_of(
      { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 }, { .x = 400, .y = 300 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 },
                                          { .subject = 0, .w = 60, .h = 20, .order = 1 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed) == 0);
  CHECK(placed_well(placed[0], poly_of(l, 0)));
  CHECK(placed_well(placed[1], poly_of(l, 0)));
  CHECK(nearest_leg(placed[1], poly_of(l, 0)) >= nearest_leg(placed[0], poly_of(l, 0)));
  CHECK(!overlaps(placed[0], placed[1]));
}

TEST_CASE("label: two thousand boxes place, and quickly") {
  // A flat grid, which is the worst shape for the obstacle sweep: every state
  // is a stranger to every route, so none of them is carved out.
  constexpr uint32_t COLS{ 50 };
  constexpr uint32_t ROWS{ 40 };
  constexpr uint32_t CELLS{ COLS * ROWS };
  Chart c;
  SubmachineId const root{ build_chart(c, "grid", {}) };
  std::vector<StateId> all;
  all.reserve(CELLS);
  SizedLayout z;
  std::vector<std::vector<scav_point>> polys;
  std::vector<scav_path_box> boxes;
  for (uint32_t i = 0; i < CELLS; ++i) {
    all.push_back(build_state(c, root, "S" + std::to_string(i), StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < all.size(); ++i) {
    build_trans(c, all[i - 1], all[i], TransKind::External, {});
    boxes.push_back({ .subject = i - 1, .w = 60, .h = 20, .order = 0 });
  }
  REQUIRE(c.states.size() == CELLS);

  z = blank(c,
            { .x = 0,
              .y = 0,
              .w = static_cast<int32_t>(400 * COLS),
              .h = static_cast<int32_t>(300 * ROWS) });
  for (uint32_t i = 0; i < all.size(); ++i) {
    z.state[all[i].v] = { .x = static_cast<int32_t>(400 * (i % COLS)),
                          .y = static_cast<int32_t>(300 * (i / COLS)),
                          .w = 200,
                          .h = 100 };
  }
  // An elbow out of one box's right edge into the next box's left edge, which
  // is one horizontal leg, one vertical, one horizontal.
  for (uint32_t i = 1; i < all.size(); ++i) {
    scav_rect const from{ z.state[all[i - 1].v] };
    scav_rect const to{ z.state[all[i].v] };
    int32_t const mx{ (from.x + from.w) + 60 };
    polys.push_back({ { .x = from.x + from.w, .y = from.y + 50 },
                      { .x = mx, .y = from.y + 50 },
                      { .x = mx, .y = to.y + 50 },
                      { .x = to.x, .y = to.y + 50 } });
  }
  Lines const l{ lines_of(polys) };

  std::vector<scav_rect> placed;
  auto const t0{ std::chrono::steady_clock::now() };
  uint32_t const fell{
    place_labels(c, z, boxes_of(boxes), l.route, l.points, tiny(), placed)
  };
  auto const t1{ std::chrono::steady_clock::now() };
  auto const us{ std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() };
  MESSAGE("place_labels over ", boxes.size(), " boxes: ", us, " us, ", fell, " fell back");
  CHECK(placed.size() == boxes.size());
#if SCAV_PERF_ASSERT_FLOOR == 1
  // A floor, not a time: the sweep is linear in obstacles per box and this is
  // what catches it becoming linear in candidates too.
  CHECK(us < 200000);
#endif
}

namespace {

// A deterministic LCG, as in geom_tests.
uint32_t next(uint64_t &state) {
  state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
  return static_cast<uint32_t>(state >> 33U);
}

// A seeded hand-laid scene: banded composites of one or two regions, plain states, mixed
// routes and up to three boxes a transition. `crowd` 2 refuses most candidates.
struct Scene {
  Chart c;
  SizedLayout z;
  Lines l;
  std::vector<scav_path_box> boxes;
  scav_profile p{};
};

Scene scene_of(uint64_t seed, uint32_t crowd) {
  Scene out;
  uint64_t state{ seed };
  auto const pick = [&state](uint32_t n) { return next(state) % n; };
  auto const upto = [&pick](int32_t n) {
    return static_cast<int32_t>(pick(static_cast<uint32_t>(n)));
  };
  int32_t const cw{ 6000 - (2000 * static_cast<int32_t>(crowd)) };
  int32_t const ch{ 4500 - (1500 * static_cast<int32_t>(crowd)) };
  out.p.font_size_grid = 8 + upto(200);

  Chart &c{ out.c };
  SubmachineId const root{ build_chart(c, "scene", {}) };
  std::vector<StateId> all;
  std::vector<StateId> composites;
  uint32_t const n_comp{ pick(4) };
  for (uint32_t i = 0; i < n_comp; ++i) {
    StateId const comp{
      build_state(c, root, "C" + std::to_string(i), StateKind::Normal, {})
    };
    composites.push_back(comp);
    all.push_back(comp);
    uint32_t const regions{ 1 + pick(2) };
    for (uint32_t m = 0; m < regions; ++m) {
      SubmachineId const sub{ build_submachine(c, comp, "r" + std::to_string(m), {}) };
      uint32_t const kids{ 1 + pick(4) };
      for (uint32_t k = 0; k < kids; ++k) {
        all.push_back(build_state(
            c,
            sub,
            "K" + std::to_string(i) + "_" + std::to_string(m) + "_" + std::to_string(k),
            StateKind::Normal,
            {}));
      }
    }
  }
  uint32_t const n_plain{ 2 + pick(7) };
  for (uint32_t i = 0; i < n_plain; ++i) {
    all.push_back(build_state(c, root, "P" + std::to_string(i), StateKind::Normal, {}));
  }
  uint32_t const n_trans{ 3 + pick(6 + (8 * crowd)) };
  for (uint32_t t = 0; t < n_trans; ++t) {
    build_trans(c,
                all[pick(static_cast<uint32_t>(all.size()))],
                all[pick(static_cast<uint32_t>(all.size()))],
                TransKind::External,
                {});
  }

  out.z = blank(c, { .x = 0, .y = 0, .w = cw, .h = ch });
  SizedLayout &z{ out.z };
  auto const rect_in = [&](scav_rect const &in, int32_t max_w, int32_t max_h) {
    int32_t const w{ imin(in.w, 40 + upto(max_w)) };
    int32_t const h{ imin(in.h, 30 + upto(max_h)) };
    return scav_rect{ .x = in.x + upto(imax(in.w - w, 0) + 1),
                      .y = in.y + upto(imax(in.h - h, 0) + 1),
                      .w = w,
                      .h = h };
  };
  for (StateId const comp : composites) {
    scav_rect const at{ rect_in(z.chart, 2500, 2000) };
    z.state[comp.v] = at;
    int32_t const band{ imin(at.h / 4, 20 + upto(200)) };
    z.before[comp.v] = { .x = at.x + 10, .y = at.y + 10, .w = at.w - 20, .h = band };
    z.after[comp.v] = { .x = at.x + 10,
                        .y = (at.y + at.h) - 10 - band,
                        .w = at.w - 20,
                        .h = band };
    Span const subs{ c.states[comp.v].submachines };
    int32_t const inner_y{ at.y + 20 + band };
    int32_t const inner_h{ imax(at.h - (2 * (band + 20)), 1) };
    int32_t const each{ at.w / static_cast<int32_t>(subs.len) };
    for (uint32_t m = 0; m < subs.len; ++m) {
      SubmachineId const sub{ c.submachine_ids[subs.off + m] };
      z.sub[sub.v] = { .x = at.x + (static_cast<int32_t>(m) * each),
                       .y = inner_y,
                       .w = each,
                       .h = inner_h };
    }
  }
  for (StateId const s : all) {
    SubmachineId const parent{ c.states[s.v].parent };
    if (parent.v == root.v) {
      if (c.states[s.v].submachines.len == 0) {
        z.state[s.v] = rect_in(z.chart, 500, 300);
      }
    } else {
      z.state[s.v] = rect_in(z.sub[parent.v], 500, 300);
    }
  }
  // A tombstoned state keeps its rect.
  if (pick(5) == 0) { c.states[all[pick(static_cast<uint32_t>(all.size()))].v].live = 0; }

  std::vector<std::vector<scav_point>> polys;
  for (uint32_t t = 0; t < n_trans; ++t) {
    std::vector<scav_point> poly;
    uint32_t const shape{ pick(20) };
    scav_rect const from{ z.state[c.transitions[t].src.v] };
    scav_point at{ .x = from.x + upto(imax(from.w, 1)),
                   .y = from.y + upto(imax(from.h, 1)) };
    if (shape != 0) { poly.push_back(at); }
    uint32_t const legs{ (shape <= 1) ? 0U : 1 + pick(5) };
    bool across{ pick(2) == 0 };
    for (uint32_t k = 0; k < legs; ++k) {
      int32_t const run{ (upto(2) == 0 ? -1 : 1) *
                         (40 + upto(1400 - (400 * static_cast<int32_t>(crowd)))) };
      uint32_t const kind{ pick(12) };
      if (kind == 0) {
        at = { .x = at.x + run, .y = at.y + (run / 2) };  // diagonal
      } else if (kind != 1) {  // 1 is degenerate: the point repeats
        at = across ? scav_point{ .x = at.x + run, .y = at.y }
                    : scav_point{ .x = at.x, .y = at.y + run };
      }
      across = !across;
      poly.push_back(at);
    }
    polys.push_back(poly);
  }
  // A route past the transition table, which a box may still ride.
  if (pick(4) == 0) {
    polys.push_back({ { .x = 100, .y = ch / 2 }, { .x = cw - 100, .y = ch / 2 } });
  }
  out.l = lines_of(polys);

  for (uint32_t t = 0; t < polys.size(); ++t) {
    uint32_t const count{ pick(4) };
    for (uint32_t j = 0; j < count; ++j) {
      out.boxes.push_back({ .subject = t,
                            .w = (pick(10) == 0) ? 0 : (30 + upto(900)),
                            .h = (pick(12) == 0) ? 0 : (10 + upto(300)),
                            .order = j });
    }
  }
  // And a subject with no route at all.
  if (pick(6) == 0) {
    out.boxes.push_back(
        { .subject = static_cast<uint32_t>(polys.size()), .w = 60, .h = 20, .order = 0 });
  }
  // Rows out of placement order.
  for (uint32_t i = 1; i < out.boxes.size(); ++i) {
    uint32_t const j{ pick(i + 1) };
    scav_path_box const swap{ out.boxes[i] };
    out.boxes[i] = out.boxes[j];
    out.boxes[j] = swap;
  }
  return out;
}

// Every coordinate of a scene moved by one offset.
Scene translated(Scene const &sc, int32_t dx, int32_t dy) {
  Scene out{ sc };
  auto const move = [dx, dy](scav_rect &r) {
    r.x += dx;
    r.y += dy;
  };
  for (scav_rect &r : out.z.state) { move(r); }
  for (scav_rect &r : out.z.before) { move(r); }
  for (scav_rect &r : out.z.after) { move(r); }
  for (scav_rect &r : out.z.sub) { move(r); }
  move(out.z.chart);
  for (scav_point &pt : out.l.points) {
    pt.x += dx;
    pt.y += dy;
  }
  return out;
}

struct Placed {
  std::vector<scav_rect> at;
  uint32_t fell{ 0 };
};

Placed placed_by(Scene const &sc, LabelSearch search) {
  Placed out;
  out.fell = place_labels_by(sc.c,
                             sc.z,
                             boxes_of(sc.boxes),
                             sc.l.route,
                             sc.l.points,
                             sc.p,
                             search,
                             out.at);
  return out;
}

bool same(Placed const &a, Placed const &b) {
  return (a.fell == b.fell) && same_rows(a.at, b.at);
}

// Whether a placed box overlaps a leg of another transition's route.
bool over_foreign(Scene const &sc, Placed const &got) {
  for (uint32_t i = 0; i < sc.boxes.size(); ++i) {
    for (uint32_t t = 0; t < sc.l.route.size(); ++t) {
      if (t == sc.boxes[i].subject) { continue; }
      scav_span const r{ sc.l.route[t] };
      for (uint32_t k = 0; (k + 1) < r.len; ++k) {
        if (overlaps(got.at[i],
                     span_rect(sc.l.points[r.off + k], sc.l.points[r.off + k + 1]))) {
          return true;
        }
      }
    }
  }
  return false;
}

constexpr uint32_t SEEDS{ 160 };
constexpr uint32_t CROWDS{ 3 };

}  // namespace

TEST_CASE(
    "label: the pruned and memoized searches place every box as the exhaustive one") {
  // The memoized search runs twice, so the second answers from the table the first filled.
  uint32_t fell{ 0 };
  uint32_t refused{ 0 };
  uint32_t boxes{ 0 };
  for (uint32_t crowd = 0; crowd < CROWDS; ++crowd) {
    for (uint32_t seed = 1; seed <= SEEDS; ++seed) {
      CAPTURE(crowd);
      CAPTURE(seed);
      Scene const sc{ scene_of((uint64_t{ crowd } << 32U) | seed, crowd) };
      Placed const want{ placed_by(sc, LabelSearch::Exhaustive) };
      CHECK(same(placed_by(sc, LabelSearch::Pruned), want));
      CHECK(same(placed_by(sc, LabelSearch::Memoized), want));
      CHECK(same(placed_by(sc, LabelSearch::Memoized), want));
      fell += want.fell;
      refused += over_foreign(sc, want) ? 1U : 0U;
      boxes += static_cast<uint32_t>(sc.boxes.size());
    }
  }
  MESSAGE(boxes,
          " boxes, ",
          fell,
          " fell back, ",
          refused,
          " scenes with a box over a foreign leg");
  // The scenes reach both the centred fallback and the colliding tier.
  CHECK(fell > 0);
  CHECK(refused > 0);
}

TEST_CASE("label: the searches agree on boxes of no width and of no height") {
  // A box of no extent can sit on the edge of the region the pruned search clips to.
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  scav_rect const wall{ .x = 180, .y = 40, .w = 80, .h = 220 };
  for (scav_path_box const box :
       { scav_path_box{ .subject = 0, .w = 0, .h = 20, .order = 0 },
         scav_path_box{ .subject = 0, .w = 60, .h = 0, .order = 0 },
         scav_path_box{ .subject = 0, .w = 0, .h = 0, .order = 0 } }) {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
    StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
    build_trans(c, a, b, TransKind::External, {});
    SizedLayout z{ blank(c, CHART) };
    z.state[x.v] = wall;
    Lines const l{ lines_of({ leg }) };
    std::vector<scav_path_box> const boxes{ box };
    std::vector<scav_rect> want;
    std::vector<scav_rect> got;
    uint32_t const fell{ place_labels_by(c,
                                         z,
                                         boxes_of(boxes),
                                         l.route,
                                         l.points,
                                         tiny(),
                                         LabelSearch::Exhaustive,
                                         want) };
    for (LabelSearch const search : { LabelSearch::Pruned, LabelSearch::Memoized }) {
      CHECK(
          place_labels_by(c, z, boxes_of(boxes), l.route, l.points, tiny(), search, got) ==
          fell);
      CHECK(same_rows(got, want));
    }
  }
}

TEST_CASE(
    "label: the searches agree where the step divides the leg and where it does not") {
  // The last slide is clamped to the leg's far end, which a leg a whole number of steps
  // long visits twice; a leg shorter than a step has only its two ends.
  for (int32_t const length : { 300, 295, 7, 0 }) {
    for (int32_t const offset : { -35, 0, 35 }) {
      CAPTURE(length);
      CAPTURE(offset);
      std::vector<scav_point> const leg{ { .x = 100, .y = 150 },
                                         { .x = 100 + length, .y = 150 } };
      std::vector<scav_rect> const strangers{
        { .x = 150 + offset, .y = 100, .w = 60, .h = 100 }
      };
      Chart c;
      SubmachineId const root{ build_chart(c, "t", {}) };
      StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
      StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
      StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
      build_trans(c, a, b, TransKind::External, {});
      SizedLayout z{ blank(c, CHART) };
      z.state[x.v] = strangers[0];
      Lines const l{ lines_of({ leg }) };
      std::vector<scav_path_box> const boxes{
        LABEL,
        { .subject = 0, .w = 60, .h = 20, .order = 1 }
      };
      std::vector<scav_rect> want;
      std::vector<scav_rect> got;
      uint32_t const fell{ place_labels_by(c,
                                           z,
                                           boxes_of(boxes),
                                           l.route,
                                           l.points,
                                           tiny(),
                                           LabelSearch::Exhaustive,
                                           want) };
      CHECK(place_labels_by(c,
                            z,
                            boxes_of(boxes),
                            l.route,
                            l.points,
                            tiny(),
                            LabelSearch::Pruned,
                            got) == fell);
      CHECK(same_rows(got, want));
    }
  }
}

TEST_CASE("label memo: a remembered box is the box its inputs place") {
  // Each variant changes one input the key must cover, after the memo placed the base; the
  // last two change nothing a box sees except by moving with it.
  constexpr uint32_t VARIANTS{ 11 };
  std::array<uint32_t, VARIANTS> moved{};
  for (uint32_t crowd = 0; crowd < CROWDS; ++crowd) {
    for (uint32_t seed = 1; seed <= (SEEDS / 2); ++seed) {
      CAPTURE(crowd);
      CAPTURE(seed);
      Scene const base{ scene_of((uint64_t{ crowd + 7 } << 32U) | seed, crowd) };
      Placed const before{ placed_by(base, LabelSearch::Pruned) };
      std::array<Scene, VARIANTS> variants{};
      variants.fill(base);
      for (scav_path_box &b : variants[0].boxes) { b.w += 37; }
      for (scav_path_box &b : variants[1].boxes) { b.h += 23; }
      variants[2].p.font_size_grid += 30;
      // Each route's second point moved: its shape changes and its origin stays.
      for (scav_span const r : variants[3].l.route) {
        if (r.len >= 2) { variants[3].l.points[r.off + 1].y += 25; }
      }
      // The strangers moved: every state that holds no other.
      for (uint32_t st = 0; st < base.c.states.size(); ++st) {
        if (base.c.states[st].submachines.len == 0) {
          variants[4].z.state[st].x += 31;
          variants[4].z.state[st].y -= 17;
        }
      }
      // The bands of every composite.
      for (scav_rect &band : variants[5].z.before) {
        band.y += 19;
        band.h += 7;
      }
      // What holds a box: the composites and the chart, narrowed.
      for (uint32_t st = 0; st < base.c.states.size(); ++st) {
        if (base.c.states[st].submachines.len != 0) {
          variants[6].z.state[st] = grow(base.z.state[st], -15);
        }
      }
      variants[6].z.chart = grow(base.z.chart, -60);
      // Other transitions' legs only: the routes no box rides.
      std::vector<uint8_t> ridden(base.l.route.size(), 0);
      for (scav_path_box const &b : base.boxes) {
        if (b.subject < ridden.size()) { ridden[b.subject] = 1; }
      }
      for (uint32_t t = 0; t < base.l.route.size(); ++t) {
        if (ridden[t] != 0) { continue; }
        scav_span const r{ base.l.route[t] };
        for (uint32_t k = 0; k < r.len; ++k) {
          variants[7].l.points[r.off + k].x += 13;
          variants[7].l.points[r.off + k].y += 29;
        }
      }
      // A transition's first box resized, which moves the walls and slide its later boxes
      // see.
      for (scav_path_box &b : variants[8].boxes) {
        if (b.order == 0) { b.w += 51; }
      }
      variants[9] = translated(base, 1234, -567);
      // A composite reaching much further than any box can see.
      for (uint32_t st = 0; st < base.c.states.size(); ++st) {
        if (base.c.states[st].submachines.len != 0) { variants[10].z.state[st].w += 3000; }
      }

      (void)placed_by(base, LabelSearch::Memoized);
      for (uint32_t k = 0; k < VARIANTS; ++k) {
        CAPTURE(k);
        Placed const want{ placed_by(variants[k], LabelSearch::Pruned) };
        CHECK(same(placed_by(variants[k], LabelSearch::Memoized), want));
        if (!same(want, before)) { ++moved[k]; }
      }
      CHECK(same(placed_by(base, LabelSearch::Memoized), before));
      CHECK(same(before, placed_by(base, LabelSearch::Exhaustive)));
    }
  }
  for (uint32_t k = 0; k < (VARIANTS - 2); ++k) {
    CAPTURE(k);
    CHECK(moved[k] > 0);
  }
}

namespace {

// `sc` with up to two routes translated or extended by a leg.
Scene with_routes_moved(Scene const &sc, uint64_t seed) {
  uint64_t state{ seed };
  auto const pick = [&state](uint32_t n) { return next(state) % n; };
  std::vector<std::vector<scav_point>> polys;
  polys.reserve(sc.l.route.size());
  for (scav_span const &r : sc.l.route) {
    polys.emplace_back(sc.l.points.begin() + r.off, sc.l.points.begin() + r.off + r.len);
  }
  uint32_t const moves{ pick(3) };
  for (uint32_t m = 0; (m < moves) && !polys.empty(); ++m) {
    std::vector<scav_point> &poly{ polys[pick(static_cast<uint32_t>(polys.size()))] };
    if (poly.empty()) { continue; }
    if (pick(2) == 0) {
      auto const dx{ static_cast<int32_t>(pick(600)) - 300 };
      auto const dy{ static_cast<int32_t>(pick(600)) - 300 };
      for (scav_point &pt : poly) {
        pt.x += dx;
        pt.y += dy;
      }
    } else {
      scav_point const last{ poly.back() };
      poly.push_back({ .x = last.x, .y = last.y + 200 + static_cast<int32_t>(pick(400)) });
    }
  }
  Scene out{ sc };
  out.l = lines_of(polys);
  return out;
}

struct Settled {
  std::vector<scav_rect> at;
  std::vector<LabelSettle> how;
  uint32_t fell{ 0 };
};

Settled settled_over(Scene const &sc, LabelBase const *was) {
  Settled out;
  out.fell = place_labels(sc.c,
                          sc.z,
                          boxes_of(sc.boxes),
                          sc.l.route,
                          sc.l.points,
                          sc.p,
                          out.at,
                          out.how,
                          was);
  return out;
}

}  // namespace

TEST_CASE("label: a placement kept from a base is the placement made afresh") {
  // Both kinds of box occur: ones that stay where they were and ones a moved route sends
  // elsewhere.
  uint32_t stayed{ 0 };
  uint32_t went{ 0 };
  for (uint32_t crowd = 0; crowd < CROWDS; ++crowd) {
    for (uint32_t seed = 1; seed <= SEEDS; ++seed) {
      CAPTURE(crowd);
      CAPTURE(seed);
      Scene const sc{ scene_of((uint64_t{ crowd } << 32U) | seed, crowd) };
      Settled const before{ settled_over(sc, nullptr) };
      LabelBase const base{ .route = &sc.l.route,
                            .points = &sc.l.points,
                            .placed = &before.at,
                            .settled = &before.how };
      Scene const after{ with_routes_moved(sc, (uint64_t{ seed } << 8U) | crowd) };
      Settled const fresh{ settled_over(after, nullptr) };
      Settled const kept{ settled_over(after, &base) };
      CHECK(kept.fell == fresh.fell);
      CHECK(same_rows(kept.at, fresh.at));
      REQUIRE(kept.how.size() == fresh.how.size());
      bool same_how{ true };
      for (uint32_t i = 0; i < fresh.how.size(); ++i) {
        same_how = same_how && (kept.how[i].found == fresh.how[i].found) &&
                   (kept.how[i].seg == fresh.how[i].seg) &&
                   (kept.how[i].mid == fresh.how[i].mid);
      }
      CHECK(same_how);
      for (uint32_t i = 0; i < fresh.at.size(); ++i) {
        bool const moved{ (fresh.at[i].x != before.at[i].x) ||
                          (fresh.at[i].y != before.at[i].y) };
        ++(moved ? went : stayed);
      }
    }
  }
  CHECK(stayed > 0);
  CHECK(went > 0);
}
