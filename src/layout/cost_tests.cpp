// Scoring against hand-written geometry: two rects and one route are enough
// to assert a single term, with no model beyond the entities they belong to
// and no pipeline run to produce them.

#include "layout/cost.h"

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {

using namespace scav;

scav_profile profile() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

SizedLayout blank(Chart const &c) {
  SizedLayout z;
  z.state.assign(c.states.size(), scav_rect{});
  z.before.assign(c.states.size(), scav_rect{});
  z.after.assign(c.states.size(), scav_rect{});
  z.sub.assign(c.submachines.size(), scav_rect{});
  return z;
}

// One polyline per transition, in transition order, as `Routes` holds them.
Routes routes_of(Chart const &c, std::vector<std::vector<scav_point>> const &lines) {
  Routes r;
  r.route.assign(c.transitions.size(), scav_span{});
  r.port.assign(c.transitions.size(), scav_span{});
  for (uint32_t t = 0; (t < lines.size()) && (t < c.transitions.size()); ++t) {
    r.route[t] = { .off = static_cast<uint32_t>(r.points.size()),
                   .len = static_cast<uint32_t>(lines[t].size()) };
    for (scav_point const &pt : lines[t]) { r.points.push_back(pt); }
  }
  return r;
}

}  // namespace

TEST_CASE("cost: a straight route between two boxes costs nothing but the chart") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 0, .w = 100, .h = 40 };
  z.chart = { .x = 0, .y = 0, .w = 400, .h = 40 };
  Routes const r{ routes_of(c, { { { .x = 100, .y = 20 }, { .x = 300, .y = 20 } } }) };

  CostTerms const t{ cost_terms(c, decompose(c), z, r, {}, profile()) };
  CHECK(t.bends == 0);
  CHECK(t.crossings == 0);
  CHECK(t.excess_len == 0);
  CHECK(t.through_box == 0);
  CHECK(t.box_overlap == 0);
  CHECK(t.area == 400LL * 40);
  CHECK(t.aspect == ((400LL * 10) - (40LL * 16)));
}

TEST_CASE("cost: a corner in a polyline is one bend") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  Routes const r{
    routes_of(c, { { { .x = 0, .y = 0 }, { .x = 100, .y = 0 }, { .x = 100, .y = 100 } } })
  };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).bends == 1);

  // Three points on one line change no direction, so they are not a bend.
  Routes const straight{
    routes_of(c, { { { .x = 0, .y = 0 }, { .x = 50, .y = 0 }, { .x = 100, .y = 0 } } })
  };
  CHECK(cost_terms(c, decompose(c), z, straight, {}, profile()).bends == 0);
}

TEST_CASE("cost: two routes that properly cross count once") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const e{ build_state(c, root, "E", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, d, e, TransKind::External, {});

  SizedLayout z{ blank(c) };
  Routes const r{ routes_of(c,
                            { { { .x = 0, .y = 0 }, { .x = 100, .y = 100 } },
                              { { .x = 0, .y = 100 }, { .x = 100, .y = 0 } } }) };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).crossings == 1);

  // Meeting at a shared endpoint is not a crossing.
  Routes const touching{ routes_of(c,
                                   { { { .x = 0, .y = 0 }, { .x = 50, .y = 50 } },
                                     { { .x = 50, .y = 50 }, { .x = 100, .y = 0 } } }) };
  CHECK(cost_terms(c, decompose(c), z, touching, {}, profile()).crossings == 0);
}

namespace {

// `n` transitions between two states, which is all `corridor` reads of a model.
Chart edges(uint32_t n) {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  for (uint32_t i = 0; i < n; ++i) { build_trans(c, a, b, TransKind::External, {}); }
  return c;
}

int64_t corridor_of(Chart const &c, std::vector<std::vector<scav_point>> const &lines) {
  SizedLayout const z{ blank(c) };
  return cost_terms(c, decompose(c), z, routes_of(c, lines), {}, profile()).corridor;
}

}  // namespace

TEST_CASE("cost: two routes ending as one line are not charged for the run they share") {
  Chart const c{ edges(2) };
  // Both turn up onto x=500 and finish at the same point, so the 20 they share
  // is one line fanning in rather than two lines side by side.
  CHECK(corridor_of(
            c,
            { { { .x = 0, .y = 60 }, { .x = 500, .y = 60 }, { .x = 500, .y = 100 } },
              { { .x = 0, .y = 80 }, { .x = 500, .y = 80 }, { .x = 500, .y = 100 } } }) ==
        0);

  // One unit short of the same point: the same 20 units of one line, now with
  // two ends on it.
  CHECK(corridor_of(
            c,
            { { { .x = 0, .y = 60 }, { .x = 500, .y = 60 }, { .x = 500, .y = 100 } },
              { { .x = 0, .y = 80 }, { .x = 500, .y = 80 }, { .x = 500, .y = 101 } } }) ==
        20);
}

TEST_CASE("cost: a shared endpoint alone exempts nothing") {
  Chart const c{ edges(2) };
  // The two reach (200,300) at right angles, so neither last leg joins the
  // other's run and the 100 they share upstream is two lines on one line.
  CHECK(corridor_of(
            c,
            { { { .x = 0, .y = 100 }, { .x = 200, .y = 100 }, { .x = 200, .y = 300 } },
              { { .x = 100, .y = 100 },
                { .x = 300, .y = 100 },
                { .x = 300, .y = 300 },
                { .x = 200, .y = 300 } } }) == 100);
}

TEST_CASE("cost: a run upstream of the merge is charged and the merge is not") {
  Chart const c{ edges(2) };
  auto const lines = [](int32_t end_y) {
    return std::vector<std::vector<scav_point>>{ { { .x = 0, .y = 0 },
                                                   { .x = 300, .y = 0 },
                                                   { .x = 300, .y = 60 },
                                                   { .x = 500, .y = 60 },
                                                   { .x = 500, .y = 100 } },
                                                 { { .x = 100, .y = 0 },
                                                   { .x = 400, .y = 0 },
                                                   { .x = 400, .y = 80 },
                                                   { .x = 500, .y = 80 },
                                                   { .x = 500, .y = end_y } } };
  };
  // 200 along y=0 before either turns off it, and 20 where they finish as one.
  CHECK(corridor_of(c, lines(100)) == 200);
  CHECK(corridor_of(c, lines(101)) == 220);
}

TEST_CASE("cost: two routes leaving as one line are not charged for it") {
  Chart const c{ edges(2) };
  auto const lines = [](int32_t start_x) {
    return std::vector<std::vector<scav_point>>{
      { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
      { { .x = start_x, .y = 0 }, { .x = 150, .y = 0 }, { .x = 150, .y = -100 } }
    };
  };
  CHECK(corridor_of(c, lines(0)) == 0);
  CHECK(corridor_of(c, lines(1)) == 149);
}

TEST_CASE("cost: three routes on one trunk are free over all three pairs") {
  Chart const c{ edges(3) };
  auto const lines = [](int32_t end_y) {
    return std::vector<std::vector<scav_point>>{
      { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
      { { .x = 50, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
      { { .x = 100, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = end_y } }
    };
  };
  CHECK(corridor_of(c, lines(100)) == 0);
  // The third one unit past the others: its two pairs are charged and the pair
  // that still ends as one line is not.
  CHECK(corridor_of(c, lines(101)) == 400);
}

TEST_CASE("cost: two routes with the same polyline are one trunk end to end") {
  Chart const c{ edges(2) };
  CHECK(corridor_of(
            c,
            { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
              { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } } }) ==
        0);
}

TEST_CASE("cost: a route that is the whole of another's end is trunk end to end") {
  Chart const c{ edges(2) };
  // A degraded net is a straight line (11.5), and one drawn between two points of
  // another route is that route's tail or its head rather than a second lane.
  CHECK(corridor_of(c,
                    { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
                      { { .x = 200, .y = 0 }, { .x = 200, .y = 100 } } }) == 0);
  CHECK(corridor_of(c,
                    { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
                      { { .x = 0, .y = 0 }, { .x = 200, .y = 0 } } }) == 0);
}

TEST_CASE("cost: a run the two find again after they part is charged") {
  Chart const c{ edges(2) };
  // 100 along y=0 out of the shared start, which is free, and 100 more along
  // y=100 where the two happen to meet again, which is two lanes on one line.
  CHECK(corridor_of(c,
                    { { { .x = 0, .y = 0 },
                        { .x = 200, .y = 0 },
                        { .x = 200, .y = 100 },
                        { .x = 400, .y = 100 } },
                      { { .x = 0, .y = 0 },
                        { .x = 100, .y = 0 },
                        { .x = 100, .y = 200 },
                        { .x = 300, .y = 200 },
                        { .x = 300, .y = 100 },
                        { .x = 500, .y = 100 } } }) == 100);
}

TEST_CASE("cost: a run against the trunk is charged and the trunk is not") {
  Chart const c{ edges(2) };
  // The second crosses x=400 on its way round and joins it later; the 50 it
  // spends on the trunk before joining it is one lane over another.
  CHECK(
      corridor_of(c,
                  { { { .x = 300, .y = 0 }, { .x = 400, .y = 0 }, { .x = 400, .y = 100 } },
                    { { .x = 200, .y = 50 },
                      { .x = 400, .y = 50 },
                      { .x = 400, .y = -100 },
                      { .x = 600, .y = -100 },
                      { .x = 600, .y = 0 },
                      { .x = 400, .y = 0 },
                      { .x = 400, .y = 100 } } }) == 50);
}

TEST_CASE("cost: a degraded net's diagonal is no one's merge leg") {
  Chart const c{ edges(2) };
  // A degraded net is a straight line (11.5), so a trunk can be reached over one.
  // The common suffix is still a trunk; the diagonals into it are not, so the
  // 200 the two share along y=200 is charged and the 100 they share is not.
  CHECK(corridor_of(c,
                    { { { .x = 0, .y = 200 },
                        { .x = 300, .y = 200 },
                        { .x = 400, .y = 100 },
                        { .x = 400, .y = 0 } },
                      { { .x = 100, .y = 200 },
                        { .x = 350, .y = 200 },
                        { .x = 400, .y = 100 },
                        { .x = 400, .y = 0 } } }) == 200);
}

TEST_CASE("cost: only the excess over the direct distance is charged") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  // Straight: length equals the direct distance, so nothing is charged.
  Routes const direct{ routes_of(c, { { { .x = 0, .y = 0 }, { .x = 300, .y = 0 } } }) };
  CHECK(cost_terms(c, decompose(c), z, direct, {}, profile()).excess_len == 0);

  // A detour of 100 each way over a 300 span costs exactly what it added.
  Routes const around{
    routes_of(c, { { { .x = 0, .y = 0 }, { .x = 150, .y = 100 }, { .x = 300, .y = 0 } } })
  };
  CostTerms const t{ cost_terms(c, decompose(c), z, around, {}, profile()) };
  CHECK(t.excess_len == ((2 * 180) - 300));  // isqrt(150^2 + 100^2) is 180
}

TEST_CASE("cost: overlapping siblings are a Tier-0 violation") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 50, .y = 50, .w = 100, .h = 100 };
  CostTerms const t{ cost_terms(c, decompose(c), z, {}, {}, profile()) };
  CHECK(t.box_overlap == 1);
  CHECK(cost_of(t, profile()).t0_violations == 1);
}

TEST_CASE("cost: an edge through a stranger's box counts, through its own does not") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 20, .h = 20 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 20, .h = 20 };
  z.state[other.v] = { .x = 150, .y = -50, .w = 100, .h = 100 };
  Routes const r{ routes_of(c, { { { .x = 10, .y = 10 }, { .x = 410, .y = 10 } } }) };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).through_box == 1);

  // The same route with the stranger moved out of the way, and the route
  // still leaving its own two endpoint boxes, which are carved out (11.14).
  z.state[other.v] = { .x = 150, .y = 500, .w = 100, .h = 100 };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).through_box == 0);
}

TEST_CASE("cost: a placed box over a state neither endpoint is under costs") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 100 };
  z.state[other.v] = { .x = 200, .y = -200, .w = 100, .h = 100 };
  Routes r{ routes_of(c, { { { .x = 100, .y = 50 }, { .x = 400, .y = 50 } } }) };
  r.placed = { { .x = 200, .y = -190, .w = 60, .h = 20 } };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 1);

  z.state[other.v] = { .x = 200, .y = 500, .w = 100, .h = 100 };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 0);
}

TEST_CASE("cost: inside the composite it runs in, only the text bands cost") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 600, .h = 200 };
  z.before[outer.v] = { .x = 10, .y = 10, .w = 580, .h = 30 };
  z.state[a.v] = { .x = 50, .y = 80, .w = 100, .h = 60 };
  z.state[b.v] = { .x = 400, .y = 80, .w = 100, .h = 60 };
  Routes r{ routes_of(c, { { { .x = 150, .y = 110 }, { .x = 400, .y = 110 } } }) };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };

  r.placed = { { .x = 200, .y = 90, .w = 60, .h = 20 } };  // in Outer, clear of its band
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 0);

  r.placed = { { .x = 200, .y = 15, .w = 60, .h = 20 } };  // in Outer's title band
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 1);
}

TEST_CASE("cost: a placed box over another transition's route is a label cost") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, a, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 100 };
  Routes r{ routes_of(c,
                      { { { .x = 100, .y = 50 }, { .x = 400, .y = 50 } },
                        { { .x = 400, .y = 80 }, { .x = 100, .y = 80 } } }) };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };

  // Straddling its own route is free and straddling the other one is not, so
  // the two rects differ only in which line they cross.
  r.placed = { { .x = 200, .y = 40, .w = 60, .h = 20 } };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 0);

  r.placed = { { .x = 200, .y = 70, .w = 60, .h = 20 } };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 1);
}

TEST_CASE("cost: a box nearer a foreign route than its own is charged the shortfall") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, a, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 200 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 200 };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  // The box rides its own leg at y 150; the foreign leg's height above it is
  // the only thing that changes between the cases.
  auto const scored = [&](int32_t foreign_y) {
    Routes r{ routes_of(
        c,
        { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
          { { .x = 400, .y = foreign_y }, { .x = 100, .y = foreign_y } } }) };
    r.placed = { { .x = 200, .y = 130, .w = 60, .h = 20 } };
    return cost_terms(c, decompose(c), z, r, s, profile()).label_near;
  };

  CHECK(scored(120) == 10);  // half a line of text nearer the stranger than it may be
  CHECK(scored(110) == 0);   // one whole line away, which is the margin it owes
  CHECK(scored(90) == 0);

  // Strip 1: the box's own leg is one height away, so the stranger has to be
  // one height further out again, and here it is not.
  Routes strip1{ routes_of(c,
                           { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                             { { .x = 400, .y = 90 }, { .x = 100, .y = 90 } } }) };
  strip1.placed = { { .x = 200, .y = 110, .w = 60, .h = 20 } };
  CHECK(cost_terms(c, decompose(c), z, strip1, s, profile()).label_near == 20);
}

TEST_CASE("cost: with no other route to be near, no box is charged") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  Routes r{ routes_of(c, { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  r.placed = { { .x = 200, .y = 130, .w = 60, .h = 20 } };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label_near == 0);
}

TEST_CASE("cost: a placed box the space table does not name owns no route") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  Routes r{ routes_of(c, { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  r.placed = { { .x = 200, .y = 140, .w = 60, .h = 20 } };  // over that one line
  auto const scored = [&](scav_spaces const &s) {
    return cost_terms(c, decompose(c), z, r, s, profile());
  };

  // Named, the line is its own and free; unnamed, it is a stranger's, and no
  // leg of its own means no nearness to price either way.
  scav_path_box const named{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  CHECK(scored({ .path_box = &named, .n_path_box = 1 }).label == 0);
  CHECK(scored({}).label == 1);
  CHECK(scored({}).label_near == 0);

  // A table too short to reach the box, and one naming a transition that is not
  // there, leave it a stranger's the same way.
  CHECK(scored({ .path_box = &named, .n_path_box = 0 }).label == 1);
  scav_path_box const past{ .subject = 7, .w = 60, .h = 20, .order = 0 };
  CHECK(scored({ .path_box = &past, .n_path_box = 1 }).label == 1);
}

TEST_CASE("cost: a box whose own transition has no route is charged nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::Internal, {});  // no route: drawn in A's rect
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  // The box belongs to the routeless transition and sits beside the other's
  // line, which is exactly the arrangement the term would charge for.
  Routes r{ routes_of(c, { {}, { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  r.placed = { { .x = 200, .y = 120, .w = 60, .h = 20 } };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };

  // No leg of its own to be nearer than, so there is no shortfall to measure.
  CostTerms const t{ cost_terms(c, decompose(c), z, r, s, profile()) };
  CHECK(t.label_near == 0);
  CHECK(t.label == 0);
}

TEST_CASE("cost: two placed boxes over each other are one label cost") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 100 };
  Routes r{ routes_of(c, { { { .x = 100, .y = 50 }, { .x = 400, .y = 50 } } }) };
  std::array<scav_path_box, 2> const boxes{
    { { .subject = 0, .w = 60, .h = 20, .order = 0 },
      { .subject = 0, .w = 60, .h = 20, .order = 1 } }
  };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 2 };

  r.placed = { { .x = 200, .y = 20, .w = 60, .h = 20 },
               { .x = 230, .y = 30, .w = 60, .h = 20 } };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 1);

  r.placed[1] = { .x = 300, .y = 20, .w = 60, .h = 20 };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 0);
}

TEST_CASE("cost: the weights turn terms into one integer, compared by tier") {
  scav_profile const p{ profile() };
  CostTerms t;
  t.crossings = 2;
  t.area = 1000;
  Cost const scored{ cost_of(t, p) };
  CHECK(scored.t0_violations == 0);
  CHECK(scored.t2 == ((int64_t{ p.w_crossings } * 2) + (int64_t{ p.w_area } * 1000)));

  // Tier 0 dominates whatever Tier 2 says, because the tiers are compared in
  // order and never summed.
  CostTerms violating;
  violating.box_overlap = 1;
  Cost const bad{ cost_of(violating, p) };
  CHECK(cost_less(scored, bad));
  CHECK_FALSE(cost_less(bad, scored));
}

TEST_CASE("cost: a hint outranks whatever Tier 2 adds up to") {
  // Tier 1 sits between the forbidden and the priced, so the two are compared
  // on it before either sum is looked at.
  Cost const hinted{ .t0_violations = 0, .t1_hints = 1, .t2 = 1000000 };
  Cost const unhinted{ .t0_violations = 0, .t1_hints = 2, .t2 = 0 };
  CHECK(cost_less(hinted, unhinted));
  CHECK_FALSE(cost_less(unhinted, hinted));

  // Equal hints and the Tier-2 sum decides after all.
  Cost const same{ .t0_violations = 0, .t1_hints = 1, .t2 = 1000001 };
  CHECK(cost_less(hinted, same));
}

namespace {

// Two concurrent submachines of one state joined by one transition, which is
// the whole of what `adjacency` reads of a model (11.8).
struct Regions {
  Chart c;
  SubmachineId left{ INVALID }, right{ INVALID };
};

Regions regions(StateKind src_kind, StateKind dst_kind) {
  Regions out;
  SubmachineId const root{ build_chart(out.c, "t", {}) };
  StateId const owner{ build_state(out.c, root, "Owner", StateKind::Normal, {}) };
  out.left = build_submachine(out.c, owner, "left", {});
  out.right = build_submachine(out.c, owner, "right", {});
  StateId const a{ build_state(out.c, out.left, "A", src_kind, {}) };
  StateId const b{ build_state(out.c, out.right, "B", dst_kind, {}) };
  build_trans(out.c, a, b, TransKind::External, {});
  return out;
}

// The term for one placement of the two regions, with no route to score.
int64_t adjacency_of(Regions const &r, scav_rect const &left, scav_rect const &right) {
  SizedLayout z{ blank(r.c) };
  z.sub[r.left.v] = left;
  z.sub[r.right.v] = right;
  return cost_terms(r.c, decompose(r.c), z, routes_of(r.c, {}), {}, profile()).adjacency;
}

}  // namespace

TEST_CASE("cost: two regions an arrow joins are charged for being apart") {
  Regions const r{ regions(StateKind::Normal, StateKind::Normal) };
  scav_profile const p{ profile() };
  scav_rect const at{ .x = 0, .y = 0, .w = 100, .h = 100 };

  // Overlapping on one axis and no more than sub_sep apart on the other is the
  // arrangement a direct arrow needs; one unit further apart is not.
  CHECK(adjacency_of(r, at, { .x = 100 + p.sub_sep, .y = 0, .w = 100, .h = 100 }) == 0);
  CHECK(adjacency_of(r, at, { .x = 101 + p.sub_sep, .y = 0, .w = 100, .h = 100 }) == 1);
  CHECK(adjacency_of(r, at, { .x = 0, .y = 100 + p.sub_sep, .w = 100, .h = 100 }) == 0);
  CHECK(adjacency_of(r, at, { .x = 0, .y = 101 + p.sub_sep, .w = 100, .h = 100 }) == 1);

  // The pair is unordered: the same two rects the other way round.
  CHECK(adjacency_of(r, { .x = 100 + p.sub_sep, .y = 0, .w = 100, .h = 100 }, at) == 0);
  CHECK(adjacency_of(r, { .x = 0, .y = 100 + p.sub_sep, .w = 100, .h = 100 }, at) == 0);

  // Diagonal is neither: two regions meeting at a corner share no face for the
  // arrow to cross.
  CHECK(adjacency_of(r, at, { .x = 400, .y = 400, .w = 100, .h = 100 }) == 1);

  // The one term the weights turn into a Tier-2 cost, nothing else being scored.
  SizedLayout z{ blank(r.c) };
  z.sub[r.left.v] = at;
  z.sub[r.right.v] = { .x = 400, .y = 400, .w = 100, .h = 100 };
  CostTerms const t{ cost_terms(r.c, decompose(r.c), z, routes_of(r.c, {}), {}, p) };
  CHECK(t.adjacency == 1);
  CHECK(cost_of(t, p).t2 == int64_t{ p.w_adjacency });
}

TEST_CASE("cost: a fan-out across two regions is not charged for being apart") {
  // Adjacency above two regions is not achievable, so a fork or a join at
  // either end of the crossing takes the pair out of the term (11.8).
  scav_rect const at{ .x = 0, .y = 0, .w = 100, .h = 100 };
  scav_rect const away{ .x = 400, .y = 400, .w = 100, .h = 100 };
  CHECK(adjacency_of(regions(StateKind::Normal, StateKind::Normal), at, away) == 1);
  CHECK(adjacency_of(regions(StateKind::Fork, StateKind::Normal), at, away) == 0);
  CHECK(adjacency_of(regions(StateKind::Join, StateKind::Normal), at, away) == 0);
  CHECK(adjacency_of(regions(StateKind::Normal, StateKind::Fork), at, away) == 0);
  CHECK(adjacency_of(regions(StateKind::Normal, StateKind::Join), at, away) == 0);

  // Another pseudostate is not one of the two, so its crossing is priced.
  CHECK(adjacency_of(regions(StateKind::Choice, StateKind::Normal), at, away) == 1);
}

TEST_CASE("cost: a route that leaves a box across its far side is inside it") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 20, .h = 20 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 20, .h = 20 };
  z.state[other.v] = { .x = 100, .y = 0, .w = 100, .h = 100 };
  // Starting on the stranger's top border, which is not inside it, and cutting
  // out through the right one: the only one of the four sides it crosses.
  Routes const r{ routes_of(c, { { { .x = 150, .y = 0 }, { .x = 250, .y = 50 } } }) };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).through_box == 1);

  // Along that same border rather than through it, which is not entry.
  Routes const grazing{ routes_of(c, { { { .x = 100, .y = 0 }, { .x = 250, .y = 0 } } }) };
  CHECK(cost_terms(c, decompose(c), z, grazing, {}, profile()).through_box == 0);
}

TEST_CASE("cost: a tombstone is not a box, an obstacle or a label collision") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const gone{ build_state(c, root, "Gone", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  SubmachineId const dropped{ build_submachine(c, b, "dropped", {}) };
  StateId const p{ build_state(c, dropped, "P", StateKind::Normal, {}) };
  StateId const q{ build_state(c, dropped, "Q", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 100 };
  // Over A, over the route, and under the placed box, so one state answers all
  // three loops at once.
  z.state[gone.v] = { .x = 80, .y = 10, .w = 200, .h = 60 };
  // Out of everything else's way, and over each other.
  z.state[p.v] = { .x = 0, .y = 900, .w = 100, .h = 100 };
  z.state[q.v] = { .x = 50, .y = 950, .w = 100, .h = 100 };
  Routes r{ routes_of(c, { { { .x = 100, .y = 50 }, { .x = 400, .y = 50 } } }) };
  r.placed = { { .x = 200, .y = 20, .w = 60, .h = 20 } };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  auto const scored = [&] { return cost_terms(c, decompose(c), z, r, s, profile()); };

  CostTerms const live{ scored() };
  CHECK(live.box_overlap == 2);  // A over Gone, and P over Q
  CHECK(live.through_box == 1);
  CHECK(live.label == 1);

  // A dead submachine's children are no longer siblings of each other.
  c.submachines[dropped.v].live = 0;
  CHECK(scored().box_overlap == 1);

  // And a dead state is no box at all, first or second of its pair.
  c.states[gone.v].live = 0;
  CostTerms const buried{ scored() };
  CHECK(buried.box_overlap == 0);
  CHECK(buried.through_box == 0);
  CHECK(buried.label == 0);
}

TEST_CASE("cost: a box in the composite's trailing band costs as its title does") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 600, .h = 200 };
  z.after[outer.v] = { .x = 10, .y = 160, .w = 580, .h = 30 };
  z.state[a.v] = { .x = 50, .y = 20, .w = 100, .h = 60 };
  z.state[b.v] = { .x = 400, .y = 20, .w = 100, .h = 60 };
  Routes r{ routes_of(c, { { { .x = 150, .y = 50 }, { .x = 400, .y = 50 } } }) };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };

  r.placed = { { .x = 200, .y = 120, .w = 60, .h = 20 } };  // clear of both bands
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 0);

  r.placed = { { .x = 200, .y = 165, .w = 60, .h = 20 } };  // in Outer's trailing band
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label == 1);
}

TEST_CASE("cost: a chart with no geometry columns scores as nothing") {
  // The columns are what one build reads of another's output, and a chart that
  // was never laid out has none of them: every term reads zero rather than
  // whatever the empty vectors happen to be.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});

  CostTerms const t{ cost_columns(c, decompose(c), profile()) };
  CHECK(t.bends == 0);
  CHECK(t.corridor == 0);
  CHECK(t.crossings == 0);
  CHECK(t.excess_len == 0);
  CHECK(t.adjacency == 0);
  CHECK(t.label == 0);
  CHECK(t.label_near == 0);
  CHECK(t.aspect == 0);
  CHECK(t.area == 0);
  CHECK(t.through_box == 0);
  CHECK(t.box_overlap == 0);
  CHECK(cost_of(t, profile()).t2 == 0);

  // A column that is there with no row behind it reads the same way: the point
  // column's length is its own, and a chart with no route has none.
  REQUIRE(column_register(c,
                          "scav.geom.point",
                          ElemKind::Point,
                          ValueKind::Pod,
                          sizeof(scav_point),
                          4,
                          COLUMN_DERIVED)
              .v != INVALID);
  REQUIRE(column_count(c, column_find(c, "scav.geom.point")) == 0);
  CostTerms const empty{ cost_columns(c, decompose(c), profile()) };
  CHECK(empty.bends == 0);
  CHECK(empty.area == 0);
  CHECK(cost_of(empty, profile()).t2 == 0);

  // The rect and span columns the same way, on a chart with neither a state
  // nor a transition to have put a row in them.
  Chart bare;
  build_chart(bare, "t", {});
  REQUIRE(column_register(bare,
                          "scav.geom.state",
                          ElemKind::State,
                          ValueKind::Pod,
                          sizeof(scav_rect),
                          4,
                          COLUMN_DERIVED)
              .v != INVALID);
  REQUIRE(column_register(bare,
                          "scav.geom.route",
                          ElemKind::Transition,
                          ValueKind::Pod,
                          sizeof(scav_span),
                          4,
                          COLUMN_DERIVED)
              .v != INVALID);
  REQUIRE(column_count(bare, column_find(bare, "scav.geom.state")) == 0);
  CostTerms const nothing{ cost_columns(bare, decompose(bare), profile()) };
  CHECK(nothing.area == 0);
  CHECK(nothing.box_overlap == 0);
  CHECK(cost_of(nothing, profile()).t2 == 0);
}

TEST_CASE("cost: a chart already at the desired ratio pays no aspect") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  scav_profile const p{ profile() };

  SizedLayout z{ blank(c) };
  z.chart = { .x = 0, .y = 0, .w = 16 * 40, .h = 10 * 40 };
  CostTerms const fitting{ cost_terms(c, decompose(c), z, {}, {}, p) };
  CHECK(fitting.aspect == 0);
  CHECK(fitting.area == ((16LL * 40) * (10LL * 40)));

  // One unit off the ratio is one unit of the term, weighted like any other.
  z.chart.w += 1;
  CostTerms const off{ cost_terms(c, decompose(c), z, {}, {}, p) };
  CHECK(off.aspect == p.dar_den);
  CHECK(cost_of(off, p).t2 ==
        ((int64_t{ p.w_aspect } * p.dar_den) + (int64_t{ p.w_area } * off.area)));
}
