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
