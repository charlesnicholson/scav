// Placement against hand-written geometry: a few rects and one polyline are
// enough, since the candidates come from the route and nothing else.

#include "layout/label.h"

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
