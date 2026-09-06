// Placement against hand-written geometry: a few rects and one polyline are
// enough, since the candidates come from the route and nothing else.

#include "layout/label.h"

#include "layout/geom.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace scav;

constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

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
  fell = place_labels(c, z, boxes_of(boxes), l.route, l.points, placed);
  return placed[0];
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  // Above the leg, centred on it: the low side wins the tie with the high one.
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK(!overlaps(placed[0], z.state[other.v]));
  CHECK(((placed[0].y == 130) || (placed[0].y == 150)));  // still against the leg
  CHECK((placed[0] == scav_rect{ .x = 130, .y = 130, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 245, .y = 110, .w = 60, .h = 20 }));

  // The band grew over every strip the only leg there is has, so the box takes
  // the centred placement.
  z.before[outer.v] = { .x = 10, .y = 0, .w = 580, .h = 300 };
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 1);
  CHECK((placed[0] == scav_rect{ .x = 245, .y = 120, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  // The low side would be struck through by the other route, so the high one.
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 150, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  // Both sides are equally far from the centred placement and the low one wins
  // that tie, so the far side is the shortfall's doing and nothing else.
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 150, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 470, .y = 80, .w = 60, .h = 20 }));
  CHECK((placed[1] == scav_rect{ .x = 490, .y = 100, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 470, .y = 80, .w = 60, .h = 20 }));
  // Further along a leg running leftwards is the smaller x, the mirror of what
  // the left-to-right case above asks for.
  CHECK((placed[1] == scav_rect{ .x = 450, .y = 100, .w = 60, .h = 20 }));
  CHECK(placed[1].x < placed[0].x);
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 40, .y = 490, .w = 60, .h = 20 }));
  CHECK((placed[1] == scav_rect{ .x = 40, .y = 470, .w = 60, .h = 20 }));
  CHECK(placed[1].y < placed[0].y);
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 1);
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 1);
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
  CHECK((on_route(leg, {}, { .x = 0, .y = 131, .w = 600, .h = 269 }, LABEL, fell) ==
         scav_rect{ .x = 220, .y = 150, .w = 60, .h = 20 }));
  CHECK(fell == 0);
}

TEST_CASE(
    "label: a stranger's rect blocks a candidate it overlaps and not one it touches") {
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  uint32_t fell{ 0 };
  scav_rect const flush{ .x = 200, .y = 30, .w = 100, .h = 100 };  // bottom at 130
  CHECK((on_route(leg, { flush }, CHART, LABEL, fell) ==
         scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
  scav_rect const over{ .x = 200, .y = 31, .w = 100, .h = 100 };  // bottom at 131
  CHECK((on_route(leg, { over }, CHART, LABEL, fell) ==
         scav_rect{ .x = 220, .y = 150, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 245, .y = 110, .w = 60, .h = 20 }));

  // The band covers the near strips of both sides, so the box takes the second
  // strip on the far one.
  z.after[outer.v] = { .x = 10, .y = 100, .w = 580, .h = 40 };
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 245, .y = 150, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
  CHECK((placed[1] == scav_rect{ .x = 220, .y = 150, .w = 60, .h = 20 }));
  CHECK(!overlaps(placed[0], placed[1]));
}

TEST_CASE("label: a candidate over a leg of its own route it does not ride is refused") {
  // The elbow's own upright leg sits under the nearest candidate of the leg the
  // box rides, and under the nearest candidate of the upright leg itself.
  std::vector<scav_point> const elbow{ { .x = 350, .y = 150 },
                                       { .x = 400, .y = 150 },
                                       { .x = 400, .y = 0 } };
  uint32_t fell{ 0 };
  CHECK((on_route(elbow, {}, CHART, LABEL, fell) ==
         scav_rect{ .x = 345, .y = 150, .w = 60, .h = 20 }));
  CHECK(fell == 0);
}

TEST_CASE("label: each of the five strips is reachable and there is no sixth") {
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  for (int32_t n = 0; n < 5; ++n) {
    CAPTURE(n);
    // Everything from the nth strip's far edge down through the whole high side.
    scav_rect const wall{ .x = 0, .y = 150 - (20 * n), .w = 600, .h = 100 + (20 * n) };
    uint32_t fell{ 0 };
    CHECK((on_route(leg, { wall }, CHART, LABEL, fell) ==
           scav_rect{ .x = 220, .y = 130 - (20 * n), .w = 60, .h = 20 }));
    CHECK(fell == 0);
  }
  uint32_t fell{ 0 };
  scav_rect const all{ .x = 0, .y = 50, .w = 600, .h = 200 };
  CHECK((on_route(leg, { all }, CHART, LABEL, fell) ==
         scav_rect{ .x = 220, .y = 140, .w = 60, .h = 20 }));
  CHECK(fell == 1);
}

TEST_CASE("label: the slide hits the leg's low end, its high end, and its exact centre") {
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  uint32_t fell{ 0 };

  scav_rect const past_low{ .x = 130, .y = 50, .w = 470, .h = 200 };
  CHECK((on_route(leg, { past_low }, CHART, LABEL, fell) ==
         scav_rect{ .x = 70, .y = 130, .w = 60, .h = 20 }));
  CHECK(fell == 0);

  scav_rect const before_high{ .x = 0, .y = 50, .w = 370, .h = 200 };
  CHECK((on_route(leg, { before_high }, CHART, LABEL, fell) ==
         scav_rect{ .x = 370, .y = 130, .w = 60, .h = 20 }));

  // 250 is no multiple of the box height above 100, so only the centre slot
  // reaches the window; its neighbours at 240 and 260 hit a wall.
  scav_rect const left{ .x = 0, .y = 50, .w = 220, .h = 200 };
  scav_rect const right{ .x = 280, .y = 50, .w = 320, .h = 200 };
  CHECK((on_route(leg, { left, right }, CHART, LABEL, fell) ==
         scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
  CHECK(fell == 0);
}

TEST_CASE(
    "label: a leg no whole number of box heights long still offers its end and its "
    "centre") {
  // 295 long: the slots run 100..380 and the end and the centre are the two the
  // enumeration adds past them.
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 395, .y = 150 } };
  uint32_t fell{ 0 };

  scav_rect const before_high{ .x = 0, .y = 50, .w = 365, .h = 200 };
  CHECK((on_route(leg, { before_high }, CHART, LABEL, fell) ==
         scav_rect{ .x = 365, .y = 130, .w = 60, .h = 20 }));
  CHECK(fell == 0);

  scav_rect const left{ .x = 0, .y = 50, .w = 217, .h = 200 };
  scav_rect const right{ .x = 277, .y = 50, .w = 323, .h = 200 };
  CHECK((on_route(leg, { left, right }, CHART, LABEL, fell) ==
         scav_rect{ .x = 217, .y = 130, .w = 60, .h = 20 }));

  scav_rect const past_low{ .x = 130, .y = 50, .w = 470, .h = 200 };
  CHECK((on_route(leg, { past_low }, CHART, LABEL, fell) ==
         scav_rect{ .x = 70, .y = 130, .w = 60, .h = 20 }));
  CHECK(fell == 0);
}

TEST_CASE("label: the anchor distance outranks the leg a candidate rides") {
  // The anchor is the horizontal leg's midpoint, so the second leg holds every
  // near candidate and the upright first leg holds none.
  std::vector<scav_point> const bend{ { .x = 100, .y = 300 },
                                      { .x = 100, .y = 150 },
                                      { .x = 400, .y = 150 } };
  uint32_t fell{ 0 };
  CHECK((on_route(bend, {}, CHART, LABEL, fell) ==
         scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
  CHECK(fell == 0);
}

TEST_CASE("label: the leg outranks the side") {
  // The wall leaves one candidate on each upright leg, equally far from the
  // anchor: the first leg's is on the high side and the third leg's on the low.
  std::vector<scav_point> const bracket{ { .x = 400, .y = 50 },
                                         { .x = 400, .y = 150 },
                                         { .x = 100, .y = 150 },
                                         { .x = 100, .y = 50 } };
  scav_rect const wall{ .x = 60, .y = 0, .w = 380, .h = 400 };
  uint32_t fell{ 0 };
  CHECK((on_route(bracket, { wall }, CHART, LABEL, fell) ==
         scav_rect{ .x = 440, .y = 140, .w = 60, .h = 20 }));
  CHECK(fell == 0);
}

TEST_CASE("label: the side outranks the strip") {
  // 280 long, so the centre 240 is a slot. Three candidates tie at 50: the low
  // side's third strip and the high side's first at two slides either side.
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 380, .y = 150 } };
  std::vector<scav_rect> const walls{
    { .x = 0, .y = 110, .w = 600, .h = 40 },   // the low side's first two strips
    { .x = 230, .y = 150, .w = 20, .h = 20 },  // the high side's first strip, mid-leg
    { .x = 0, .y = 170, .w = 600, .h = 80 }    // the high side's other four strips
  };
  uint32_t fell{ 0 };
  CHECK((on_route(leg, walls, CHART, LABEL, fell) ==
         scav_rect{ .x = 210, .y = 90, .w = 60, .h = 20 }));
  CHECK(fell == 0);
}

TEST_CASE("label: the strip outranks the slide") {
  // The wall takes the centre slide off both first strips, leaving the first
  // strip one slide out and the second strip on centre, tied at 30.
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 380, .y = 150 } };
  scav_rect const wall{ .x = 210, .y = 130, .w = 20, .h = 40 };
  uint32_t fell{ 0 };
  CHECK((on_route(leg, { wall }, CHART, LABEL, fell) ==
         scav_rect{ .x = 230, .y = 130, .w = 60, .h = 20 }));
  CHECK(fell == 0);
}

TEST_CASE("label: candidates alike to the slide go to the lower one") {
  // A box taller than it is wide steps further than its own width, so a wall can
  // take the centre slide alone and leave the two either side of it tied.
  std::vector<scav_point> const leg{ { .x = 100, .y = 150 }, { .x = 400, .y = 150 } };
  scav_rect const wall{ .x = 235, .y = 90, .w = 20, .h = 120 };
  scav_path_box const tall{ .subject = 0, .w = 20, .h = 60, .order = 0 };
  uint32_t fell{ 0 };
  CHECK((on_route(leg, { wall }, CHART, tall, fell) ==
         scav_rect{ .x = 210, .y = 90, .w = 20, .h = 60 }));
  CHECK(fell == 0);
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 250, .y = 240, .w = 60, .h = 20 }));
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
  z.state[wall.v] = { .x = 0, .y = 130, .w = 600, .h = 40 };  // both first strips
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                            { { .x = 100, .y = 95 }, { .x = 400, .y = 95 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  // Both second strips are the same distance away; the low one reads as the
  // stranger's label, so the high one wins.
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 170, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 190, .y = 130, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 210, .y = 150, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  scav_rect const alone{ .x = 220, .y = 130, .w = 60, .h = 20 };
  CHECK((placed[0] == alone));

  // Another route and another box's worth of state, both out of reach: neither
  // the shortfall nor the sweep has anything to say, so nothing moves.
  z.state[far.v] = { .x = 0, .y = 300, .w = 600, .h = 100 };
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 1);
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
  // The upright leg is enumerated for the second box too, and loses on distance
  // rather than on being out of bounds.
  CHECK((placed[1] == scav_rect{ .x = 230, .y = 150, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, none, l.route, l.points, placed) == 0);
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
  z.state[gone.v] = { .x = 0, .y = 130, .w = 600, .h = 40 };  // both first strips
  Lines const l{ lines_of({ { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 110, .w = 60, .h = 20 }));

  c.states[gone.v].live = 0;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 220, .y = 130, .w = 60, .h = 20 }));
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
  CHECK(place_labels(c, z, boxes_of(forward), l.route, l.points, first) == 0);
  CHECK(place_labels(c, z, boxes_of(backward), l.route, l.points, second) == 0);
  CHECK((first[0] == second[1]));
  CHECK((first[1] == second[0]));
}

TEST_CASE("label: a transition's second box never goes back to an earlier leg") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const lower{ build_state(c, root, "L", StateKind::Normal, {}) };
  StateId const strip{ build_state(c, root, "S", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c, CHART) };
  // The walls leave the horizontal leg one candidate, further from the anchor
  // than the upright leg's, so the first box takes the upright one.
  z.state[lower.v] = { .x = 70, .y = 70, .w = 360, .h = 180 };
  z.state[strip.v] = { .x = 70, .y = 50, .w = 300, .h = 20 };
  Lines const l{ lines_of(
      { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 }, { .x = 400, .y = 250 } } }) };
  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 60, .h = 20, .order = 0 },
                                          { .subject = 0, .w = 60, .h = 20, .order = 1 } };

  std::vector<scav_rect> placed;
  CHECK(place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) == 0);
  CHECK((placed[0] == scav_rect{ .x = 440, .y = 140, .w = 60, .h = 20 }));
  // The horizontal leg's survivor ties the second box's best on distance and
  // would win on leg order; the second box may not go back to it.
  CHECK((placed[1] == scav_rect{ .x = 440, .y = 160, .w = 60, .h = 20 }));
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
  uint32_t const fell{ place_labels(c, z, boxes_of(boxes), l.route, l.points, placed) };
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
