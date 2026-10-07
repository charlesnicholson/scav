// Cost-term tests on hand-written rects and routes over minimal charts.

#include "layout/cost.h"

#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/router.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_int.h"
#include "scav_stable_sort.h"

#include "doctest.h"
#include "scav_vector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace scav {

// Test-only prototypes of the SCAV_INTERNAL functions in `cost.cpp`.
Ancestry cost_flatten_ancestry(Chart const &c);
bool cost_ancestor(Chart const &c, Ancestry const &an, StateId ancestor, StateId of);
ChildGrid cost_child_grid(Chart const &c, SizedLayout const &z);
void cost_grid_query(ChildGrid const &g,
                     uint32_t frame,
                     scav_rect const &q,
                     GridQuery &out);
int32_t cost_box_overlaps(Chart const &c, SizedLayout const &z, ChildGrid const &g);
int32_t cost_through_boxes(Chart const &c,
                           SizedLayout const &z,
                           Ancestry const &an,
                           ChildGrid const &g,
                           std::vector<Piece> const &pieces);
int64_t cost_crossings(std::vector<Piece> const &pieces, std::vector<uint32_t> &per_trans);
Wide cost_corridor(Routes const &r, std::vector<Piece> const &pieces);
Wide cost_crowding(std::vector<Piece> const &pieces, int32_t em);
int32_t cost_path_turns(scav_rect const &s,
                        uint32_t s_face,
                        std::vector<scav_point> const &via,
                        scav_rect const &t,
                        uint32_t t_face);

}  // namespace scav

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

// Splits `r` into the flat segment list `cost_terms` builds, in transition order.
std::vector<Piece> pieces_of(Routes const &r) {
  std::vector<Piece> out;
  for (uint32_t t = 0; t < r.route.size(); ++t) {
    scav_span const route{ r.route[t] };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      out.push_back({ .a = r.points[route.off + k],
                      .b = r.points[route.off + k + 1],
                      .trans = t,
                      .k = k });
    }
  }
  return out;
}

}  // namespace

TEST_CASE("cost: a straight route between two boxes costs its length and the chart") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 0, .w = 100, .h = 40 };
  z.chart = { .x = 0, .y = 0, .w = 400, .h = 40 };
  Routes const r{ routes_of(c, { { { .x = 100, .y = 20 }, { .x = 300, .y = 20 } } }) };

  CostTerms const t{ cost_terms(c, decompose(c), z, r, {}, profile()) };
  CHECK(t.bends == 0);
  CHECK(t.crossings == 0);
  CHECK(t.excess_len == 0);
  CHECK(t.length == 200);
  CHECK(t.through_box == 0);
  CHECK(t.box_overlap == 0);
  CHECK(t.area == 400LL * 40);
  CHECK(t.aspect == ((400LL * 10) - (40LL * 16)));
}

TEST_CASE("cost: a start arrow running up is a quarter bend, running left a whole one") {
  // An initial at the origin into `A`, its last leg each of the four ways.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const start{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, start, a, TransKind::Default, {});
  SizedLayout z{ blank(c) };
  scav_profile const p{ profile() };
  auto const starts = [&](scav_point to) {
    Routes const r{ routes_of(c, { { { .x = 0, .y = 0 }, to } }) };
    return cost_terms(c, decompose(c), z, r, {}, p).backward_starts;
  };
  CHECK(starts({ .x = 0, .y = 100 }) == 0);   // down
  CHECK(starts({ .x = 100, .y = 0 }) == 0);   // right
  CHECK(starts({ .x = 0, .y = -100 }) == 1);  // up
  CHECK(starts({ .x = -100, .y = 0 }) == 4);  // left
  CHECK((int64_t{ p.w_backward_starts } * 4) == p.w_bends);
}

TEST_CASE("cost: a corner in a polyline is one bend") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  Routes const r{
    routes_of(c, { { { .x = 0, .y = 0 }, { .x = 100, .y = 0 }, { .x = 100, .y = 100 } } })
  };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).bends == 1);

  // Three collinear points make no bend.
  Routes const straight{
    routes_of(c, { { { .x = 0, .y = 0 }, { .x = 50, .y = 0 }, { .x = 100, .y = 0 } } })
  };
  CHECK(cost_terms(c, decompose(c), z, straight, {}, profile()).bends == 0);
}

namespace {

// Src in the root, Dst inside Moving inside Arm: Src -> Dst passes through Arm only.
// Then Dst -> Src, and Src -> Arm.
struct Transit {
  Chart c;
  StateId src, arm, moving, dst;
  SizedLayout z;
};

Transit transit_chart() {
  Transit out;
  Chart &c{ out.c };
  SubmachineId const root{ build_chart(c, "t", {}) };
  out.src = build_state(c, root, "Src", StateKind::Normal, {});
  out.arm = build_state(c, root, "Arm", StateKind::Normal, {});
  SubmachineId const arm_m{ build_submachine(c, out.arm, "arm", {}) };
  out.moving = build_state(c, arm_m, "Moving", StateKind::Normal, {});
  SubmachineId const travel{ build_submachine(c, out.moving, "travel", {}) };
  out.dst = build_state(c, travel, "Dst", StateKind::Normal, {});
  build_trans(c, out.src, out.dst, TransKind::Default, {});
  build_trans(c, out.dst, out.src, TransKind::Default, {});
  build_trans(c, out.src, out.arm, TransKind::Default, {});
  out.z = blank(c);
  out.z.state[out.src.v] = { .x = 0, .y = 100, .w = 100, .h = 60 };
  out.z.state[out.arm.v] = { .x = 200, .y = 0, .w = 600, .h = 400 };
  out.z.state[out.moving.v] = { .x = 300, .y = 50, .w = 400, .h = 300 };
  out.z.state[out.dst.v] = { .x = 400, .y = 200, .w = 100, .h = 60 };
  out.z.chart = { .x = 0, .y = 0, .w = 800, .h = 400 };
  return out;
}

// Src's right side to Dst's left, jogging down at `x`: two bends there.
std::vector<scav_point> jog_at(int32_t x) {
  return { { .x = 100, .y = 130 },
           { .x = x, .y = 130 },
           { .x = x, .y = 230 },
           { .x = 400, .y = 230 } };
}

CostTerms transit_terms(Transit const &k,
                        std::vector<std::vector<scav_point>> const &lines) {
  return cost_terms(k.c, decompose(k.c), k.z, routes_of(k.c, lines), {}, profile());
}

}  // namespace

TEST_CASE("cost: a crossing route's bend in the common ancestor costs nothing extra") {
  Transit const k{ transit_chart() };
  CostTerms const t{ transit_terms(k, { jog_at(150) }) };
  CHECK(t.bends == 2);
  CHECK(t.transit_bends == 0);
  // A bend on Arm's border is outside Arm.
  CHECK(transit_terms(k, { jog_at(200) }).transit_bends == 0);
}

TEST_CASE("cost: a crossing route's bend in a state it only passes through costs") {
  Transit const k{ transit_chart() };
  CostTerms const t{ transit_terms(k, { jog_at(250) }) };
  CHECK(t.bends == 2);
  CHECK(t.transit_bends == 2);
  // Each transit bend adds `w_transit_bends` to t2.
  scav_profile const p{ profile() };
  CHECK((cost_of(t, p).t2 - cost_of(transit_terms(k, { jog_at(150) }), p).t2) ==
        (2 * int64_t{ p.w_transit_bends }));

  // The same from the source's end: Dst -> Src along the route reversed.
  auto const reversed = [](std::vector<scav_point> const &line) {
    return std::vector<scav_point>{ line.rbegin(), line.rend() };
  };
  CHECK(transit_terms(k, { {}, reversed(jog_at(250)) }).transit_bends == 2);
  CHECK(transit_terms(k, { {}, reversed(jog_at(150)) }).transit_bends == 0);
}

TEST_CASE("cost: a crossing route's bend in an end's own machine costs nothing extra") {
  Transit const k{ transit_chart() };
  CostTerms const t{ transit_terms(k, { jog_at(350) }) };
  CHECK(t.bends == 2);
  CHECK(t.transit_bends == 0);
  // One bend inside Dst itself.
  CHECK(transit_terms(k,
                      { { { .x = 100, .y = 130 },
                          { .x = 450, .y = 130 },
                          { .x = 450, .y = 230 },
                          { .x = 400, .y = 230 } } })
            .transit_bends == 0);
  // Src -> Arm: its bends inside Arm are inside its own end.
  CHECK(transit_terms(k,
                      { {},
                        {},
                        { { .x = 100, .y = 130 },
                          { .x = 250, .y = 130 },
                          { .x = 250, .y = 20 },
                          { .x = 260, .y = 20 } } })
            .transit_bends == 0);
}

TEST_CASE("cost: two routes that properly cross count once") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const e{ build_state(c, root, "E", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, d, e, TransKind::Default, {});

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

// `n` transitions from A to B.
Chart edges(uint32_t n) {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  for (uint32_t i = 0; i < n; ++i) { build_trans(c, a, b, TransKind::Default, {}); }
  return c;
}

int64_t corridor_of(Chart const &c, std::vector<std::vector<scav_point>> const &lines) {
  SizedLayout const z{ blank(c) };
  return cost_terms(c, decompose(c), z, routes_of(c, lines), {}, profile()).corridor;
}

}  // namespace

TEST_CASE(
    "cost: party marks a bent route, both of a crossing, and neither of a clear one") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  for (uint32_t i = 0; i < 4; ++i) { build_trans(c, a, b, TransKind::Default, {}); }

  SizedLayout const z{ blank(c) };
  Routes const r{ routes_of(
      c,
      { { { .x = 0, .y = 0 }, { .x = 100, .y = 0 }, { .x = 100, .y = 100 } },
        { { .x = 300, .y = 0 }, { .x = 400, .y = 100 } },
        { { .x = 300, .y = 100 }, { .x = 400, .y = 0 } },
        { { .x = 1000, .y = 1000 }, { .x = 1100, .y = 1000 } } }) };
  SplitGraph const g{ decompose(c) };
  std::vector<uint8_t> party;
  CostTerms const t{ cost_terms(cost_context(c, g), c, g, z, r, {}, profile(), &party) };
  CHECK(t.crossings == 1);
  CHECK(t.bends == 1);
  REQUIRE(party.size() == 4);
  CHECK(party[0] != 0);
  CHECK(party[1] != 0);
  CHECK(party[2] != 0);
  CHECK(party[3] == 0);

  // Straight routes on one line, then half an em apart, mark both of each pair.
  Chart const four{ edges(4) };
  Routes const shared{ routes_of(
      four,
      { { { .x = 0, .y = 60 }, { .x = 500, .y = 60 } },
        { { .x = 100, .y = 60 }, { .x = 400, .y = 60 } },
        { { .x = 0, .y = 1000 }, { .x = 1000, .y = 1000 } },
        { { .x = 0, .y = 1096 }, { .x = 1000, .y = 1096 } } }) };
  SplitGraph const g4{ decompose(four) };
  CostTerms const u{ cost_terms(cost_context(four, g4),
                                four,
                                g4,
                                blank(four),
                                shared,
                                {},
                                profile(),
                                &party) };
  CHECK(u.bends == 0);
  CHECK(u.crossings == 0);
  CHECK(u.excess_len == 0);
  CHECK(u.corridor == 300);
  CHECK(u.crowding > 0);
  REQUIRE(party.size() == 4);
  CHECK(party[0] != 0);
  CHECK(party[1] != 0);
  CHECK(party[2] != 0);
  CHECK(party[3] != 0);
}

TEST_CASE("cost: charge is each transition's weighted terms, a pair's to both of it") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  for (uint32_t i = 0; i < 4; ++i) { build_trans(c, a, b, TransKind::Default, {}); }
  scav_profile const p{ profile() };
  Wide const em{ p.font_size_grid };

  // A bent route, two crossing diagonals, and a clear route.
  Routes const r{ routes_of(
      c,
      { { { .x = 0, .y = 0 }, { .x = 100, .y = 0 }, { .x = 100, .y = 100 } },
        { { .x = 300, .y = 0 }, { .x = 400, .y = 100 } },
        { { .x = 300, .y = 100 }, { .x = 400, .y = 0 } },
        { { .x = 1000, .y = 1000 }, { .x = 1100, .y = 1000 } } }) };
  SplitGraph const g{ decompose(c) };
  std::vector<uint8_t> party;
  std::vector<Wide> charge;
  (void)cost_terms(cost_context(c, g), c, g, blank(c), r, {}, p, &party, &charge);
  REQUIRE(charge.size() == 4);
  // The bend, and 200 run where the direct distance is 141.
  CHECK(charge[0] == (Wide{ p.w_bends } * em) + (Wide{ p.w_excess_len } * (200 - 141)));
  CHECK(charge[1] == Wide{ p.w_crossings } * em);
  CHECK(charge[2] == Wide{ p.w_crossings } * em);
  CHECK(charge[3] == 0);

  // A run of 300 shared on one line, then two routes half an em apart over 1000.
  Routes const shared{ routes_of(
      c,
      { { { .x = 0, .y = 60 }, { .x = 500, .y = 60 } },
        { { .x = 100, .y = 60 }, { .x = 400, .y = 60 } },
        { { .x = 0, .y = 1000 }, { .x = 1000, .y = 1000 } },
        { { .x = 0, .y = 1000 + (p.font_size_grid / 2) },
          { .x = 1000, .y = 1000 + (p.font_size_grid / 2) } } }) };
  CostTerms const t{
    cost_terms(cost_context(c, g), c, g, blank(c), shared, {}, p, &party, &charge)
  };
  CHECK(t.corridor == 300);
  CHECK(t.crowding == 500);
  CHECK(charge[0] == Wide{ p.w_corridor } * 300);
  CHECK(charge[1] == Wide{ p.w_corridor } * 300);
  CHECK(charge[2] == Wide{ p.w_crowding } * 500);
  CHECK(charge[3] == Wide{ p.w_crowding } * 500);
  for (uint32_t i = 0; i < 4; ++i) { CHECK((party[i] != 0) == (charge[i] != 0)); }
}

TEST_CASE("cost: party marks every transition while the drawing breaks Tier 0") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 20, .h = 2000 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 20, .h = 2000 };
  z.state[other.v] = { .x = 150, .y = 950, .w = 100, .h = 100 };
  Routes const r{ routes_of(c,
                            { { { .x = 20, .y = 1000 }, { .x = 400, .y = 1000 } },
                              { { .x = 20, .y = 1500 }, { .x = 400, .y = 1500 } } }) };
  SplitGraph const g{ decompose(c) };
  std::vector<uint8_t> party;
  CostTerms t{ cost_terms(cost_context(c, g), c, g, z, r, {}, profile(), &party) };
  CHECK(t.through_box == 1);
  REQUIRE(party.size() == 2);
  CHECK(party[0] != 0);
  CHECK(party[1] != 0);

  // X moved clear: no Tier-0 count, and neither route is marked.
  z.state[other.v] = { .x = 150, .y = 3000, .w = 100, .h = 100 };
  t = cost_terms(cost_context(c, g), c, g, z, r, {}, profile(), &party);
  CHECK(cost_of(t, profile()).t0_violations == 0);
  REQUIRE(party.size() == 2);
  CHECK(party[0] == 0);
  CHECK(party[1] == 0);
}

TEST_CASE("cost: two routes ending as one line are not charged for the run they share") {
  Chart const c{ edges(2) };
  // Both turn onto x=500 and end at one point; the 20 they share there costs nothing.
  CHECK(corridor_of(
            c,
            { { { .x = 0, .y = 60 }, { .x = 500, .y = 60 }, { .x = 500, .y = 100 } },
              { { .x = 0, .y = 80 }, { .x = 500, .y = 80 }, { .x = 500, .y = 100 } } }) ==
        0);

  // The second ends one unit past the first's end; the 20 they share on x=500 is charged.
  CHECK(corridor_of(
            c,
            { { { .x = 0, .y = 60 }, { .x = 500, .y = 60 }, { .x = 500, .y = 100 } },
              { { .x = 0, .y = 80 }, { .x = 500, .y = 80 }, { .x = 500, .y = 101 } } }) ==
        20);
}

TEST_CASE("cost: a shared endpoint alone exempts nothing") {
  Chart const c{ edges(2) };
  // Both end at (200,300) at right angles; the 100 they share along y=100 is charged.
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
  // The 200 shared along y=0 is charged; the 20 shared on x=500 only when the ends differ.
  CHECK(corridor_of(c, lines(100)) == 200);
  CHECK(corridor_of(c, lines(101)) == 220);
}

TEST_CASE("cost: two routes leaving as one line are charged for it") {
  Chart const c{ edges(2) };
  auto const lines = [](int32_t start_x) {
    return std::vector<std::vector<scav_point>>{
      { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
      { { .x = start_x, .y = 0 }, { .x = 150, .y = 0 }, { .x = 150, .y = -100 } }
    };
  };
  // Routes leaving one state on one line are charged: 150 from a shared start,
  // 149 with the starts one unit apart.
  CHECK(corridor_of(c, lines(0)) == 150);
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

TEST_CASE("cost: a route arriving as another's last leg is trunk, its head is not") {
  Chart const c{ edges(2) };
  // The second route is the first's final leg alone: trunk, uncharged.
  CHECK(corridor_of(c,
                    { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
                      { { .x = 200, .y = 0 }, { .x = 200, .y = 100 } } }) == 0);
  // The second route is the first's opening leg alone; the 200 they share on y=0 is
  // charged.
  CHECK(corridor_of(c,
                    { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
                      { { .x = 0, .y = 0 }, { .x = 200, .y = 0 } } }) == 200);
}

TEST_CASE("cost: a run the two find again after they part is charged") {
  Chart const c{ edges(2) };
  // The 100 shared along y=0 from the common start and the 100 shared along y=100
  // where they meet again are both charged.
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
                        { .x = 500, .y = 100 } } }) == 200);
}

TEST_CASE("cost: a run against the trunk is charged and the trunk is not") {
  Chart const c{ edges(2) };
  // The second runs 50 along the trunk at x=400 before looping round to join it;
  // that 50 is charged.
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

TEST_CASE("cost: two routes on one line with different ends are a Tier-0 shared run") {
  Chart const c{ edges(2) };
  SizedLayout const z{ blank(c) };
  auto const terms = [&c, &z](std::vector<std::vector<scav_point>> const &lines) {
    return cost_terms(c, decompose(c), z, routes_of(c, lines), {}, profile());
  };
  // Different starts and ends, 100 shared along x=200.
  CostTerms const apart{ terms({ { { .x = 0, .y = 0 },
                                   { .x = 200, .y = 0 },
                                   { .x = 200, .y = 300 },
                                   { .x = 400, .y = 300 } },
                                 { { .x = 0, .y = 100 },
                                   { .x = 200, .y = 100 },
                                   { .x = 200, .y = 200 },
                                   { .x = 400, .y = 200 } } }) };
  CHECK(apart.shared_run == 1);
  CHECK(apart.corridor == 100);
  CHECK(cost_of(apart, profile()).t0_violations == 1);

  // A fan-in along its final leg.
  CostTerms const fan_in{ terms(
      { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 300 } },
        { { .x = 100, .y = 100 }, { .x = 200, .y = 100 }, { .x = 200, .y = 300 } } }) };
  CHECK(fan_in.shared_run == 0);
  CHECK(fan_in.corridor == 0);

  // A fan-out along its first leg: priced by `corridor`, permitted by Tier 0.
  CostTerms const fan_out{ terms(
      { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
        { { .x = 0, .y = 0 }, { .x = 100, .y = 0 }, { .x = 100, .y = 100 } } }) };
  CHECK(fan_out.shared_run == 0);
  CHECK(fan_out.corridor == 100);
  CHECK(cost_of(fan_out, profile()).t0_violations == 0);
}

TEST_CASE("cost: a degraded net's diagonal is no one's merge leg") {
  Chart const c{ edges(2) };
  // Only the shared final leg on x=400 is trunk; the 200 shared along y=200 is charged.
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
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  // Straight: length equals the direct distance, excess 0.
  Routes const direct{ routes_of(c, { { { .x = 0, .y = 0 }, { .x = 300, .y = 0 } } }) };
  CHECK(cost_terms(c, decompose(c), z, direct, {}, profile()).excess_len == 0);

  // A detour 100 off the line over a 300 span is charged its added length.
  Routes const around{
    routes_of(c, { { { .x = 0, .y = 0 }, { .x = 150, .y = 100 }, { .x = 300, .y = 0 } } })
  };
  CostTerms const t{ cost_terms(c, decompose(c), z, around, {}, profile()) };
  CHECK(t.excess_len == ((2 * 180) - 300));  // isqrt(150^2 + 100^2) is 180
}

TEST_CASE("cost: length is every route end to end, once, crossed or not") {
  Chart const c{ edges(2) };
  SizedLayout const z{ blank(c) };
  Routes const one{ routes_of(c, { { { .x = 0, .y = 0 }, { .x = 300, .y = 0 } } }) };
  CostTerms const alone{ cost_terms(c, decompose(c), z, one, {}, profile()) };
  CHECK(alone.excess_len == 0);
  CHECK(alone.length == 300);

  // A second route of three legs, crossing the first.
  Routes const two{ routes_of(c,
                              { { { .x = 0, .y = 0 }, { .x = 300, .y = 0 } },
                                { { .x = 100, .y = -100 },
                                  { .x = 100, .y = 100 },
                                  { .x = 200, .y = 100 },
                                  { .x = 200, .y = 50 } } }) };
  CostTerms const both{ cost_terms(c, decompose(c), z, two, {}, profile()) };
  REQUIRE(both.crossings == 1);
  CHECK(both.excess_len == 2 * ((200 + 100 + 50) - 180));  // isqrt(100^2 + 150^2)
  CHECK(both.length == 300 + (200 + 100 + 50));
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
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 20, .h = 20 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 20, .h = 20 };
  z.state[other.v] = { .x = 150, .y = -50, .w = 100, .h = 100 };
  Routes const r{ routes_of(c, { { { .x = 10, .y = 10 }, { .x = 410, .y = 10 } } }) };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).through_box == 1);

  // X moved clear of the route; the route's own endpoint boxes are carved out.
  z.state[other.v] = { .x = 150, .y = 500, .w = 100, .h = 100 };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).through_box == 0);
}

TEST_CASE(
    "cost: a route along a state's border is a Tier-0 violation, square off it is not") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.state[other.v] = { .x = 200, .y = 20, .w = 100, .h = 100 };
  // Along X's top edge.
  Routes const along{ routes_of(c, { { { .x = 100, .y = 20 }, { .x = 400, .y = 20 } } }) };
  CostTerms const t{ cost_terms(c, decompose(c), z, along, {}, profile()) };
  CHECK(t.through_box == 0);
  CHECK(t.flush == 1);
  CHECK(cost_of(t, profile()).t0_violations == 1);

  // A route `border_band` from the border is clear; one unit nearer is flush.
  int32_t const band{ border_band(profile()) };
  REQUIRE(band > 1);
  Routes const clear{
    routes_of(c, { { { .x = 100, .y = 20 - band }, { .x = 400, .y = 20 - band } } })
  };
  CHECK(cost_terms(c, decompose(c), z, clear, {}, profile()).flush == 0);
  Routes const near{
    routes_of(c, { { { .x = 100, .y = 21 - band }, { .x = 400, .y = 21 - band } } })
  };
  CHECK(cost_terms(c, decompose(c), z, near, {}, profile()).flush == 1);

  // A run along its own endpoint's border is flush as well.
  Routes const own{ routes_of(c,
                              { { { .x = 50, .y = 0 },
                                  { .x = 100, .y = 0 },
                                  { .x = 150, .y = 0 },
                                  { .x = 150, .y = 20 - band },
                                  { .x = 400, .y = 20 - band } } }) };
  CHECK(cost_terms(c, decompose(c), z, own, {}, profile()).flush == 1);
}

TEST_CASE(
    "cost: flush counts a run nearer than pad to a border, inside the state or out") {
  // `Box` holds A and B; `A -> B` runs along Box's top border at each offset from it,
  // negative above it and positive inside it.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const box{ build_state(c, root, "Box", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, box, {}, {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  scav_profile const p{ profile() };
  SizedLayout z{ blank(c) };
  z.state[box.v] = { .x = 0, .y = 0, .w = 2000, .h = 1000 };
  z.sub[inner.v] = { .x = p.pad,
                     .y = p.pad,
                     .w = 2000 - (2 * p.pad),
                     .h = 1000 - (2 * p.pad) };
  z.state[a.v] = { .x = 400, .y = 600, .w = 200, .h = 200 };
  z.state[b.v] = { .x = 1400, .y = 600, .w = 200, .h = 200 };
  for (int32_t const off : { -p.pad, 1 - p.pad, p.pad - 1, p.pad }) {
    CAPTURE(off);
    Routes const r{ routes_of(c,
                              { { { .x = 500, .y = 600 },
                                  { .x = 500, .y = off },
                                  { .x = 1500, .y = off },
                                  { .x = 1500, .y = 600 } } }) };
    bool const near{ (off > -p.pad) && (off < p.pad) };
    CHECK(cost_terms(c, decompose(c), z, r, {}, p).flush == (near ? 1 : 0));
  }
}

TEST_CASE("cost: a route that turns straight back along itself is a Tier-0 violation") {
  // A leg that reverses along the previous leg is a retrace; a square turn and a U
  // through a jog are not.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };

  Routes const back{ routes_of(c,
                               { { { .x = 100, .y = 20 },
                                   { .x = 250, .y = 20 },
                                   { .x = 250, .y = 300 },
                                   { .x = 250, .y = 200 },
                                   { .x = 400, .y = 200 } } }) };
  CostTerms const t{ cost_terms(c, decompose(c), z, back, {}, profile()) };
  CHECK(t.retrace == 1);
  CHECK(cost_of(t, profile()).t0_violations == 1);

  Routes const turns{ routes_of(c,
                                { { { .x = 100, .y = 20 },
                                    { .x = 250, .y = 20 },
                                    { .x = 250, .y = 300 },
                                    { .x = 200, .y = 300 },
                                    { .x = 200, .y = 200 },
                                    { .x = 400, .y = 200 } } }) };
  CHECK(cost_terms(c, decompose(c), z, turns, {}, profile()).retrace == 0);
}

TEST_CASE("cost: a route that crosses itself is a Tier-0 violation") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  scav_profile p{ profile() };
  p.pad = 8;  // under every gap between these routes and the boxes
  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };

  // Right, down, left, then up across the first leg.
  Routes const knot{ routes_of(c,
                               { { { .x = 100, .y = 20 },
                                   { .x = 300, .y = 20 },
                                   { .x = 300, .y = 100 },
                                   { .x = 200, .y = 100 },
                                   { .x = 200, .y = -50 },
                                   { .x = 450, .y = -50 },
                                   { .x = 450, .y = 0 } } }) };
  CostTerms const t{ cost_terms(c, decompose(c), z, knot, {}, p) };
  CHECK(t.self_crossing == 1);
  CHECK(t.retrace == 0);
  CHECK(cost_of(t, p).t0_violations == 1);

  Routes const square{ routes_of(c,
                                 { { { .x = 100, .y = 20 },
                                     { .x = 300, .y = 20 },
                                     { .x = 300, .y = 100 },
                                     { .x = 450, .y = 100 },
                                     { .x = 450, .y = 40 } } }) };
  CHECK(cost_terms(c, decompose(c), z, square, {}, p).self_crossing == 0);
}

TEST_CASE("cost: an external route between two regions that crosses their divider") {
  // `On` holds regions `main` and `aux` side by side; `A` in `main` goes to `B` in `aux`.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const main_sub{ build_submachine(c, on, "main", {}) };
  SubmachineId const aux_sub{ build_submachine(c, on, "aux", {}) };
  StateId const a{ build_state(c, main_sub, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, aux_sub, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[on.v] = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.sub[main_sub.v] = { .x = 10, .y = 10, .w = 180, .h = 180 };
  z.sub[aux_sub.v] = { .x = 210, .y = 10, .w = 180, .h = 180 };
  z.state[a.v] = { .x = 40, .y = 60, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 240, .y = 140, .w = 100, .h = 40 };
  auto const regions = [&](Routes const &r) {
    return cost_terms(c, decompose(c), z, r, {}, profile()).through_region;
  };

  // Out of `On` on the left, back in on the left, then across `main` and the divider.
  Routes const across{ routes_of(c,
                                 { { { .x = 40, .y = 80 },
                                     { .x = -50, .y = 80 },
                                     { .x = -50, .y = 160 },
                                     { .x = 240, .y = 160 } } }) };
  CHECK(regions(across) == 1);
  // Out of `main` downward, round below `On` and up into `aux`.
  CHECK(regions(routes_of(c,
                          { { { .x = 90, .y = 100 },
                              { .x = 90, .y = 260 },
                              { .x = 290, .y = 260 },
                              { .x = 290, .y = 180 } } })) == 0);
  // A transition of the default kind crosses the divider by design.
  c.transitions[0].kind = TransKind::Default;
  CHECK(regions(across) == 0);
}

TEST_CASE("cost: a route through a region neither end is in is a Tier-0 violation") {
  // `On` holds regions `main` and `aux`; `Ready` in `main` goes to `X` outside.
  // Crossing `aux` is a `through_region` violation; `through_box` skips ancestor `On`.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const main_sub{ build_submachine(c, on, "main", {}) };
  SubmachineId const aux_sub{ build_submachine(c, on, "aux", {}) };
  StateId const ready{ build_state(c, main_sub, "Ready", StateKind::Normal, {}) };
  StateId const idle{ build_state(c, aux_sub, "Idle", StateKind::Normal, {}) };
  StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, ready, x, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[on.v] = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.sub[main_sub.v] = { .x = 10, .y = 10, .w = 180, .h = 180 };
  z.sub[aux_sub.v] = { .x = 210, .y = 10, .w = 180, .h = 180 };
  z.state[ready.v] = { .x = 40, .y = 60, .w = 100, .h = 40 };
  z.state[idle.v] = { .x = 240, .y = 140, .w = 100, .h = 40 };
  z.state[x.v] = { .x = 600, .y = 60, .w = 100, .h = 40 };

  // Straight out through `aux`, missing `Idle` itself.
  Routes const through{ routes_of(c,
                                  { { { .x = 140, .y = 80 }, { .x = 600, .y = 80 } } }) };
  scav_profile ring{ profile() };
  ring.pad = 10;  // the fixture's ring
  CostTerms const t{ cost_terms(c, decompose(c), z, through, {}, ring) };
  CHECK(t.through_box == 0);
  CHECK(t.through_region == 1);
  CHECK(cost_of(t, ring).t0_violations == 1);

  // Out of `main` downward and round below `On`: its own region, then outside.
  Routes const round{ routes_of(c,
                                { { { .x = 90, .y = 100 },
                                    { .x = 90, .y = 260 },
                                    { .x = 650, .y = 260 },
                                    { .x = 650, .y = 100 } } }) };
  CHECK(cost_terms(c, decompose(c), z, round, {}, profile()).through_region == 0);

  // `On` made a child of its own region `main`: the climb from `Ready` cycles until the
  // step cap ends it.
  c.states[on.v].parent = main_sub;
  CHECK(cost_terms(c, decompose(c), z, through, {}, profile()).through_region == 1);
}

TEST_CASE("cost: a route along its owner's border beside a sibling region crosses it") {
  // `On` stacks `main` over `aux`; `Ready` in `main` goes to `X`, level with `aux`.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const main_sub{ build_submachine(c, on, "main", {}) };
  SubmachineId const aux_sub{ build_submachine(c, on, "aux", {}) };
  StateId const ready{ build_state(c, main_sub, "Ready", StateKind::Normal, {}) };
  StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, ready, x, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[on.v] = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.sub[main_sub.v] = { .x = 20, .y = 10, .w = 360, .h = 80 };
  z.sub[aux_sub.v] = { .x = 20, .y = 110, .w = 360, .h = 80 };
  z.state[ready.v] = { .x = 60, .y = 30, .w = 100, .h = 40 };
  z.state[x.v] = { .x = -300, .y = 130, .w = 100, .h = 40 };
  auto const regions = [&](Routes const &r) {
    return cost_terms(c, decompose(c), z, r, {}, profile()).through_region;
  };

  // Down the pad ring past the divider, outside both region rects, then out: two pieces.
  CHECK(regions(routes_of(c,
                          { { { .x = 60, .y = 50 },
                              { .x = 10, .y = 50 },
                              { .x = 10, .y = 150 },
                              { .x = -200, .y = 150 } } })) == 2);
  // Out of `On` level with `main`, then down outside it.
  CHECK(regions(routes_of(c,
                          { { { .x = 60, .y = 50 },
                              { .x = -100, .y = 50 },
                              { .x = -100, .y = 150 },
                              { .x = -200, .y = 150 } } })) == 0);
}

TEST_CASE("cost: a region is tested where the descent reaches its owner") {
  // `Shell` holds `On`, which holds regions `main` and `aux`; `Ready` in `main` goes to
  // `X` outside.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const shell{ build_state(c, root, "Shell", StateKind::Normal, {}) };
  SubmachineId const in{ build_submachine(c, shell, "in", {}) };
  StateId const on{ build_state(c, in, "On", StateKind::Normal, {}) };
  SubmachineId const main_sub{ build_submachine(c, on, "main", {}) };
  SubmachineId const aux_sub{ build_submachine(c, on, "aux", {}) };
  StateId const ready{ build_state(c, main_sub, "Ready", StateKind::Normal, {}) };
  StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, ready, x, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[shell.v] = { .x = -10, .y = -10, .w = 420, .h = 220 };
  z.sub[in.v] = { .x = -5, .y = -5, .w = 410, .h = 210 };
  z.state[on.v] = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.sub[main_sub.v] = { .x = 10, .y = 10, .w = 180, .h = 180 };
  z.sub[aux_sub.v] = { .x = 210, .y = 10, .w = 180, .h = 180 };
  z.state[ready.v] = { .x = 40, .y = 60, .w = 100, .h = 40 };
  z.state[x.v] = { .x = 600, .y = 60, .w = 100, .h = 40 };
  auto const regions = [&](Routes const &r) {
    return cost_terms(c, decompose(c), z, r, {}, profile()).through_region;
  };
  Routes const through{ routes_of(c,
                                  { { { .x = 140, .y = 80 }, { .x = 600, .y = 80 } } }) };
  Routes const below{ routes_of(c,
                                { { { .x = 90, .y = 100 },
                                    { .x = 90, .y = 300 },
                                    { .x = 650, .y = 300 },
                                    { .x = 650, .y = 100 } } }) };
  CHECK(regions(through) == 1);
  CHECK(regions(below) == 0);

  // `aux` hanging 200 below its owner is entered at y 300 by a piece that misses `On`.
  z.sub[aux_sub.v].h = 380;
  CHECK(regions(below) == 0);
  z.sub[aux_sub.v].h = 180;

  // Under a dead `Shell`, `On` is detached and its regions are tested outright.
  c.states[shell.v].live = 0;
  CHECK(regions(through) == 1);

  // A dead owner's regions are tested by none.
  c.states[shell.v].live = 1;
  c.states[on.v].live = 0;
  CHECK(regions(through) == 0);
}

TEST_CASE("cost: a placed box over a state neither endpoint is under breaks Tier 0") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 100 };
  z.state[other.v] = { .x = 200, .y = -150, .w = 100, .h = 100 };
  Routes r{ routes_of(c, { { { .x = 100, .y = 50 }, { .x = 400, .y = 50 } } }) };
  r.placed = { { .x = 200, .y = -60, .w = 60, .h = 20 } };  // 90 above its route
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  scav_profile near{ profile() };
  near.pad = 16;  // under the route's 100-unit gap to X
  CostTerms const over{ cost_terms(c, decompose(c), z, r, s, near) };
  CHECK(over.label_over_box == 1);
  CHECK(over.label == 0);
  CHECK(cost_of(over, near).t0_violations == 1);

  z.state[other.v] = { .x = 200, .y = 500, .w = 100, .h = 100 };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label_over_box == 0);

  r.placed = { { .x = 410, .y = 10, .w = 60, .h = 20 } };  // over its own leaf endpoint
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label_over_box == 1);
}

TEST_CASE("cost: inside the composite it runs in, only the text bands cost") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

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
  CostTerms const band{ cost_terms(c, decompose(c), z, r, s, profile()) };
  CHECK(band.label == 1);
  CHECK(band.label_over_box == 0);
}

TEST_CASE(
    "cost: a composite endpoint enclosing the other end holds the box, bar its bands") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const away{ build_state(c, root, "Away", StateKind::Normal, {}) };
  build_trans(c, outer, a, TransKind::Default, {});
  build_trans(c, a, outer, TransKind::Default, {});
  build_trans(c, outer, away, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 600, .h = 200 };
  z.before[outer.v] = { .x = 10, .y = 10, .w = 580, .h = 30 };
  z.state[a.v] = { .x = 50, .y = 80, .w = 100, .h = 60 };
  z.state[away.v] = { .x = 900, .y = 80, .w = 100, .h = 60 };
  Routes r{ routes_of(c,
                      { { { .x = 600, .y = 150 }, { .x = 100, .y = 150 } },
                        { { .x = 100, .y = 80 }, { .x = 100, .y = 60 } },
                        { { .x = 600, .y = 110 }, { .x = 900, .y = 110 } } }) };
  auto const over = [&](uint32_t subject, scav_rect at) {
    scav_path_box const box{ .subject = subject, .w = at.w, .h = at.h, .order = 0 };
    scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
    r.placed = { at };
    return cost_terms(c, decompose(c), z, r, s, profile()).label_over_box;
  };
  CHECK(over(0, { .x = 300, .y = 160, .w = 60, .h = 20 }) == 0);  // inside Outer, into A
  CHECK(over(1, { .x = 300, .y = 160, .w = 60, .h = 20 }) == 0);  // inside Outer, out of A
  CHECK(over(0, { .x = 300, .y = 15, .w = 60, .h = 20 }) == 1);   // Outer's title band
  CHECK(over(0, { .x = 60, .y = 100, .w = 60, .h = 20 }) == 1);   // A, a leaf endpoint
  CHECK(over(2, { .x = 500, .y = 120, .w = 60, .h = 20 }) == 1);  // Outer, to a sibling
}

TEST_CASE("cost: a placed box over another transition's route breaks Tier 0") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, a, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 100 };
  Routes r{ routes_of(c,
                      { { { .x = 100, .y = 50 }, { .x = 400, .y = 50 } },
                        { { .x = 400, .y = 80 }, { .x = 100, .y = 80 } } }) };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };

  // Straddling either route counts: its own (y=50) or the other (y=80).
  r.placed = { { .x = 200, .y = 40, .w = 60, .h = 20 } };
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label_over_route == 1);

  r.placed = { { .x = 200, .y = 70, .w = 60, .h = 20 } };
  CostTerms const over{ cost_terms(c, decompose(c), z, r, s, profile()) };
  CHECK(over.label_over_route == 1);
  CHECK(over.label == 0);
  CHECK(cost_of(over, profile()).t0_violations == 1);

  r.placed = { { .x = 200, .y = 82, .w = 60, .h = 20 } };  // 2 units below the other route
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label_over_route == 0);
}

TEST_CASE("cost: a box nearer a foreign route than its own is charged the shortfall") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, a, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 200 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 200 };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  // The box sits on its own leg at y=150; the cases vary only the foreign leg's y.
  auto const scored = [&](int32_t foreign_y) {
    Routes r{ routes_of(
        c,
        { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
          { { .x = 400, .y = foreign_y }, { .x = 100, .y = foreign_y } } }) };
    r.placed = { { .x = 200, .y = 130, .w = 60, .h = 20 } };
    return cost_terms(c, decompose(c), z, r, s, profile()).label_near;
  };

  CHECK(scored(120) == 10);  // foreign leg 10 above the box: shortfall 20 - 10
  CHECK(scored(110) == 0);   // one box height away, the required margin
  CHECK(scored(90) == 0);

  // Strip 1: own leg 20 below the box, foreign leg 20 above it; shortfall 20 + 20 - 20.
  Routes strip1{ routes_of(c,
                           { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } },
                             { { .x = 400, .y = 90 }, { .x = 100, .y = 90 } } }) };
  strip1.placed = { { .x = 200, .y = 110, .w = 60, .h = 20 } };
  CHECK(cost_terms(c, decompose(c), z, strip1, s, profile()).label_near == 20);
}

TEST_CASE("cost: a placed box past the leader from its own route breaks Tier 0") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 300, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 300, .w = 100, .h = 100 };
  Routes r{ routes_of(c, { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };
  int32_t const leader{ label_leader(profile()) };
  auto const scored = [&](scav_rect at) {
    r.placed = { at };
    return cost_terms(c, decompose(c), z, r, s, profile());
  };
  auto const above = [](int32_t gap) {  // `gap` above the route
    return scav_rect{ .x = 200, .y = 130 - gap, .w = 60, .h = 20 };
  };

  CHECK(scored(above(leader - 1)).label_far == 0);
  CHECK(scored(above(leader)).label_far == 0);
  CostTerms const far{ scored(above(leader + 1)) };
  CHECK(far.label_far == 1);
  CHECK(cost_of(far, profile()).t0_violations == 1);
  // Past the route's end, and diagonal off it: the gap is Chebyshev.
  CHECK(scored({ .x = 401 + leader, .y = 140, .w = 60, .h = 20 }).label_far == 1);
  CHECK(scored({ .x = 400 + leader, .y = 130 - leader, .w = 60, .h = 20 }).label_far == 0);
  // An unnamed box owns no route and counts 0.
  r.placed = { above(10 * leader) };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).label_far == 0);
}

TEST_CASE("cost: with no other route to be near, no box is charged") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

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
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  Routes r{ routes_of(c, { { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  r.placed = { { .x = 200, .y = 140, .w = 60, .h = 20 } };  // over that one line
  auto const scored = [&](scav_spaces const &s) {
    return cost_terms(c, decompose(c), z, r, s, profile());
  };

  // Named or unnamed, a line through the box counts. With no leg of its own,
  // `label_near` is 0.
  scav_path_box const named{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  CHECK(scored({ .path_box = &named, .n_path_box = 1 }).label_over_route == 1);
  CHECK(scored({}).label_over_route == 1);
  CHECK(scored({}).label_near == 0);

  // A table too short to reach the box, or naming a missing transition, also
  // leaves the line foreign.
  CHECK(scored({ .path_box = &named, .n_path_box = 0 }).label_over_route == 1);
  scav_path_box const past{ .subject = 7, .w = 60, .h = 20, .order = 0 };
  CHECK(scored({ .path_box = &past, .n_path_box = 1 }).label_over_route == 1);
}

TEST_CASE("cost: a box whose own transition has no route is charged nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::Internal, {});  // no route: drawn in A's rect
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  // The box belongs to the routeless transition and sits near the other's line.
  Routes r{ routes_of(c, { {}, { { .x = 100, .y = 150 }, { .x = 400, .y = 150 } } }) };
  r.placed = { { .x = 200, .y = 120, .w = 60, .h = 20 } };
  scav_path_box const box{ .subject = 0, .w = 60, .h = 20, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };

  CostTerms const t{ cost_terms(c, decompose(c), z, r, s, profile()) };
  CHECK(t.label_near == 0);
  CHECK(t.label == 0);

  r.placed = { { .x = 200, .y = -2000, .w = 60, .h = 20 } };  // far from every route
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label_far == 0);
}

TEST_CASE("cost: two placed boxes over each other are one label cost") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

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
  // Crossings score as a count; 1000 square grid units round up to one em squared.
  CHECK(scored.t2 == ((int64_t{ p.w_crossings } * 2) + int64_t{ p.w_area }));

  // Tiers compare lexicographically: one Tier-0 violation outranks any Tier-2 sum.
  CostTerms violating;
  violating.box_overlap = 1;
  Cost const bad{ cost_of(violating, p) };
  CHECK(cost_less(scored, bad));
  CHECK_FALSE(cost_less(bad, scored));
}

TEST_CASE("cost: every Tier-2 term is scored in the unit the profile names it") {
  scav_profile const p{ profile() };
  int64_t const em{ p.font_size_grid };
  auto const only = [&p](int64_t CostTerms::*term, int64_t v) {
    CostTerms t;
    t.*term = v;
    return cost_of(t, p).t2;
  };

  // Counts score as is; lengths convert to ems and areas to ems squared, rounding up.
  CHECK(only(&CostTerms::bends, 1) == int64_t{ p.w_bends });
  CHECK(only(&CostTerms::crossings, 1) == int64_t{ p.w_crossings });
  CHECK(only(&CostTerms::adjacency, 1) == int64_t{ p.w_adjacency });
  CHECK(only(&CostTerms::label, 1) == int64_t{ p.w_label });
  CHECK(only(&CostTerms::corridor, 1) == int64_t{ p.w_corridor });
  CHECK(only(&CostTerms::excess_len, 1) == int64_t{ p.w_excess_len });
  CHECK(only(&CostTerms::label_near, 1) == int64_t{ p.w_label_near });
  CHECK(only(&CostTerms::aspect, 1) == int64_t{ p.w_aspect });
  CHECK(only(&CostTerms::area, 1) == int64_t{ p.w_area });
  CHECK(only(&CostTerms::crowding, 1) == int64_t{ p.w_crowding });
  CHECK(only(&CostTerms::length, 1) == int64_t{ p.w_length });
  CHECK(only(&CostTerms::transit_bends, 1) == int64_t{ p.w_transit_bends });
  CHECK(only(&CostTerms::whitespace, 1) == int64_t{ p.w_whitespace });

  // One em, or one em squared, costs one unit; one grid unit more costs two.
  CHECK(only(&CostTerms::corridor, em) == int64_t{ p.w_corridor });
  CHECK(only(&CostTerms::corridor, em + 1) == (2 * int64_t{ p.w_corridor }));
  CHECK(only(&CostTerms::length, em) == int64_t{ p.w_length });
  CHECK(only(&CostTerms::length, em + 1) == (2 * int64_t{ p.w_length }));
  CHECK(only(&CostTerms::area, em * em) == int64_t{ p.w_area });
  CHECK(only(&CostTerms::area, (em * em) + 1) == (2 * int64_t{ p.w_area }));
  // The shipped `w_whitespace` is 0; these checks price it at 3.
  scav_profile priced{ p };
  priced.w_whitespace = 3;
  auto const whitespace = [&priced](int64_t v) {
    CostTerms t;
    t.whitespace = v;
    return cost_of(t, priced).t2;
  };
  CHECK(whitespace(1) == 3);
  CHECK(whitespace(em * em) == 3);
  CHECK(whitespace((em * em) + 1) == 6);

  // Zero terms score zero.
  CHECK(cost_of(CostTerms{}, p).t2 == 0);
  CHECK(only(&CostTerms::corridor, 0) == 0);
  CHECK(only(&CostTerms::area, 0) == 0);
}

TEST_CASE("cost: the shipped weights sum a hand-built term vector") {
  scav_profile const p{ profile() };
  REQUIRE(p.font_size_grid == 192);  // the ceilings below divide by this em
  CostTerms t;
  t.bends = 3;
  t.corridor = 400;  // 3 ems
  t.crossings = 5;
  t.excess_len = 1000;  // 6
  t.adjacency = 2;
  t.label = 4;
  t.label_near = 300;  // 2
  t.aspect = 500;      // 3
  t.area = 100000;     // 3 em squared
  t.length = 2000;     // 11
  t.transit_bends = 2;
  t.whitespace = 40000;  // 2 em squared
  CHECK(cost_of(t, p).t2 ==
        ((int64_t{ p.w_bends } * 3) + (int64_t{ p.w_corridor } * 3) +
         (int64_t{ p.w_crossings } * 5) + (int64_t{ p.w_excess_len } * 6) +
         (int64_t{ p.w_adjacency } * 2) + (int64_t{ p.w_label } * 4) +
         (int64_t{ p.w_label_near } * 2) + (int64_t{ p.w_aspect } * 3) +
         (int64_t{ p.w_area } * 3) + (int64_t{ p.w_length } * 11) +
         (int64_t{ p.w_transit_bends } * 2) + (int64_t{ p.w_whitespace } * 2)));
  CHECK(cost_of(t, p).t2 == 7473 + (int64_t{ p.w_length } * 11) +
                                (int64_t{ p.w_transit_bends } * 2) +
                                (int64_t{ p.w_whitespace } * 2));
}

TEST_CASE("cost: an em of one grid unit leaves every length where it stood") {
  // font_size_grid 1, the profile's lower bound, makes the em conversion the identity.
  scav_profile p{ profile() };
  p.font_size_grid = 1;
  REQUIRE(profile_validate(p));

  CostTerms t;
  t.corridor = 400;
  t.excess_len = 1000;
  t.label_near = 300;
  t.aspect = 500;
  t.area = 100000;
  t.length = 2000;
  t.whitespace = 40000;
  CHECK(cost_of(t, p).t2 ==
        ((int64_t{ p.w_corridor } * 400) + (int64_t{ p.w_excess_len } * 1000) +
         (int64_t{ p.w_label_near } * 300) + (int64_t{ p.w_aspect } * 500) +
         (int64_t{ p.w_area } * 100000) + (int64_t{ p.w_length } * 2000) +
         (int64_t{ p.w_whitespace } * 40000)));
}

TEST_CASE("cost: the shares divide the sum into floored basis points") {
  scav_profile const p{ profile() };
  CostTerms t;
  t.bends = 1;
  CHECK(cost_shares(t, p)[0] == 10000);  // one term is the whole of the sum

  t.area = 100000;  // three em squared at w_area against one bend at w_bends
  int64_t total{ 0 };
  for (int64_t const bp : cost_shares(t, p)) { total += bp; }
  // Each share floors: the sum is at most 10000, short by under one per scoring term.
  CHECK(total <= 10000);
  CHECK(total > (10000 - static_cast<int64_t>(TIER2_TERMS)));

  for (int64_t const bp : cost_shares(CostTerms{}, p)) { CHECK(bp == 0); }
}

TEST_CASE("cost: a hint outranks whatever Tier 2 adds up to") {
  // Tier 1 compares after Tier 0 and before Tier 2.
  Cost const hinted{ .t0_violations = 0, .t1_hints = 1, .t2 = 1000000 };
  Cost const unhinted{ .t0_violations = 0, .t1_hints = 2, .t2 = 0 };
  CHECK(cost_less(hinted, unhinted));
  CHECK_FALSE(cost_less(unhinted, hinted));

  // With equal hints, Tier 2 decides.
  Cost const same{ .t0_violations = 0, .t1_hints = 1, .t2 = 1000001 };
  CHECK(cost_less(hinted, same));
}

namespace {

// Two concurrent regions of one state, joined by one transition.
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
  build_trans(out.c, a, b, TransKind::Default, {});
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

  // Regions overlapping on one axis and at most `sub_sep` apart on the other
  // score 0; one unit further apart scores 1.
  CHECK(adjacency_of(r, at, { .x = 100 + p.sub_sep, .y = 0, .w = 100, .h = 100 }) == 0);
  CHECK(adjacency_of(r, at, { .x = 101 + p.sub_sep, .y = 0, .w = 100, .h = 100 }) == 1);
  CHECK(adjacency_of(r, at, { .x = 0, .y = 100 + p.sub_sep, .w = 100, .h = 100 }) == 0);
  CHECK(adjacency_of(r, at, { .x = 0, .y = 101 + p.sub_sep, .w = 100, .h = 100 }) == 1);

  // The pair is unordered: the same two rects the other way round.
  CHECK(adjacency_of(r, { .x = 100 + p.sub_sep, .y = 0, .w = 100, .h = 100 }, at) == 0);
  CHECK(adjacency_of(r, { .x = 0, .y = 100 + p.sub_sep, .w = 100, .h = 100 }, at) == 0);

  // Regions 300 apart on both axes, overlapping on neither, score 1.
  CHECK(adjacency_of(r, at, { .x = 400, .y = 400, .w = 100, .h = 100 }) == 1);

  // With only `adjacency` scored, the Tier-2 cost is `w_adjacency`.
  SizedLayout z{ blank(r.c) };
  z.sub[r.left.v] = at;
  z.sub[r.right.v] = { .x = 400, .y = 400, .w = 100, .h = 100 };
  CostTerms const t{ cost_terms(r.c, decompose(r.c), z, routes_of(r.c, {}), {}, p) };
  CHECK(t.adjacency == 1);
  CHECK(cost_of(t, p).t2 == int64_t{ p.w_adjacency });
}

TEST_CASE("cost: a fan-out across two regions is not charged for being apart") {
  // A fork or join at either end of the transition exempts the pair from `adjacency`.
  scav_rect const at{ .x = 0, .y = 0, .w = 100, .h = 100 };
  scav_rect const away{ .x = 400, .y = 400, .w = 100, .h = 100 };
  CHECK(adjacency_of(regions(StateKind::Normal, StateKind::Normal), at, away) == 1);
  CHECK(adjacency_of(regions(StateKind::Fork, StateKind::Normal), at, away) == 0);
  CHECK(adjacency_of(regions(StateKind::Join, StateKind::Normal), at, away) == 0);
  CHECK(adjacency_of(regions(StateKind::Normal, StateKind::Fork), at, away) == 0);
  CHECK(adjacency_of(regions(StateKind::Normal, StateKind::Join), at, away) == 0);

  // Any other pseudostate end, here a choice, is charged.
  CHECK(adjacency_of(regions(StateKind::Choice, StateKind::Normal), at, away) == 1);
}

TEST_CASE("cost: a route that leaves a box across its far side is inside it") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 20, .h = 20 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 20, .h = 20 };
  z.state[other.v] = { .x = 100, .y = 0, .w = 100, .h = 100 };
  // Starts on X's top border and leaves through its right side, the only side it
  // crosses.
  Routes const r{ routes_of(c, { { { .x = 150, .y = 0 }, { .x = 250, .y = 50 } } }) };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).through_box == 1);

  // Along X's top border.
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
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 100 };
  // Gone overlaps A, the route and the placed box.
  z.state[gone.v] = { .x = 80, .y = 10, .w = 200, .h = 60 };
  // P and Q overlap each other, clear of the rest.
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
  CHECK(live.label_over_box == 1);

  // A dead submachine's children are not paired as siblings.
  c.submachines[dropped.v].live = 0;
  CHECK(scored().box_overlap == 1);

  // With Gone dead, all three terms drop to zero.
  c.states[gone.v].live = 0;
  CostTerms const buried{ scored() };
  CHECK(buried.box_overlap == 0);
  CHECK(buried.through_box == 0);
  CHECK(buried.label_over_box == 0);
}

TEST_CASE("cost: a box in the composite's trailing band costs as its title does") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

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

  // A registered point column with no rows also scores zero.
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

  // Registered state rect and route span columns score zero on a chart with no
  // states or transitions.
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

TEST_CASE("cost: a chart that was never laid out scores nothing") {
  // Two siblings and a transition with no geometry columns; scoring returns zero
  // terms when any column is shorter than its entities.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  CostTerms const unlaid{ cost_columns(c, decompose(c), profile()) };
  CHECK(unlaid.bends == 0);
  CHECK(unlaid.corridor == 0);
  CHECK(unlaid.crossings == 0);
  CHECK(unlaid.excess_len == 0);
  CHECK(unlaid.adjacency == 0);
  CHECK(unlaid.label == 0);
  CHECK(unlaid.label_near == 0);
  CHECK(unlaid.aspect == 0);
  CHECK(unlaid.area == 0);
  CHECK(unlaid.through_box == 0);
  CHECK(unlaid.box_overlap == 0);
  CHECK(cost_of(unlaid, profile()).t2 == 0);

  // Rects with no route table also score zero, the chart rect included.
  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 0, .w = 100, .h = 40 };
  z.chart = { .x = 0, .y = 0, .w = 400, .h = 40 };
  CostTerms const routeless{ cost_terms(c, decompose(c), z, {}, {}, profile()) };
  CHECK(routeless.area == 0);
  CHECK(routeless.aspect == 0);
  CHECK(routeless.box_overlap == 0);
  CHECK(cost_of(routeless, profile()).t2 == 0);

  // Clearing any one rect column zeroes every term.
  Routes const r{ routes_of(c, { { { .x = 100, .y = 20 }, { .x = 300, .y = 20 } } }) };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).area == (400LL * 40));
  SizedLayout no_state{ z };
  no_state.state.clear();
  CHECK(cost_terms(c, decompose(c), no_state, r, {}, profile()).area == 0);
  SizedLayout no_before{ z };
  no_before.before.clear();
  CHECK(cost_terms(c, decompose(c), no_before, r, {}, profile()).area == 0);
  SizedLayout no_after{ z };
  no_after.after.clear();
  CHECK(cost_terms(c, decompose(c), no_after, r, {}, profile()).area == 0);
  SizedLayout no_sub{ z };
  no_sub.sub.clear();
  CHECK(cost_terms(c, decompose(c), no_sub, r, {}, profile()).area == 0);
}

TEST_CASE("cost: a route reaching past the points scores nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 0, .w = 100, .h = 40 };
  z.chart = { .x = 0, .y = 0, .w = 400, .h = 40 };
  Routes r{ routes_of(c, { { { .x = 100, .y = 20 }, { .x = 300, .y = 20 } } }) };

  // The span claims one point past the end of the point column; every term reads zero.
  r.route[0].len += 1;
  CostTerms const past{ cost_terms(c, decompose(c), z, r, {}, profile()) };
  CHECK(past.bends == 0);
  CHECK(past.area == 0);
  CHECK(past.aspect == 0);
  CHECK(cost_of(past, profile()).t2 == 0);

  // With the correct span, the route scores normally.
  r.route[0].len -= 1;
  CostTerms const t{ cost_terms(c, decompose(c), z, r, {}, profile()) };
  CHECK(t.bends == 0);
  CHECK(t.crossings == 0);
  CHECK(t.excess_len == 0);
  CHECK(t.through_box == 0);
  CHECK(t.box_overlap == 0);
  CHECK(t.area == 400LL * 40);
  CHECK(t.aspect == ((400LL * 10) - (40LL * 16)));
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

  // One grid unit wider adds `dar_den` to `aspect`; aspect converts to ems and area to
  // ems squared before weighting.
  z.chart.w += 1;
  CostTerms const off{ cost_terms(c, decompose(c), z, {}, {}, p) };
  CHECK(off.aspect == p.dar_den);
  int64_t const em{ p.font_size_grid };
  CHECK(cost_of(off, p).t2 == ((int64_t{ p.w_aspect } * ceil_div(off.aspect, em)) +
                               (int64_t{ p.w_area } * ceil_div(off.area, em * em))));
}

namespace {

// Outer holds A and B in one region; Leaf sits beside it in the root.
struct Composite {
  Chart c;
  StateId outer, a, b, leaf;
  SubmachineId inner;
};

Composite composite_chart() {
  Composite k;
  SubmachineId const root{ build_chart(k.c, "t", {}) };
  k.outer = build_state(k.c, root, "Outer", StateKind::Normal, {});
  k.inner = build_submachine(k.c, k.outer, "main", {});
  k.a = build_state(k.c, k.inner, "A", StateKind::Normal, {});
  k.b = build_state(k.c, k.inner, "B", StateKind::Normal, {});
  k.leaf = build_state(k.c, root, "Leaf", StateKind::Normal, {});
  build_trans(k.c, k.a, k.b, TransKind::Default, {});
  return k;
}

// Outer `w` by `h` at the origin: a ring of 10, bands of 40 and 10, A and B 20 apart at
// the hole's top left, in a chart at the desired ratio whatever Outer's size.
SizedLayout composite_sizing(Composite const &k, int32_t w, int32_t h) {
  SizedLayout z{ blank(k.c) };
  z.state[k.outer.v] = { .x = 0, .y = 0, .w = w, .h = h };
  z.before[k.outer.v] = { .x = 10, .y = 10, .w = w - 20, .h = 40 };
  z.after[k.outer.v] = { .x = 10, .y = h - 20, .w = w - 20, .h = 10 };
  z.sub[k.inner.v] = { .x = 10, .y = 50, .w = w - 20, .h = h - 70 };
  z.state[k.a.v] = { .x = 10, .y = 50, .w = 100, .h = 60 };
  z.state[k.b.v] = { .x = 130, .y = 50, .w = 100, .h = 60 };
  z.state[k.leaf.v] = { .x = 1400, .y = 0, .w = 100, .h = 60 };
  z.chart = { .x = 0, .y = 0, .w = 1600, .h = 1000 };
  return z;
}

CostTerms composite_terms(Composite const &k,
                          SizedLayout const &z,
                          scav_profile const &p) {
  Routes const r{ routes_of(k.c, { { { .x = 110, .y = 80 }, { .x = 130, .y = 80 } } }) };
  return cost_terms(k.c, decompose(k.c), z, r, {}, p);
}

}  // namespace

TEST_CASE("cost: whitespace is a composite's hole less its children's rects") {
  Composite k{ composite_chart() };
  scav_profile const p{ profile() };
  // Tight: the hole is 220 by 60; A and B fill all but the 20-wide gap between them.
  CHECK(composite_terms(k, composite_sizing(k, 240, 130), p).whitespace == 20 * 60);
  CHECK(composite_terms(k, composite_sizing(k, 440, 330), p).whitespace ==
        (420 * 260) - (2 * 100 * 60));

  // A dead child's rect counts as whitespace; a state whose only region is dead
  // scores no whitespace.
  SizedLayout const tight{ composite_sizing(k, 240, 130) };
  k.c.states[k.b.v].live = 0;
  CHECK(composite_terms(k, tight, p).whitespace == (20 * 60) + (100 * 60));
  k.c.states[k.b.v].live = 1;
  k.c.submachines[k.inner.v].live = 0;
  CHECK(composite_terms(k, tight, p).whitespace == 0);
  k.c.submachines[k.inner.v].live = 1;

  // Whitespace is capped at the chart's area.
  SizedLayout small{ tight };
  small.chart = { .x = 0, .y = 0, .w = 10, .h = 10 };
  CHECK(composite_terms(k, small, p).whitespace == 100);
}

TEST_CASE("cost: a padded composite costs more than the same composite tight") {
  Composite const k{ composite_chart() };
  scav_profile p{ profile() };
  REQUIRE(p.font_size_grid == 192);  // the ceilings below divide by this em
  p.w_whitespace = 1;
  CostTerms const tight{ composite_terms(k, composite_sizing(k, 240, 130), p) };
  CostTerms const padded{ composite_terms(k, composite_sizing(k, 440, 330), p) };
  CHECK(padded.whitespace > tight.whitespace);
  REQUIRE(padded.area == tight.area);
  REQUIRE(padded.aspect == tight.aspect);
  // Same chart rect and route: the difference is whitespace, 1 em squared against 3.
  CHECK(cost_of(tight, p).t2 < cost_of(padded, p).t2);
  CHECK((cost_of(padded, p).t2 - cost_of(tight, p).t2) == 2);
}

TEST_CASE("cost: the containment walk nests a descendant's interval in its own") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  StateId const beside{ build_state(c, root, "Beside", StateKind::Normal, {}) };
  SubmachineId const mid{ build_submachine(c, outer, "mid", {}) };
  StateId const inner{ build_state(c, mid, "Inner", StateKind::Normal, {}) };
  SubmachineId const deep{ build_submachine(c, inner, "deep", {}) };
  StateId const leaf{ build_state(c, deep, "Leaf", StateKind::Normal, {}) };

  Ancestry const an{ cost_flatten_ancestry(c) };
  CHECK(an.detached.empty());
  CHECK(an.tin[outer.v] < an.tin[inner.v]);
  CHECK(an.tin[inner.v] < an.tin[leaf.v]);
  CHECK(an.tout[leaf.v] <= an.tout[inner.v]);
  CHECK(an.tout[inner.v] <= an.tout[outer.v]);
  CHECK(an.tout[beside.v] == an.tin[beside.v]);  // a leaf's interval is a point

  // `cost_ancestor` agrees with `ancestor_or_self` for every ordered pair.
  for (StateId const a : { outer, beside, inner, leaf }) {
    for (StateId const b : { outer, beside, inner, leaf }) {
      CAPTURE(a.v);
      CAPTURE(b.v);
      CHECK(cost_ancestor(c, an, a, b) == ancestor_or_self(c, a, b));
    }
  }
  CHECK_FALSE(cost_ancestor(c, an, outer, { INVALID }));
  CHECK_FALSE(cost_ancestor(c, an, { INVALID }, outer));
  CHECK_FALSE(cost_ancestor(c, an, { INVALID }, { INVALID }));
}

TEST_CASE("cost: a state outside every children span answers by the climb") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const lost{ build_state(c, root, "Lost", StateKind::Normal, {}) };
  SubmachineId const under{ build_submachine(c, lost, "under", {}) };
  StateId const below{ build_state(c, under, "Below", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  // The root's child span stops before `Lost`, so the walk skips `Lost` and
  // `Below`; their `parent` links stay intact.
  --c.submachines[root.v].children.len;

  Ancestry const an{ cost_flatten_ancestry(c) };
  CHECK(an.tin[lost.v] == 0);
  CHECK(an.tin[below.v] == 0);
  CHECK(an.detached.size() == 2);
  CHECK(cost_ancestor(c, an, lost, below));
  CHECK_FALSE(cost_ancestor(c, an, below, lost));

  // Tier 0 tests both detached states at their own rects.
  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 20, .h = 20 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 20, .h = 20 };
  z.state[lost.v] = { .x = 100, .y = -50, .w = 200, .h = 100 };
  z.state[below.v] = { .x = 120, .y = -30, .w = 60, .h = 60 };
  Routes const r{ routes_of(c, { { { .x = 10, .y = 10 }, { .x = 410, .y = 10 } } }) };
  CHECK(cost_terms(c, decompose(c), z, r, {}, profile()).through_box == 2);
}

TEST_CASE("cost: a live state a tombstone stands over is still an obstacle") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const gone{ build_state(c, root, "Gone", StateKind::Normal, {}) };
  SubmachineId const under{ build_submachine(c, gone, "under", {}) };
  StateId const hidden{ build_state(c, under, "Hidden", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  c.states[gone.v].live = 0;

  Ancestry const an{ cost_flatten_ancestry(c) };
  CHECK(an.detached.size() == 1);
  CHECK(an.detached[0] == hidden.v);

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 20, .h = 20 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 20, .h = 20 };
  // Gone keeps a zero rect; Hidden alone sits on the route.
  z.state[hidden.v] = { .x = 150, .y = -50, .w = 100, .h = 100 };
  Routes const r{ routes_of(c, { { { .x = 10, .y = 10 }, { .x = 410, .y = 10 } } }) };
  CostTerms const t{ cost_terms(c, decompose(c), z, r, {}, profile()) };
  CHECK(t.through_box == 1);
  CHECK(t.box_overlap == 0);  // Gone is dead and Hidden has no sibling
}

namespace {

// `n` 40-unit boxes 100 apart on a diagonal, and `bar` over all of them; `bar` lies in
// every cell of the frame's grid.
Chart diagonal_chart(uint32_t n, SizedLayout &z, std::vector<StateId> &all, StateId &bar) {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  for (uint32_t i = 0; i < n; ++i) {
    all.push_back(build_state(c, root, "n", StateKind::Normal, {}));
  }
  bar = build_state(c, root, "bar", StateKind::Normal, {});
  z = blank(c);
  for (uint32_t i = 0; i < all.size(); ++i) {
    z.state[all[i].v] = { .x = static_cast<int32_t>(100 * i),
                          .y = static_cast<int32_t>(100 * i),
                          .w = 40,
                          .h = 40 };
  }
  int32_t const span{ static_cast<int32_t>(100 * n) };
  z.state[bar.v] = { .x = 0, .y = 0, .w = span, .h = span };
  return c;
}

}  // namespace

TEST_CASE("cost: a grid query yields a child once, whatever cells it spans") {
  SizedLayout z;
  std::vector<StateId> all;
  StateId bar{ INVALID };
  Chart const c{ diagonal_chart(9, z, all, bar) };
  ChildGrid const g{ cost_child_grid(c, z) };
  REQUIRE(g.frame.size() == 1);
  CHECK(g.frame[0].children.len == 10);
  CHECK(g.frame[0].side == 4);  // isqrt(10) + 1

  GridQuery q;
  cost_grid_query(g, 0, { .x = -10, .y = -10, .w = 2000, .h = 2000 }, q);
  CHECK(q.hit.size() == 10);  // `bar` covers all sixteen cells and comes once

  // Clamped into the far corner cell, which the last two of the nine reach.
  cost_grid_query(g, 0, { .x = 5000, .y = 5000, .w = 10, .h = 10 }, q);
  CHECK(q.hit.size() == 3);
  bool found{ false };
  for (uint32_t const at : q.hit) { found = found || (g.child[at] == bar.v); }
  CHECK(found);

  cost_grid_query(g, 7, { .x = 0, .y = 0, .w = 10, .h = 10 }, q);
  CHECK(q.hit.empty());  // no such frame
}

TEST_CASE("cost: overlapping siblings come out of the frame's grid, once a pair") {
  SizedLayout z;
  std::vector<StateId> all;
  StateId bar{ INVALID };
  Chart const c{ diagonal_chart(24, z, all, bar) };  // too many children to scan
  CHECK(cost_box_overlaps(c, z, cost_child_grid(c, z)) == 24);

  // With `bar` moved clear, no pair overlaps.
  z.state[bar.v] = { .x = 5000, .y = 5000, .w = 40, .h = 40 };
  CHECK(cost_box_overlaps(c, z, cost_child_grid(c, z)) == 0);
}

TEST_CASE("cost: overlapping siblings of a small frame are scanned, once a pair") {
  SizedLayout z;
  std::vector<StateId> all;
  StateId bar{ INVALID };
  Chart const c{ diagonal_chart(9, z, all, bar) };
  CHECK(cost_box_overlaps(c, z, cost_child_grid(c, z)) == 9);

  z.state[bar.v] = { .x = 5000, .y = 5000, .w = 40, .h = 40 };
  CHECK(cost_box_overlaps(c, z, cost_child_grid(c, z)) == 0);
}

namespace {

// One state holding two concurrent regions, each with one child, between the
// transition's endpoints; each child rect lies inside its parent's.
struct Concurrent {
  Chart c;
  StateId owner{ INVALID }, spinning{ INVALID }, dark{ INVALID };
  StateId src{ INVALID }, dst{ INVALID };
  SizedLayout z;
};

Concurrent concurrent_chart() {
  Concurrent out;
  Chart &c{ out.c };
  SubmachineId const root{ build_chart(c, "t", {}) };
  out.src = build_state(c, root, "Lit", StateKind::Normal, {});
  out.owner = build_state(c, root, "Running", StateKind::Normal, {});
  SubmachineId const motor{ build_submachine(c, out.owner, "motor", {}) };
  out.spinning = build_state(c, motor, "Spinning", StateKind::Normal, {});
  SubmachineId const light{ build_submachine(c, out.owner, "light", {}) };
  out.dark = build_state(c, light, "Dark", StateKind::Normal, {});
  out.dst = build_state(c, root, "Stopped", StateKind::Normal, {});
  build_trans(c, out.src, out.dst, TransKind::Default, {});

  out.z = blank(c);
  out.z.state[out.src.v] = { .x = 0, .y = 80, .w = 40, .h = 40 };
  out.z.state[out.owner.v] = { .x = 100, .y = 0, .w = 400, .h = 200 };
  out.z.state[out.spinning.v] = { .x = 120, .y = 40, .w = 100, .h = 120 };
  out.z.state[out.dark.v] = { .x = 280, .y = 40, .w = 100, .h = 120 };
  out.z.state[out.dst.v] = { .x = 600, .y = 80, .w = 40, .h = 40 };
  return out;
}

}  // namespace

TEST_CASE("cost: a piece in the root frame is charged for the grandchildren it enters") {
  Concurrent const r{ concurrent_chart() };
  // One straight line across the owner at a height strictly inside both of its
  // regions' children: the owner, and two states two levels below the frame.
  Routes const route{ routes_of(r.c,
                                { { { .x = 40, .y = 100 }, { .x = 600, .y = 100 } } }) };
  Ancestry const an{ cost_flatten_ancestry(r.c) };
  ChildGrid const g{ cost_child_grid(r.c, r.z) };
  CHECK(cost_through_boxes(r.c, r.z, an, g, pieces_of(route)) == 3);
  CHECK(cost_terms(r.c, decompose(r.c), r.z, route, {}, profile()).through_box == 3);

  // Above every one of them, the descent prunes at the owner and charges none.
  Routes const over{ routes_of(r.c,
                               { { { .x = 40, .y = -100 }, { .x = 600, .y = -100 } } }) };
  CHECK(cost_through_boxes(r.c, r.z, an, g, pieces_of(over)) == 0);
}

TEST_CASE("cost: a piece leaving one region is charged for its sibling's child") {
  Concurrent r{ concurrent_chart() };
  // With Spinning as the source, the owner encloses it and is carved out; Dark is
  // still charged.
  r.c.transitions[0].src = r.spinning;
  Routes const route{ routes_of(r.c,
                                { { { .x = 220, .y = 100 }, { .x = 600, .y = 100 } } }) };
  Ancestry const an{ cost_flatten_ancestry(r.c) };
  ChildGrid const g{ cost_child_grid(r.c, r.z) };
  CHECK(cost_through_boxes(r.c, r.z, an, g, pieces_of(route)) == 1);

  // The same segment with the carve-out gone: the owner is charged as well.
  r.c.transitions[0].src = r.src;
  Ancestry const flat{ cost_flatten_ancestry(r.c) };
  CHECK(cost_through_boxes(r.c, r.z, flat, g, pieces_of(route)) == 2);
}

TEST_CASE("cost: the carve-out excuses a box over an endpoint, not what it holds") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const main{ build_submachine(c, outer, "main", {}) };
  StateId const src{ build_state(c, main, "Src", StateKind::Normal, {}) };
  StateId const stranger{ build_state(c, main, "Stranger", StateKind::Normal, {}) };
  StateId const dst{ build_state(c, root, "Dst", StateKind::Normal, {}) };
  build_trans(c, src, dst, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.state[src.v] = { .x = 20, .y = 60, .w = 60, .h = 80 };
  z.state[stranger.v] = { .x = 150, .y = 60, .w = 60, .h = 80 };
  z.state[dst.v] = { .x = 600, .y = 80, .w = 40, .h = 40 };
  // From Src's border through Stranger and out of Outer: Outer is carved out and
  // Stranger is charged.
  Routes const route{ routes_of(c,
                                { { { .x = 80, .y = 100 }, { .x = 600, .y = 100 } } }) };
  CHECK(cost_terms(c, decompose(c), z, route, {}, profile()).through_box == 1);
}

TEST_CASE("cost: a degraded diagonal crosses what the axis-aligned sweep cannot") {
  // The band sweep pairs each vertical with the horizontals in its span; a diagonal
  // is tested against every piece.
  std::vector<Piece> const mixed{
    { .a = { .x = 0, .y = 50 }, .b = { .x = 100, .y = 50 }, .trans = 0, .k = 0 },
    { .a = { .x = 50, .y = 0 }, .b = { .x = 50, .y = 100 }, .trans = 1, .k = 0 },
    { .a = { .x = 0, .y = 0 }, .b = { .x = 100, .y = 100 }, .trans = 2, .k = 0 },
  };
  std::vector<uint32_t> per(3, 0);
  CHECK(cost_crossings(mixed, per) == 3);
  CHECK(per[0] == 2);
  CHECK(per[1] == 2);
  CHECK(per[2] == 2);

  // Two diagonals are one pair, charged from the earlier of the two alone.
  std::vector<Piece> const crossed{
    { .a = { .x = 0, .y = 0 }, .b = { .x = 100, .y = 100 }, .trans = 0, .k = 0 },
    { .a = { .x = 0, .y = 100 }, .b = { .x = 100, .y = 0 }, .trans = 1, .k = 0 },
  };
  std::vector<uint32_t> two(2, 0);
  CHECK(cost_crossings(crossed, two) == 1);

  // Neither collinear horizontals nor two legs of one transition count as crossings.
  std::vector<Piece> const parallel{
    { .a = { .x = 0, .y = 0 }, .b = { .x = 100, .y = 0 }, .trans = 0, .k = 0 },
    { .a = { .x = 50, .y = 0 }, .b = { .x = 150, .y = 0 }, .trans = 1, .k = 0 },
  };
  std::vector<uint32_t> flat(2, 0);
  CHECK(cost_crossings(parallel, flat) == 0);
  std::vector<Piece> const own{
    { .a = { .x = 0, .y = 50 }, .b = { .x = 100, .y = 50 }, .trans = 0, .k = 0 },
    { .a = { .x = 50, .y = 0 }, .b = { .x = 50, .y = 100 }, .trans = 0, .k = 1 },
  };
  std::vector<uint32_t> one(1, 0);
  CHECK(cost_crossings(own, one) == 0);
}

TEST_CASE("cost: collinear pieces meet in one bucket, and a trunk empties it") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, a, b, TransKind::Default, {});

  // One bucket, the horizontals at y = 0, sharing [50, 100]; the two verticals
  // sit at different x and so in buckets of their own.
  Routes const apart{ routes_of(
      c,
      { { { .x = 0, .y = 0 }, { .x = 100, .y = 0 }, { .x = 100, .y = 40 } },
        { { .x = 50, .y = 0 }, { .x = 150, .y = 0 }, { .x = 150, .y = 40 } } }) };
  CHECK(cost_corridor(apart, pieces_of(apart)) == 50);

  // Two runs along y=0 ending at one point: the 100 they share is trunk.
  Routes const merged{ routes_of(c,
                                 { { { .x = 0, .y = 0 }, { .x = 150, .y = 0 } },
                                   { { .x = 50, .y = 0 }, { .x = 150, .y = 0 } } }) };
  CHECK(cost_corridor(merged, pieces_of(merged)) == 0);

  // Perpendicular legs share no line.
  Routes const crossing{ routes_of(c,
                                   { { { .x = 0, .y = 0 }, { .x = 150, .y = 0 } },
                                     { { .x = 50, .y = -40 }, { .x = 50, .y = 40 } } }) };
  CHECK(cost_corridor(crossing, pieces_of(crossing)) == 0);
}

namespace {

Piece seg(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t trans) {
  return { .a = { .x = x0, .y = y0 }, .b = { .x = x1, .y = y1 }, .trans = trans, .k = 0 };
}

}  // namespace

TEST_CASE("cost: crowding charges two tight lanes their overlap scaled by the shortfall") {
  int32_t const em{ 192 };
  // Two horizontals of different transitions, 1000 overlapped, half an em apart:
  // half the overlap.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 96, 1000, 96, 1) }, em) == 500);
  // 191 apart charges 5; an em apart charges 0.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 191, 1000, 191, 1) }, em) == 5);
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 192, 1000, 192, 1) }, em) == 0);
  // Verticals score the same way on the other axis.
  CHECK(cost_crowding({ seg(0, 0, 0, 1000, 0), seg(48, 0, 48, 1000, 1) }, em) == 750);
  // Only the 400-unit overlap counts: 400 * 96 / 192.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(600, 96, 3000, 96, 1) }, em) == 200);
}

TEST_CASE("cost: crowding leaves what is not a tight lane to the terms that price it") {
  int32_t const em{ 192 };
  // Collinear pairs score in `corridor` alone.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 0, 1000, 0, 1) }, em) == 0);
  // Two legs of one transition score 0.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 96, 1000, 96, 0) }, em) == 0);
  // Perpendicular pairs score no crowding.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(500, 50, 500, 900, 1) }, em) == 0);
  // Parallel and tight with no overlap: end to end, or touching at x=1000.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(2000, 96, 3000, 96, 1) }, em) == 0);
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(1000, 96, 3000, 96, 1) }, em) == 0);
  // A degenerate piece has no axis and scores 0; a zero em scores 0.
  CHECK(cost_crowding({ seg(5, 5, 5, 5, 0), seg(5, 50, 900, 50, 1) }, em) == 0);
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 96, 1000, 96, 1) }, 0) == 0);
}

TEST_CASE("cost: crowding is continuous with corridor at the line they share") {
  // As two lanes close to one line, crowding approaches the shared length
  // `corridor` charges at zero separation.
  int32_t const em{ 192 };
  Wide last{ 0 };
  for (int32_t apart = 191; apart >= 1; --apart) {
    Wide const now{ cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, apart, 1000, apart, 1) },
                                  em) };
    CHECK(now >= last);  // closer never charges less
    last = now;
  }
  CHECK(last == 994);  // one unit apart: 1000 * 191 / 192, floored
}

TEST_CASE("cost: crowding sums every tight pair, and order does not matter") {
  int32_t const em{ 192 };
  // Three lanes 64 apart: pairs at 64, 64 and 128, all tight.
  std::vector<Piece> three{ seg(0, 0, 1000, 0, 0),
                            seg(0, 64, 1000, 64, 1),
                            seg(0, 128, 1000, 128, 2) };
  Wide const want{ ((1000 * 128) + (1000 * 128) + (1000 * 64)) / 192 };
  CHECK(cost_crowding(three, em) == want);
  std::vector<Piece> shuffled{ three[2], three[0], three[1] };
  CHECK(cost_crowding(shuffled, em) == want);
}

TEST_CASE("cost: a transition whose route vanished is a Tier 0 violation") {
  // `vanished` counts transitions with a segment to draw and fewer than two route points.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, a, TransKind::Default, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 0, .w = 100, .h = 40 };
  z.chart = { .x = 0, .y = 0, .w = 400, .h = 40 };
  scav_profile const p{ profile() };

  // Drawn: two points each.
  Routes const drawn{ routes_of(c,
                                { { { .x = 100, .y = 10 }, { .x = 300, .y = 10 } },
                                  { { .x = 300, .y = 30 }, { .x = 100, .y = 30 } } }) };
  CHECK(cost_terms(c, decompose(c), z, drawn, {}, p).vanished == 0);

  // A route collapsed to one point scores nothing in Tier 2 and one Tier-0 violation.
  Routes const collapsed{ routes_of(
      c,
      { { { .x = 100, .y = 10 }, { .x = 300, .y = 10 } }, { { .x = 300, .y = 30 } } }) };
  CostTerms const t{ cost_terms(c, decompose(c), z, collapsed, {}, p) };
  CHECK(t.vanished == 1);
  CHECK(cost_of(t, p).t0_violations == 1);
  CHECK(
      cost_less(cost_of(cost_terms(c, decompose(c), z, drawn, {}, p), p), cost_of(t, p)));

  // An empty route is vanished too.
  Routes const empty{
    routes_of(c, { { { .x = 100, .y = 10 }, { .x = 300, .y = 10 } }, {} })
  };
  CHECK(cost_terms(c, decompose(c), z, empty, {}, p).vanished == 1);
}

namespace {

// Every term by brute force over every pair, the oracle for the indexed scorer. Of
// `cost.cpp` it calls only the containment walk.
namespace reference {

Wide orient2d(scav_point a, scav_point b, scav_point c) {
  return ((Wide{ b.x } - a.x) * (Wide{ c.y } - a.y)) -
         ((Wide{ b.y } - a.y) * (Wide{ c.x } - a.x));
}

bool crosses(scav_point a, scav_point b, scav_point c, scav_point d) {
  Wide const d1{ orient2d(a, b, c) };
  Wide const d2{ orient2d(a, b, d) };
  Wide const d3{ orient2d(c, d, a) };
  Wide const d4{ orient2d(c, d, b) };
  if ((d1 == 0) || (d2 == 0) || (d3 == 0) || (d4 == 0)) { return false; }
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

bool enters(scav_point a, scav_point b, scav_rect const &r) {
  if (inside(a, r) || inside(b, r)) { return true; }
  scav_point const tl{ .x = r.x, .y = r.y };
  scav_point const tr{ .x = r.x + r.w, .y = r.y };
  scav_point const bl{ .x = r.x, .y = r.y + r.h };
  scav_point const br{ .x = r.x + r.w, .y = r.y + r.h };
  return crosses(a, b, tl, tr) || crosses(a, b, bl, br) || crosses(a, b, tl, bl) ||
         crosses(a, b, tr, br);
}

bool adjacent(scav_rect const &a, scav_rect const &b, int32_t sep) {
  bool const x_over{ (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) };
  bool const y_over{ (a.y < (b.y + b.h)) && (b.y < (a.y + a.h)) };
  int32_t const x_gap{ (a.x < b.x) ? (b.x - (a.x + a.w)) : (a.x - (b.x + b.w)) };
  int32_t const y_gap{ (a.y < b.y) ? (b.y - (a.y + a.h)) : (a.y - (b.y + b.h)) };
  return (y_over && (x_gap <= sep)) || (x_over && (y_gap <= sep));
}

Wide length_of(scav_point a, scav_point b) {
  Wide const dx{ Wide{ b.x } - a.x };
  Wide const dy{ Wide{ b.y } - a.y };
  return static_cast<Wide>(isqrt(static_cast<uint64_t>((dx * dx) + (dy * dy))));
}

Wide shared_run(scav_point a, scav_point b, scav_point c, scav_point d) {
  bool const flat{ (a.y == b.y) && (c.y == d.y) && (a.y == c.y) };
  bool const upright{ (a.x == b.x) && (c.x == d.x) && (a.x == c.x) };
  if (!(flat || upright)) { return 0; }
  Wide const alo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
  Wide const ahi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
  Wide const clo{ flat ? imin(c.x, d.x) : imin(c.y, d.y) };
  Wide const chi{ flat ? imax(c.x, d.x) : imax(c.y, d.y) };
  return imax(Wide{ 0 }, imin(ahi, chi) - imax(alo, clo));
}

// 0 horizontal, 1 vertical, 2 neither; `at` the line's coordinate.
uint32_t axis_of(Piece const &p, int32_t &at) {
  if ((p.a.y == p.b.y) && (p.a.x != p.b.x)) {
    at = p.a.y;
    return 0;
  }
  if ((p.a.x == p.b.x) && (p.a.y != p.b.y)) {
    at = p.a.x;
    return 1;
  }
  return 2;
}

struct Trunk {
  uint32_t tail{ 0 };
  bool merged_tail{ false };
};

Trunk trunk_of(std::vector<scav_point> const &pts,
               scav_span a,
               scav_span b,
               uint32_t cap = 2) {
  Trunk out;
  uint32_t const shortest{ imin(imin(a.len, b.len), cap) };
  while ((out.tail < shortest) &&
         same(pts[(a.off + a.len - 1) - out.tail], pts[(b.off + b.len - 1) - out.tail])) {
    ++out.tail;
  }
  if ((out.tail > 0) && (out.tail < a.len) && (out.tail < b.len)) {
    uint32_t const i{ (a.off + a.len - 1) - out.tail };
    uint32_t const j{ (b.off + b.len - 1) - out.tail };
    out.merged_tail = shared_run(pts[i], pts[i + 1], pts[j], pts[j + 1]) > 0;
  }
  return out;
}

bool trunk_piece(Trunk const &t, uint32_t len, uint32_t k) {
  return ((k + t.tail) >= len) || (t.merged_tail && ((k + t.tail + 1) == len));
}

// The span of `pts` reversed into `out`, so a common head reads as a common tail.
scav_span reversed(std::vector<scav_point> const &pts,
                   scav_span s,
                   std::vector<scav_point> &out) {
  scav_span const at{ .off = static_cast<uint32_t>(out.size()), .len = s.len };
  for (uint32_t k = s.len; k-- > 0;) { out.push_back(pts[s.off + k]); }
  return at;
}

// Whether segments `ku` of `u` and `kv` of `v` both lie in the two routes' common head or
// common tail, uncapped, with the leg out of or into it where those legs share a run.
bool fan_pair(Routes const &r, uint32_t u, uint32_t ku, uint32_t v, uint32_t kv) {
  scav_span const a{ r.route[u] };
  scav_span const b{ r.route[v] };
  Trunk const tail{ trunk_of(r.points, a, b, ~0U) };
  if (trunk_piece(tail, a.len, ku) && trunk_piece(tail, b.len, kv)) { return true; }
  std::vector<scav_point> back;
  scav_span const ra{ reversed(r.points, a, back) };
  scav_span const rb{ reversed(r.points, b, back) };
  Trunk const head{ trunk_of(back, ra, rb, ~0U) };
  return trunk_piece(head, a.len, a.len - 2 - ku) &&
         trunk_piece(head, b.len, b.len - 2 - kv);
}

// Pairs of different transitions' collinear segments sharing a run outside `fan_pair`.
int32_t shared_runs(Routes const &r, std::vector<Piece> const &pieces) {
  int32_t total{ 0 };
  for (uint32_t i = 0; i < pieces.size(); ++i) {
    for (uint32_t j = i + 1; j < pieces.size(); ++j) {
      Piece const &u{ pieces[i] };
      Piece const &v{ pieces[j] };
      if (u.trans == v.trans) { continue; }
      if (shared_run(u.a, u.b, v.a, v.b) <= 0) { continue; }
      if (!fan_pair(r, u.trans, u.k, v.trans, v.k)) { ++total; }
    }
  }
  return total;
}

uint32_t direction(scav_point a, scav_point b) {
  auto const axis = [](int32_t from, int32_t to) {
    if (to > from) { return 2U; }
    return (to < from) ? 0U : 1U;
  };
  return (axis(a.x, b.x) * 3U) + axis(a.y, b.y);
}

bool geometry_complete(Chart const &c, SizedLayout const &z, Routes const &r) {
  if ((z.state.size() < c.states.size()) || (z.before.size() < c.states.size()) ||
      (z.after.size() < c.states.size()) || (z.sub.size() < c.submachines.size()) ||
      (r.route.size() < c.transitions.size())) {
    return false;
  }
  for (scav_span const &span : r.route) {
    if ((Wide{ span.off } + span.len) > static_cast<Wide>(r.points.size())) {
      return false;
    }
  }
  return true;
}

bool within(Chart const &c, StateId state, uint32_t m) {
  for (StateId at{ state }; (at.v != INVALID) && (at.v < c.states.size());) {
    SubmachineId const up{ c.states[at.v].parent };
    if (up.v == m) { return true; }
    if (up.v >= c.submachines.size()) { return false; }
    at = c.submachines[up.v].owner;
  }
  return false;
}

int64_t crossings(std::vector<Piece> const &pieces, std::vector<uint32_t> &per_trans) {
  int64_t total{ 0 };
  for (uint32_t i = 0; i < pieces.size(); ++i) {
    for (uint32_t j = i + 1; j < pieces.size(); ++j) {
      if (pieces[i].trans == pieces[j].trans) { continue; }
      if (!crosses(pieces[i].a, pieces[i].b, pieces[j].a, pieces[j].b)) { continue; }
      ++total;
      ++per_trans[pieces[i].trans];
      ++per_trans[pieces[j].trans];
    }
  }
  return total;
}

Wide corridor(Routes const &r, std::vector<Piece> const &pieces) {
  Wide total{ 0 };
  for (uint32_t i = 0; i < pieces.size(); ++i) {
    for (uint32_t j = i + 1; j < pieces.size(); ++j) {
      Piece const &u{ pieces[i] };
      Piece const &v{ pieces[j] };
      if (u.trans == v.trans) { continue; }
      Wide const shared{ shared_run(u.a, u.b, v.a, v.b) };
      if (shared <= 0) { continue; }
      Trunk const pair{ trunk_of(r.points, r.route[u.trans], r.route[v.trans]) };
      if (trunk_piece(pair, r.route[u.trans].len, u.k) &&
          trunk_piece(pair, r.route[v.trans].len, v.k)) {
        continue;
      }
      total += shared;
    }
  }
  return total;
}

Wide crowding(std::vector<Piece> const &pieces, int32_t em) {
  if (em <= 0) { return 0; }
  Wide scaled{ 0 };
  for (uint32_t i = 0; i < pieces.size(); ++i) {
    for (uint32_t j = i + 1; j < pieces.size(); ++j) {
      Piece const &u{ pieces[i] };
      Piece const &v{ pieces[j] };
      int32_t u_at{ 0 };
      int32_t v_at{ 0 };
      uint32_t const axis{ axis_of(u, u_at) };
      if ((axis >= 2) || (axis_of(v, v_at) != axis) || (u.trans == v.trans)) { continue; }
      Wide const apart{ imax(Wide{ u_at } - v_at, Wide{ v_at } - u_at) };
      if ((apart == 0) || (apart >= em)) { continue; }
      Wide const u_lo{ (axis == 0) ? imin(u.a.x, u.b.x) : imin(u.a.y, u.b.y) };
      Wide const u_hi{ (axis == 0) ? imax(u.a.x, u.b.x) : imax(u.a.y, u.b.y) };
      Wide const v_lo{ (axis == 0) ? imin(v.a.x, v.b.x) : imin(v.a.y, v.b.y) };
      Wide const v_hi{ (axis == 0) ? imax(v.a.x, v.b.x) : imax(v.a.y, v.b.y) };
      Wide const along{ imin(u_hi, v_hi) - imax(u_lo, v_lo) };
      if (along <= 0) { continue; }
      scaled += along * (Wide{ em } - apart);
    }
  }
  return scaled / em;
}

// A frame's children with a rect, live, in span order.
std::vector<uint32_t> children_of(Chart const &c, SizedLayout const &z, uint32_t m) {
  std::vector<uint32_t> out;
  Span const kids{ c.submachines[m].children };
  for (uint32_t i = 0; i < kids.len; ++i) {
    uint32_t const st{ c.state_ids[kids.off + i].v };
    if ((st < z.state.size()) && (c.states[st].live != 0)) { out.push_back(st); }
  }
  return out;
}

int32_t box_overlaps(Chart const &c, SizedLayout const &z) {
  int32_t total{ 0 };
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    std::vector<uint32_t> const kids{ children_of(c, z, m) };
    for (uint32_t i = 0; i < kids.size(); ++i) {
      for (uint32_t j = i + 1; j < kids.size(); ++j) {
        if (overlaps(z.state[kids[i]], z.state[kids[j]])) { ++total; }
      }
    }
  }
  return total;
}

// Region `m`'s rect grown across to its owner's box and along to the midpoints of the gaps
// to its live sibling regions; its rect alone without siblings.
scav_rect region_cell(Chart const &c, SizedLayout const &z, uint32_t m) {
  scav_rect const &r{ z.sub[m] };
  StateId const owner{ c.submachines[m].owner };
  if (owner.v == INVALID) { return r; }
  scav_rect const &b{ z.state[owner.v] };
  int32_t lo_x{ r.x };
  int32_t hi_x{ r.x + r.w };
  int32_t lo_y{ r.y };
  int32_t hi_y{ r.y + r.h };
  Span const subs{ c.states[owner.v].submachines };
  for (uint32_t k = 0; k < subs.len; ++k) {
    uint32_t const o{ c.submachine_ids[subs.off + k].v };
    if ((o == m) || (c.submachines[o].live == 0)) { continue; }
    scav_rect const &q{ z.sub[o] };
    if (((q.y + q.h) <= r.y) || (q.y >= (r.y + r.h))) {
      lo_x = std::min(lo_x, b.x);
      hi_x = std::max(hi_x, b.x + b.w);
      if ((q.y + q.h) <= r.y) {
        lo_y = std::min(lo_y, q.y + q.h + ((r.y - (q.y + q.h)) / 2));
      } else {
        hi_y = std::max(hi_y, r.y + r.h + ((q.y - (r.y + r.h)) / 2));
      }
    } else if (((q.x + q.w) <= r.x) || (q.x >= (r.x + r.w))) {
      lo_y = std::min(lo_y, b.y);
      hi_y = std::max(hi_y, b.y + b.h);
      if ((q.x + q.w) <= r.x) {
        lo_x = std::min(lo_x, q.x + q.w + ((r.x - (q.x + q.w)) / 2));
      } else {
        hi_x = std::max(hi_x, r.x + r.w + ((q.x - (r.x + r.w)) / 2));
      }
    }
  }
  return { .x = lo_x, .y = lo_y, .w = hi_x - lo_x, .h = hi_y - lo_y };
}

// `through_box`, and `through_region` per piece entering a live region of a state the
// descent reaches or of a detached one, where neither end lies in the region: its
// `region_cell`, or its rect where an end is the region's owner.
void through(Chart const &c,
             SizedLayout const &z,
             Ancestry const &an,
             std::vector<Piece> const &pieces,
             CostTerms &t) {
  for (Piece const &piece : pieces) {
    Transition const &tr{ c.transitions[piece.trans] };
    scav_rect const reach{ span_rect(piece.a, piece.b) };
    bool foreign{ false };
    auto const charge = [&](uint32_t st) {
      if (enters(piece.a, piece.b, z.state[st]) && !cost_ancestor(c, an, { st }, tr.src) &&
          !cost_ancestor(c, an, { st }, tr.dst)) {
        ++t.through_box;
      }
      Span const subs{ c.states[st].submachines };
      bool src_side{ false };
      bool dst_side{ false };
      for (uint32_t i = 0; i < subs.len; ++i) {
        uint32_t const m{ c.submachine_ids[subs.off + i].v };
        bool const own{ (tr.src.v == st) || (tr.dst.v == st) };
        scav_rect const cell{ own ? z.sub[m] : region_cell(c, z, m) };
        if ((c.submachines[m].live == 0) || !enters(piece.a, piece.b, cell)) { continue; }
        bool const has_src{ within(c, tr.src, m) };
        bool const has_dst{ within(c, tr.dst, m) };
        foreign = foreign || (!has_src && !has_dst);
        src_side = src_side || (has_src && !has_dst);
        dst_side = dst_side || (has_dst && !has_src);
      }
      // An external route's piece entering both its ends' regions crosses their divider.
      foreign = foreign || ((tr.kind == TransKind::External) && src_side && dst_side);
    };
    for (uint32_t const st : an.detached) { charge(st); }
    std::vector<uint32_t> stack;
    for (uint32_t m = 0; m < c.submachines.size(); ++m) {
      if (c.submachines[m].owner.v == INVALID) { stack.push_back(m); }
    }
    while (!stack.empty()) {
      uint32_t const frame{ stack.back() };
      stack.pop_back();
      if (frame >= c.submachines.size()) { continue; }
      for (uint32_t const st : children_of(c, z, frame)) {
        if (!overlaps(reach, z.state[st])) { continue; }
        charge(st);
        Span const subs{ c.states[st].submachines };
        for (uint32_t i = 0; i < subs.len; ++i) {
          stack.push_back(c.submachine_ids[subs.off + i].v);
        }
      }
    }
    t.through_region += foreign ? 1 : 0;
  }
}

// `s` and every state enclosing it, `s` first.
std::vector<StateId> chain_up(Chart const &c, StateId s) {
  std::vector<StateId> out;
  for (StateId at{ s }; (at.v != INVALID) && (out.size() < c.states.size());
       at = enclosing_state(c, at)) {
    out.push_back(at);
  }
  return out;
}

// Each end's chain, the end first, cut where it meets the other's: the states
// between that end and the innermost state holding both.
std::array<std::vector<StateId>, 2> below_common(Chart const &c,
                                                 StateId src,
                                                 StateId dst) {
  std::array<std::vector<StateId>, 2> out{ chain_up(c, src), chain_up(c, dst) };
  for (size_t i = 0; i < out[0].size(); ++i) {
    for (size_t j = 0; j < out[1].size(); ++j) {
      if (out[0][i] == out[1][j]) {
        out[0].resize(i);
        out[1].resize(j);
        return out;
      }
    }
  }
  return out;
}

// A bend in a state passed through: on one end's chain, neither the end nor
// the state enclosing it holds the point, and a state above those does.
bool transit_bend(Chart const &c,
                  SizedLayout const &z,
                  Transition const &tr,
                  scav_point at) {
  for (std::vector<StateId> const &chain : below_common(c, tr.src, tr.dst)) {
    if (chain.size() < 3) { continue; }
    if (inside(at, z.state[chain[0].v]) || inside(at, z.state[chain[1].v])) { continue; }
    for (size_t k = 2; k < chain.size(); ++k) {
      if (inside(at, z.state[chain[k].v])) { return true; }
    }
  }
  return false;
}

CostTerms terms(Chart const &c,
                SplitGraph const &g,
                SizedLayout const &z,
                Routes const &r,
                scav_spaces const &s,
                scav_profile const &p) {
  CostTerms t;
  if (!geometry_complete(c, z, r)) { return t; }
  t.aspect = (Wide{ z.chart.w } * p.dar_den) - (Wide{ z.chart.h } * p.dar_num);
  if (t.aspect < 0) { t.aspect = -t.aspect; }
  t.area = Wide{ z.chart.w } * z.chart.h;

  // Each live state's rect charged to its region's owner by the parent link; a composite
  // is a live owner of a live region.
  std::vector<Wide> held(c.states.size(), 0);
  std::vector<uint8_t> composite(c.states.size(), 0);
  for (Submachine const &m : c.submachines) {
    if ((m.live != 0) && (m.owner.v != INVALID)) { composite[m.owner.v] = 1; }
  }
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    SubmachineId const up{ c.states[st].parent };
    if ((c.states[st].live == 0) || (up.v >= c.submachines.size()) ||
        (c.submachines[up.v].live == 0) || (c.submachines[up.v].owner.v == INVALID)) {
      continue;
    }
    held[c.submachines[up.v].owner.v] += Wide{ z.state[st].w } * z.state[st].h;
  }
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if ((c.states[st].live == 0) || (composite[st] == 0)) { continue; }
    scav_rect const &box{ z.state[st] };
    scav_rect const &b{ z.before[st] };
    Wide const top{ Wide{ b.y } + b.h };
    Wide const bottom{ (Wide{ box.y } + box.h) - (Wide{ b.y } - box.y) - z.after[st].h };
    Wide const hole{ Wide{ b.w } * imax(bottom - top, Wide{ 0 }) };
    t.whitespace += imax(hole - held[st], Wide{ 0 });
  }
  t.whitespace = imin(t.whitespace, t.area);

  std::vector<Piece> pieces;
  std::vector<uint32_t> crossings_of(c.transitions.size(), 0);
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    scav_span const route{ r.route[tr] };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      pieces.push_back({ .a = r.points[route.off + k],
                         .b = r.points[route.off + k + 1],
                         .trans = tr,
                         .k = k });
      if ((k + 2) < route.len) {
        uint32_t const in{ direction(r.points[route.off + k],
                                     r.points[route.off + k + 1]) };
        uint32_t const out{ direction(r.points[route.off + k + 1],
                                      r.points[route.off + k + 2]) };
        if (in != out) { ++t.bends; }
        if ((in != out) &&
            transit_bend(c, z, c.transitions[tr], r.points[route.off + k + 1])) {
          ++t.transit_bends;
        }
        if (((in % 2) == 1) && (out == (8 - in))) { ++t.retrace; }
      }
    }
    for (uint32_t i = 0; (i + 1) < route.len; ++i) {
      for (uint32_t j = i + 2; (j + 1) < route.len; ++j) {
        if (crosses(r.points[route.off + i],
                    r.points[route.off + i + 1],
                    r.points[route.off + j],
                    r.points[route.off + j + 1])) {
          ++t.self_crossing;
        }
      }
    }
  }
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    if ((tr < g.trans_segments.size()) && (g.trans_segments[tr].len != 0) &&
        (r.route[tr].len < 2)) {
      ++t.vanished;
    }
  }
  t.crossings = crossings(pieces, crossings_of);
  t.corridor = corridor(r, pieces);
  t.shared_run = shared_runs(r, pieces);
  t.crowding = crowding(pieces, p.font_size_grid);

  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    scav_span const route{ r.route[tr] };
    if (route.len < 2) { continue; }
    Wide actual{ 0 };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      actual += length_of(r.points[route.off + k], r.points[route.off + k + 1]);
    }
    t.length += actual;
    Wide boxes{ 0 };
    for (uint32_t i = 0; i < s.n_path_box; ++i) {
      if (s.path_box[i].subject == tr) { boxes += s.path_box[i].w; }
    }
    Wide const direct{ length_of(r.points[route.off],
                                 r.points[route.off + route.len - 1]) };
    Wide const excess{ actual - imax(direct, boxes) };
    if (excess > 0) { t.excess_len += excess * (1 + crossings_of[tr]); }
  }

  std::vector<uint8_t> encloses(c.states.size(), 0);
  auto const mark = [&](StateId of, uint8_t v) {
    StateId at{ enclosing_state(c, of) };
    for (size_t step = 0; (step < c.states.size()) && (at.v != INVALID); ++step) {
      encloses[at.v] = v;
      at = enclosing_state(c, at);
    }
  };
  for (uint32_t i = 0; i < r.placed.size(); ++i) {
    for (uint32_t j = i + 1; j < r.placed.size(); ++j) {
      if (overlaps(r.placed[i], r.placed[j])) { ++t.label; }
    }
    uint32_t subject{ INVALID };
    uint32_t host{ INVALID };
    Wide height{ 0 };
    if ((s.path_box != nullptr) && (i < s.n_path_box) &&
        (s.path_box[i].subject < c.transitions.size())) {
      subject = s.path_box[i].subject;
      height = s.path_box[i].h;
      Transition const &tr{ c.transitions[subject] };
      mark(tr.src, 1);
      StateId up{ enclosing_state(c, tr.dst) };
      for (size_t step = 0; (step < c.states.size()) && (up.v != INVALID); ++step) {
        if (encloses[up.v] == 1) { encloses[up.v] = 2; }
        if (up.v == tr.src.v) { host = up.v; }
        up = enclosing_state(c, up);
      }
      for (StateId at{ enclosing_state(c, tr.src) }; at.v != INVALID;
           at = enclosing_state(c, at)) {
        if (at.v == tr.dst.v) { host = at.v; }
      }
      if ((tr.src == tr.dst) && (tr.kind != TransKind::Default)) { host = tr.src.v; }
    }
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if (c.states[st].live == 0) { continue; }
      // Bands with zero width or height are ignored.
      auto const wall = [&](Vector<scav_rect> const &v) {
        return (st < v.size()) && (v[st].w > 0) && (v[st].h > 0) &&
               overlaps(r.placed[i], v[st]);
      };
      bool const bands{ wall(z.before) || wall(z.after) || wall(z.lead) || wall(z.trail) };
      if (st == host) {
        if (bands) { ++t.label_over_box; }
      } else if (encloses[st] == 2) {
        if (bands) { ++t.label; }
      } else if (overlaps(r.placed[i], z.state[st])) {
        ++t.label_over_box;
      }
    }
    Wide own{ -1 };
    Wide other{ -1 };
    for (Piece const &piece : pieces) {
      scav_rect const seg{ span_rect(piece.a, piece.b) };
      Wide const away{ chebyshev_gap(r.placed[i], seg) };
      if (overlaps(r.placed[i], seg)) { ++t.label_over_route; }
      if (piece.trans == subject) {
        own = (own < 0) ? away : imin(own, away);
        continue;
      }
      other = (other < 0) ? away : imin(other, away);
    }
    if (own > label_leader(p)) { ++t.label_far; }
    if ((own >= 0) && (other >= 0)) {
      Wide const shortfall{ (own + height) - other };
      if (shortfall > 0) { t.label_near += shortfall; }
    }
    if (subject != INVALID) { mark(c.transitions[subject].src, 0); }
  }

  for (SplitPort const &port : g.ports) {
    if ((port.sub.v == INVALID) || (port.into.v == INVALID)) { continue; }
    Transition const &tr{ c.transitions[port.trans.v] };
    StateKind const src_kind{ c.states[tr.src.v].kind };
    StateKind const dst_kind{ c.states[tr.dst.v].kind };
    if ((src_kind == StateKind::Fork) || (src_kind == StateKind::Join) ||
        (dst_kind == StateKind::Fork) || (dst_kind == StateKind::Join)) {
      continue;
    }
    if (!adjacent(z.sub[port.sub.v], z.sub[port.into.v], p.sub_sep)) { ++t.adjacency; }
  }

  Ancestry const an{ cost_flatten_ancestry(c) };
  t.box_overlap = box_overlaps(c, z);
  through(c, z, an, pieces, t);
  for (Piece const &piece : pieces) {
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if ((c.states[st].live != 0) &&
          along_border(piece.a, piece.b, z.state[st], border_band(p) - 1)) {
        ++t.flush;
        break;
      }
    }
  }
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    scav_span const route{ r.route[tr] };
    if (!inner_loop(c, tr) || (route.len < 2)) { continue; }
    uint32_t const st{ c.transitions[tr].dst.v };
    scav_rect const box{ z.state[st] };
    uint32_t const ruled{ (st < s.n_box_state) ? s.box_state[st].ruled : 0U };
    auto const drawn = [&](scav_point at) {
      bool const border{ (((at.x == box.x) || (at.x == box.x + box.w)) &&
                          (at.y >= box.y) && (at.y <= box.y + box.h)) ||
                         (((at.y == box.y) || (at.y == box.y + box.h)) &&
                          (at.x >= box.x) && (at.x <= box.x + box.w)) };
      auto const row = [st](Vector<scav_rect> const &v) {
        return (st < v.size()) ? v[st] : scav_rect{};
      };
      scav_rect const b{ row(z.before) };
      scav_rect const a{ row(z.after) };
      scav_rect const l{ row(z.lead) };
      scav_rect const w{ row(z.trail) };
      bool const across_x{ (at.x >= b.x) && (at.x <= b.x + b.w) };
      bool const across_y{ (at.y >= l.y) && (at.y <= l.y + l.h) };
      return border ||
             (((ruled & 1U) != 0) && (b.h > 0) && across_x && (at.y == b.y + b.h)) ||
             (((ruled & 2U) != 0) && (a.h > 0) && across_x && (at.y == a.y)) ||
             (((ruled & 4U) != 0) && (l.w > 0) && across_y && (at.x == l.x + l.w)) ||
             (((ruled & 8U) != 0) && (w.w > 0) && across_y && (at.x == w.x));
    };
    if (!drawn(r.points[route.off]) || !drawn(r.points[route.off + route.len - 1])) {
      ++t.loop_unanchored;
    }
  }
  return t;
}

}  // namespace reference

constexpr uint32_t TERMS{ 19 };

std::array<int64_t, TERMS> terms_of(CostTerms const &t) {
  return { t.bends,    t.corridor,      t.crossings,      t.excess_len,  t.adjacency,
           t.label,    t.label_near,    t.aspect,         t.area,        t.crowding,
           t.length,   t.transit_bends, t.whitespace,     t.through_box, t.box_overlap,
           t.vanished, t.flush,         t.through_region, t.label_far };
}

constexpr std::array<char const *, TERMS> TERM_NAMES{
  "bends",    "corridor",      "crossings",      "excess_len",  "adjacency",
  "label",    "label_near",    "aspect",         "area",        "crowding",
  "length",   "transit_bends", "whitespace",     "through_box", "box_overlap",
  "vanished", "flush",         "through_region", "label_far"
};

// The first term the two disagree on, or empty.
std::string first_difference(CostTerms const &got, CostTerms const &want) {
  std::array<int64_t, TERMS> const a{ terms_of(got) };
  std::array<int64_t, TERMS> const b{ terms_of(want) };
  for (uint32_t i = 0; i < TERMS; ++i) {
    if (a[i] != b[i]) { return TERM_NAMES[i]; }
  }
  if (got.retrace != want.retrace) { return "retrace"; }
  if (got.self_crossing != want.self_crossing) { return "self_crossing"; }
  if (got.shared_run != want.shared_run) { return "shared_run"; }
  if (got.label_over_box != want.label_over_box) { return "label_over_box"; }
  if (got.label_over_route != want.label_over_route) { return "label_over_route"; }
  if (got.loop_unanchored != want.loop_unanchored) { return "loop_unanchored"; }
  return {};
}

// Seeded coordinates on a lattice of 5 in [-60, 60], a quarter nudged by -2..2.
struct Lattice {
  uint64_t s;
  uint32_t next(uint32_t n) {
    s = (s * 6364136223846793005ULL) + 1442695040888963407ULL;
    return static_cast<uint32_t>((s >> 33U) % n);
  }
  int32_t coord() {
    int32_t const nudge{ (next(4) == 0) ? (static_cast<int32_t>(next(5)) - 2) : 0 };
    return (5 * (static_cast<int32_t>(next(25)) - 12)) + nudge;
  }
  int32_t extent() { return 5 * static_cast<int32_t>(next(9)); }  // zero included
  scav_rect rect() { return { .x = coord(), .y = coord(), .w = extent(), .h = extent() }; }
  // One step of a route: along an axis mostly, diagonal or nowhere sometimes.
  scav_point step(scav_point at) {
    int32_t const d{ 5 * (static_cast<int32_t>(next(9)) - 4) };
    switch (next(8)) {
      case 0: return { .x = at.x + d, .y = at.y + d };
      case 1: return at;
      default:
        return (next(2) == 0) ? scav_point{ .x = at.x + d, .y = at.y }
                              : scav_point{ .x = at.x, .y = at.y + d };
    }
  }
};

// Nested states, some with two regions, pseudostates, sometimes one frame wider than
// the scan threshold, and tombstones among the states and regions.
Chart random_chart(Lattice &r) {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<SubmachineId> frames{ root };
  std::vector<StateId> states;
  constexpr std::array<StateKind, 4> ODD{ StateKind::Initial,
                                          StateKind::Choice,
                                          StateKind::Fork,
                                          StateKind::Join };
  uint32_t const n{ 2 + r.next(20) };
  for (uint32_t i = 0; i < n; ++i) {
    SubmachineId const in{ frames[r.next(static_cast<uint32_t>(frames.size()))] };
    StateKind const kind{ (r.next(6) == 0) ? ODD[r.next(4)] : StateKind::Normal };
    StateId const st{ build_state(c, in, "S", kind, {}) };
    states.push_back(st);
    if (r.next(3) == 0) {
      frames.push_back(build_submachine(c, st, "m", {}));
      if (r.next(2) == 0) { frames.push_back(build_submachine(c, st, "n", {})); }
    }
  }
  if (r.next(3) == 0) {
    SubmachineId const in{ frames[r.next(static_cast<uint32_t>(frames.size()))] };
    uint32_t const wide{ 14 + r.next(30) };
    for (uint32_t i = 0; i < wide; ++i) {
      states.push_back(build_state(c, in, "W", StateKind::Normal, {}));
    }
  }
  std::vector<uint8_t> dead(c.states.size(), 0);
  for (StateId const st : states) { dead[st.v] = (r.next(10) == 0) ? 1U : 0U; }
  std::vector<StateId> alive;
  for (StateId const st : states) {
    if (dead[st.v] == 0) { alive.push_back(st); }
  }
  uint32_t const trans{ alive.empty() ? 0U : r.next((2 * n) + 1) };
  for (uint32_t i = 0; i < trans; ++i) {
    StateId const src{ alive[r.next(static_cast<uint32_t>(alive.size()))] };
    StateId const dst{ alive[r.next(static_cast<uint32_t>(alive.size()))] };
    build_trans(c, src, dst, TransKind::Default, {});
  }
  for (StateId const st : states) {
    if (dead[st.v] != 0) { c.states[st.v].live = 0; }
  }
  for (uint32_t m = 1; m < c.submachines.size(); ++m) {
    if (r.next(12) == 0) { c.submachines[m].live = 0; }
  }
  return c;
}

// Random geometry, routes and boxes for `c`; a quarter of routes after the first end on
// an earlier route's last one or two points.
struct Candidate {
  SizedLayout z;
  Routes r;
  std::vector<scav_path_box> boxes;
  [[nodiscard]] scav_spaces spaces() const {
    return { .path_box = boxes.empty() ? nullptr : boxes.data(),
             .n_path_box = static_cast<uint32_t>(boxes.size()) };
  }
};

Candidate random_candidate(Chart const &c, Lattice &r) {
  Candidate out;
  SizedLayout &z{ out.z };
  z = blank(c);
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    z.state[st] = r.rect();
    if (r.next(2) == 0) { z.before[st] = r.rect(); }
    if (r.next(2) == 0) { z.after[st] = r.rect(); }
  }
  for (scav_rect &sub : z.sub) { sub = r.rect(); }
  z.chart = r.rect();

  std::vector<std::vector<scav_point>> lines(c.transitions.size());
  for (uint32_t t = 0; t < lines.size(); ++t) {
    uint32_t const len{ r.next(7) };
    if (len == 0) { continue; }
    scav_point at{ .x = r.coord(), .y = r.coord() };
    lines[t].push_back(at);
    for (uint32_t k = 1; k < len; ++k) {
      at = r.step(at);
      lines[t].push_back(at);
    }
    if ((t > 0) && (r.next(4) == 0)) {
      std::vector<scav_point> const &into{ lines[r.next(t)] };
      if (!into.empty()) {
        lines[t].push_back(into[into.size() - 1]);
        if ((into.size() > 1) && (r.next(2) == 0)) {
          lines[t].back() = into[into.size() - 2];
          lines[t].push_back(into[into.size() - 1]);
        }
      }
    }
  }
  out.r = routes_of(c, lines);

  uint32_t const trans{ static_cast<uint32_t>(c.transitions.size()) };
  uint32_t const placed{ r.next(trans + 3) };
  for (uint32_t i = 0; i < placed; ++i) { out.r.placed.push_back(r.rect()); }
  uint32_t const named{ (r.next(4) == 0) ? r.next(placed + 1) : placed };
  for (uint32_t i = 0; i < named; ++i) {
    out.boxes.push_back({ .subject = r.next(trans + 2),
                          .w = r.extent() * 4,
                          .h = r.extent(),
                          .order = 0 });
  }
  return out;
}

// `k` moved by `by` on both axes, which changes which lane-key bytes agree and
// which radix passes run.
Candidate shifted(Candidate k, int32_t by) {
  auto const rect = [by](scav_rect &x) {
    x.x += by;
    x.y += by;
  };
  for (scav_rect &x : k.z.state) { rect(x); }
  for (scav_rect &x : k.z.before) { rect(x); }
  for (scav_rect &x : k.z.after) { rect(x); }
  for (scav_rect &x : k.z.sub) { rect(x); }
  rect(k.z.chart);
  for (scav_rect &x : k.r.placed) { rect(x); }
  for (scav_point &pt : k.r.points) {
    pt.x += by;
    pt.y += by;
  }
  return k;
}

// The readable profile with the knobs the terms read drawn at random: the em, the
// `flush` band through `node_sep` and `pad`, and `sub_sep`.
scav_profile random_profile(Lattice &r) {
  scav_profile p{ profile() };
  constexpr std::array<int32_t, 5> EM{ 0, 1, 5, 12, 20 };
  constexpr std::array<int32_t, 3> SEP{ 3, 15, 30 };
  constexpr std::array<int32_t, 3> PAD{ 0, 5, 12 };
  p.font_size_grid = EM[r.next(5)];
  p.node_sep = SEP[r.next(3)];
  p.pad = PAD[r.next(3)];
  p.sub_sep = 5 * static_cast<int32_t>(r.next(3));
  return p;
}

}  // namespace

TEST_CASE("cost: the indexed terms are the direct scans' over seeded random charts" *
          doctest::test_suite("full")) {
  // Each candidate scored through a kept context, a per-call one, and the reference;
  // `seen` tallies each term nonzero somewhere.
  Lattice r{ 20260928 };
  std::array<uint32_t, TERMS> seen{};
  uint32_t mismatches{ 0 };
  uint32_t long_sorts{ 0 };  // candidates past the insertion sort's cutoff
  for (uint32_t trial = 0; (trial < 600) && (mismatches == 0); ++trial) {
    Chart const c{ random_chart(r) };
    SplitGraph const g{ decompose(c) };
    CostContext const ctx{ cost_context(c, g) };
    for (uint32_t cand = 0; cand < 4; ++cand) {
      constexpr std::array<int32_t, 4> MOVE{ 0, 1000, 0, -1000 };
      Candidate const k{ shifted(random_candidate(c, r), MOVE[cand]) };
      scav_profile const p{ random_profile(r) };
      scav_spaces const s{ k.spaces() };
      uint32_t keys{ 0 };  // the lane sort's: one per axis-aligned piece of nonzero length
      for (Piece const &pc : pieces_of(k.r)) {
        keys += ((pc.a.x == pc.b.x) != (pc.a.y == pc.b.y)) ? 1U : 0U;
      }
      long_sorts += (keys > SCAV_SORT_SMALL) ? 1U : 0U;
      CostTerms const want{ reference::terms(c, g, k.z, k.r, s, p) };
      std::string const held{ first_difference(cost_terms(ctx, c, g, k.z, k.r, s, p),
                                               want) };
      std::string const fresh{ first_difference(cost_terms(c, g, k.z, k.r, s, p), want) };
      if (!held.empty() || !fresh.empty()) {
        CAPTURE(trial);
        CAPTURE(cand);
        CHECK(held == "");
        CHECK(fresh == "");
        ++mismatches;
        break;
      }
      std::array<int64_t, TERMS> const got{ terms_of(want) };
      for (uint32_t i = 0; i < TERMS; ++i) { seen[i] += (got[i] != 0) ? 1U : 0U; }
    }
  }
  CHECK(mismatches == 0);
  CHECK(long_sorts > 0);
  for (uint32_t i = 0; i < TERMS; ++i) {
    CAPTURE(TERM_NAMES[i]);
    CHECK(seen[i] > 0);
  }
}

TEST_CASE("cost: the indexed terms are the direct scans' at the edges") {
  // Outer encloses both ends of A -> B; Far lies outside it.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  StateId const far{ build_state(c, root, "Far", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, far, TransKind::Default, {});
  SplitGraph const g{ decompose(c) };
  CostContext const ctx{ cost_context(c, g) };
  // A band of twelve and an em of twenty.
  scav_profile p{ profile() };
  p.node_sep = 30;
  p.pad = 12;
  p.font_size_grid = 20;
  int32_t const near{ border_band(p) - 1 };
  REQUIRE(near == 11);

  SizedLayout z{ blank(c) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.before[outer.v] = { .x = 0, .y = 0, .w = 400, .h = 20 };
  z.after[outer.v] = { .x = 0, .y = 180, .w = 400, .h = 20 };
  z.sub[inner.v] = { .x = 10, .y = 20, .w = 380, .h = 160 };
  z.state[a.v] = { .x = 40, .y = 60, .w = 100, .h = 60 };
  z.state[b.v] = { .x = 140, .y = 60, .w = 100, .h = 60 };  // touching A, not over it
  z.state[far.v] = { .x = 600, .y = 60, .w = 0, .h = 0 };   // zero-size
  z.chart = { .x = 0, .y = 0, .w = 600, .h = 200 };

  auto const agree = [&](Routes const &r, scav_spaces const &s) {
    CostTerms const want{ reference::terms(c, g, z, r, s, p) };
    CHECK(first_difference(cost_terms(ctx, c, g, z, r, s, p), want) == "");
    CHECK(first_difference(cost_terms(c, g, z, r, s, p), want) == "");
    return want;
  };

  // No routes, then one point each.
  CHECK(agree(routes_of(c, {}), {}).vanished == 2);
  CHECK(agree(routes_of(c, { { { .x = 90, .y = 90 } }, { { .x = 190, .y = 90 } } }), {})
            .vanished == 2);

  // Along A's top border, out to one unit past the band either side; then single-point
  // routes on A's and B's top borders.
  for (int32_t const off : { 0, near, near + 1, -near, -(near + 1) }) {
    CAPTURE(off);
    Routes const along{ routes_of(
        c,
        { { { .x = 50, .y = 60 + off }, { .x = 130, .y = 60 + off } },
          { { .x = 240, .y = 70 }, { .x = 600, .y = 70 } } }) };
    CHECK(agree(along, {}).flush == ((imax(off, -off) <= near) ? 1 : 0));
  }
  Routes const point{ routes_of(c,
                                { { { .x = 90, .y = 60 }, { .x = 90, .y = 60 } },
                                  { { .x = 200, .y = 60 }, { .x = 200, .y = 60 } } }) };
  CHECK(agree(point, {}).flush == 2);
  // Down the composite's left border and along the chart's far edge, at the
  // clamped end of every grid.
  Routes const rim{ routes_of(c,
                              { { { .x = 0, .y = 0 }, { .x = 0, .y = 200 } },
                                { { .x = 600, .y = -500 }, { .x = 600, .y = 900 } } }) };
  agree(rim, {});

  // A label over its own route, over the other one, against B, touching A,
  // and a zero-size one; with their space requests, one naming no transition.
  Routes r{ routes_of(c,
                      { { { .x = 140, .y = 150 }, { .x = 300, .y = 150 } },
                        { { .x = 300, .y = 140 }, { .x = 600, .y = 140 } } }) };
  r.placed = { { .x = 150, .y = 140, .w = 60, .h = 20 },
               { .x = 280, .y = 130, .w = 60, .h = 20 },
               { .x = 200, .y = 100, .w = 60, .h = 30 },
               { .x = 0, .y = 60, .w = 40, .h = 20 },
               { .x = 360, .y = 150, .w = 0, .h = 0 } };
  std::vector<scav_path_box> const boxes{
    { .subject = 0, .w = 60, .h = 20, .order = 0 },
    { .subject = 1, .w = 60, .h = 20, .order = 0 },
    { .subject = 0, .w = 60, .h = 30, .order = 1 },
    { .subject = 7, .w = 40, .h = 20, .order = 0 },
  };
  scav_spaces const s{ .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()) };
  CostTerms const labelled{ agree(r, s) };
  CHECK(labelled.label_over_box + labelled.label_over_route > 0);
  CHECK(labelled.label_near > 0);

  // The box's own route is 40 above it and its height 20, so a foreign route nearer
  // than 60 is a shortfall: 59 charges one, 60 none.
  for (int32_t const gap : { 59, 60 }) {
    CAPTURE(gap);
    Routes lone{ routes_of(
        c,
        { { { .x = 150, .y = 100 }, { .x = 300, .y = 100 } },
          { { .x = 150, .y = 160 + gap }, { .x = 300, .y = 160 + gap } } }) };
    lone.placed = { { .x = 200, .y = 140, .w = 40, .h = 20 } };
    scav_path_box const box{ .subject = 0, .w = 40, .h = 20, .order = 0 };
    CHECK(agree(lone, { .path_box = &box, .n_path_box = 1 }).label_near ==
          ((gap < 60) ? 1 : 0));
  }
}

TEST_CASE("cost: a thread's kept buffers carry nothing from one chart to the next") {
  // A, then B, then A on this thread, each against a fresh thread's score; the random
  // pairs resize the kept buffers between calls.
  auto const fresh =
      [](Chart const &c, SplitGraph const &g, Candidate const &k, scav_profile const &p) {
        CostTerms out;
        std::thread([&] { out = cost_terms(c, g, k.z, k.r, k.spaces(), p); }).join();
        return out;
      };
  Lattice r{ 41 };
  uint32_t mismatches{ 0 };
  for (uint32_t trial = 0; (trial < 150) && (mismatches == 0); ++trial) {
    Chart const a{ random_chart(r) };
    Chart const b{ random_chart(r) };
    SplitGraph const ga{ decompose(a) };
    SplitGraph const gb{ decompose(b) };
    Candidate const ka{ random_candidate(a, r) };
    Candidate const kb{ random_candidate(b, r) };
    scav_profile const pa{ random_profile(r) };
    scav_profile const pb{ random_profile(r) };
    CostTerms const want_a{ fresh(a, ga, ka, pa) };
    CostTerms const want_b{ fresh(b, gb, kb, pb) };
    std::string const first{
      first_difference(cost_terms(a, ga, ka.z, ka.r, ka.spaces(), pa), want_a)
    };
    std::string const between{
      first_difference(cost_terms(b, gb, kb.z, kb.r, kb.spaces(), pb), want_b)
    };
    std::string const again{
      first_difference(cost_terms(a, ga, ka.z, ka.r, ka.spaces(), pa), want_a)
    };
    if (!first.empty() || !between.empty() || !again.empty()) {
      CAPTURE(trial);
      CHECK(first == "");
      CHECK(between == "");
      CHECK(again == "");
      ++mismatches;
    }
  }
  CHECK(mismatches == 0);
}

TEST_CASE("cost: a stop ends the count once the Tier 2 terms but the labels' reach it") {
  // A route that jogs through a third box: Tier 0 counts the box, Tier 2 the bends.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const m{ build_state(c, root, "M", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  SplitGraph const g{ decompose(c) };
  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 100 };
  z.state[b.v] = { .x = 600, .y = 0, .w = 100, .h = 100 };
  z.state[m.v] = { .x = 300, .y = -50, .w = 100, .h = 200 };
  z.chart = { .x = 0, .y = -50, .w = 700, .h = 200 };
  z.sub[root.v] = z.chart;
  Routes const r{ routes_of(c,
                            { { { .x = 100, .y = 50 },
                                { .x = 200, .y = 50 },
                                { .x = 200, .y = 30 },
                                { .x = 500, .y = 30 },
                                { .x = 500, .y = 50 },
                                { .x = 600, .y = 50 } } }) };
  scav_profile const p{ profile() };
  CostContext const ctx{ cost_context(c, g) };
  CostTerms const whole{ cost_terms(ctx, c, g, z, r, {}, p) };
  REQUIRE(whole.through_box == 1);
  REQUIRE(whole.bends == 4);
  int64_t const t2{ cost_of(whole, p).t2 };

  CostStop low{ .t2 = t2 };
  CostTerms const cut{ cost_terms(ctx, c, g, z, r, {}, p, nullptr, nullptr, &low) };
  CHECK(low.stopped);
  CHECK(cut.through_box == 0);
  CHECK(cut.bends == 4);
  CHECK(cost_of(cut, p).t2 == t2);

  CostStop high{ .t2 = t2 + 1 };
  CostTerms const all{ cost_terms(ctx, c, g, z, r, {}, p, nullptr, nullptr, &high) };
  CHECK_FALSE(high.stopped);
  CHECK(first_difference(all, whole).empty());

  // Without aspect the same stop is not reached.
  REQUIRE(whole.aspect > 0);
  CostStop flat{ .t2 = t2, .aspect = false };
  (void)cost_terms(ctx, c, g, z, r, {}, p, nullptr, nullptr, &flat);
  CHECK_FALSE(flat.stopped);
}

TEST_CASE("cost: a context built once scores every candidate as one built for it") {
  Lattice r{ 7 };
  for (uint32_t trial = 0; trial < 40; ++trial) {
    CAPTURE(trial);
    Chart const c{ random_chart(r) };
    SplitGraph const g{ decompose(c) };
    CostContext const ctx{ cost_context(c, g) };
    bool agree{ true };
    for (uint32_t cand = 0; cand < 12; ++cand) {
      Candidate const k{ random_candidate(c, r) };
      scav_profile const p{ random_profile(r) };
      scav_spaces const s{ k.spaces() };
      CostTerms const held{ cost_terms(ctx, c, g, k.z, k.r, s, p) };
      CostTerms const fresh{ cost_terms(cost_context(c, g), c, g, k.z, k.r, s, p) };
      agree = agree && first_difference(held, fresh).empty();
    }
    CHECK(agree);
    // Scoring leaves `ctx` equal to a fresh build, its grid cells unfilled.
    CostContext const again{ cost_context(c, g) };
    CHECK(ctx.an.tin == again.an.tin);
    CHECK(ctx.an.tout == again.an.tout);
    CHECK(ctx.an.detached == again.an.detached);
    CHECK(ctx.transit_top == again.transit_top);
    CHECK(ctx.grid.child == again.grid.child);
    CHECK(ctx.grid.bucket_off == again.grid.bucket_off);
    CHECK(ctx.grid.bucket_at.empty());
    REQUIRE(ctx.grid.frame.size() == again.grid.frame.size());
    for (uint32_t m = 0; m < ctx.grid.frame.size(); ++m) {
      CHECK(ctx.grid.frame[m].children.off == again.grid.frame[m].children.off);
      CHECK(ctx.grid.frame[m].children.len == again.grid.frame[m].children.len);
      CHECK(ctx.grid.frame[m].side == again.grid.frame[m].side);
      CHECK(ctx.grid.frame[m].bucket == again.grid.frame[m].bucket);
    }
  }
}

TEST_CASE("cost bound: boxes sharing a row or a column take no turn, others one") {
  scav_rect const a{ .x = 0, .y = 0, .w = 100, .h = 40 };
  CHECK(cost_path_turns(a, 4, {}, { .x = 300, .y = 20, .w = 100, .h = 40 }, 4) == 0);
  CHECK(cost_path_turns(a, 4, {}, { .x = 50, .y = 200, .w = 100, .h = 40 }, 4) == 0);
  CHECK(cost_path_turns(a, 4, {}, { .x = 300, .y = 200, .w = 100, .h = 40 }, 4) == 1);
  // Touching corners share a point.
  CHECK(cost_path_turns(a, 4, {}, { .x = 100, .y = 40, .w = 100, .h = 40 }, 4) == 0);
}

TEST_CASE("cost bound: a named face turned from the far box costs two turns") {
  scav_rect const a{ .x = 0, .y = 0, .w = 100, .h = 400 };
  scav_rect const right{ .x = 300, .y = 0, .w = 100, .h = 400 };
  CHECK(cost_path_turns(a, 1, {}, right, 4) == 0);  // right face, toward it
  CHECK(cost_path_turns(a, 0, {}, right, 4) == 2);  // left face, out and back
  CHECK(cost_path_turns(a, 2, {}, right, 4) == 2);  // top face, up, across, down
  CHECK(cost_path_turns(a, 4, {}, right, 0) == 0);  // into its left face
  CHECK(cost_path_turns(a, 4, {}, right, 1) == 2);  // into its right face, round it
  CHECK(cost_path_turns(a, 0, {}, right, 1) == 4);  // both faces turned away
  // A box beyond the named face takes no extra turn.
  CHECK(cost_path_turns(right, 0, {}, a, 4) == 0);
}

TEST_CASE("cost bound: a waypoint off the line between two boxes costs its turns") {
  scav_rect const a{ .x = 0, .y = 0, .w = 100, .h = 40 };
  scav_rect const b{ .x = 300, .y = 0, .w = 100, .h = 40 };
  CHECK(cost_path_turns(a, 4, { { .x = 200, .y = 20 } }, b, 4) == 0);
  CHECK(cost_path_turns(a, 4, { { .x = 200, .y = 300 } }, b, 4) == 2);
  CHECK(cost_path_turns(a, 4, { { .x = 200, .y = 300 }, { .x = 250, .y = 300 } }, b, 4) ==
        2);
  CHECK(cost_path_turns(a, 4, { { .x = 200, .y = 300 }, { .x = 250, .y = 20 } }, b, 4) ==
        2);
  CHECK(cost_path_turns(a, 4, { { .x = 200, .y = 300 }, { .x = 250, .y = 600 } }, b, 4) ==
        3);
}

namespace {

// `c` sized by hand: every per-state vector sized, loops empty.
SizedLayout sized(Chart const &c) {
  SizedLayout z{ blank(c) };
  z.lead.assign(c.states.size(), scav_rect{});
  z.trail.assign(c.states.size(), scav_rect{});
  z.loop.assign(c.states.size(), scav_rect{});
  z.loop_place.assign(c.states.size(), 0);
  return z;
}

// A rectilinear router that names no face for an unpinned end.
class AnyFaceRouter final : public Router {
 public:
  [[nodiscard]] RouterName name() const override { return { .bytes = "any", .len = 3 }; }
  [[nodiscard]] uint32_t version() const override { return 1; }
  [[nodiscard]] bool rectilinear() const override { return true; }
  void route(RouteInput const & /*in*/, RouteOutput &out) const override {
    out.points.clear();
    out.net_points.clear();
    out.metrics.clear();
  }
};

OrthogonalRouter const ORTHO;
AnyFaceRouter const ANY_FACE;
StraightRouter const STRAIGHT;

// Two states and one transition A -> B in the root, sized as `a` and `b`; the chart is
// their cover. `more` adds siblings sized as given.
struct TwoBoxes {
  Chart c;
  SplitGraph g;
  SizedLayout z;
  std::vector<std::vector<uint32_t>> bends;

  TwoBoxes(scav_rect const &a,
           scav_rect const &b,
           std::vector<scav_rect> const &more = {},
           StateKind a_kind = StateKind::Normal) {
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const sa{ build_state(c, root, "A", a_kind, {}) };
    StateId const sb{ build_state(c, root, "B", StateKind::Normal, {}) };
    std::vector<StateId> kids;
    for (size_t k = 0; k < more.size(); ++k) {
      std::string const name{ "S" + std::to_string(k) };
      kids.push_back(build_state(c, root, name, StateKind::Normal, {}));
    }
    build_trans(c, sa, sb, TransKind::Default, {});
    g = decompose(c);
    z = sized(c);
    z.state[sa.v] = a;
    z.state[sb.v] = b;
    for (size_t k = 0; k < more.size(); ++k) { z.state[kids[k].v] = more[k]; }
    z.before = z.state;  // no corner arc
    int32_t x0{ a.x };
    int32_t y0{ a.y };
    int32_t x1{ a.x + a.w };
    int32_t y1{ a.y + a.h };
    for (scav_rect const &r : more) {
      x0 = std::min(x0, r.x);
      y0 = std::min(y0, r.y);
      x1 = std::max(x1, r.x + r.w);
      y1 = std::max(y1, r.y + r.h);
    }
    x0 = std::min(x0, b.x);
    y0 = std::min(y0, b.y);
    x1 = std::max(x1, b.x + b.w);
    y1 = std::max(y1, b.y + b.h);
    z.chart = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
    z.sub[root.v] = z.chart;
    bends.assign(g.segments.size(), {});
  }

  // The bound's bends under `router` with `faces` named.
  [[nodiscard]] int64_t turns(Router const &router,
                              Vector<uint32_t> const &faces = {}) const {
    scav_profile const p{ profile() };
    return cost_bound(c, g, bends, z, faces, p, route_clearance(p), router).bends;
  }
};

// The `CandidateMemo::box_faces` word naming face `face` at end `end` of segment `seg`.
constexpr uint32_t face_word(uint32_t seg, uint32_t end, uint32_t face) {
  return (seg << 3U) | (end << 2U) | face;
}

}  // namespace

TEST_CASE("cost bound: area, length and bends of two boxes, and a named face") {
  TwoBoxes const two{ { .x = 0, .y = 0, .w = 100, .h = 400 },
                      { .x = 300, .y = 600, .w = 100, .h = 400 } };
  scav_profile const p{ profile() };
  int32_t const clear{ route_clearance(p) };

  CostTerms const t{ cost_bound(two.c, two.g, two.bends, two.z, {}, p, clear, ANY_FACE) };
  CHECK(t.area == 400LL * 1000);
  CHECK(t.length == 200 + 200);
  CHECK(t.bends == 1);
  CHECK(cost_of(t, p).t0_violations == 0);

  // A router that is not rectilinear: the larger axis gap, and no bends.
  CostTerms const any{
    cost_bound(two.c, two.g, two.bends, two.z, {}, p, clear, STRAIGHT)
  };
  CHECK(any.length == 200);
  CHECK(any.bends == 0);

  // Leaving A by its left face, away from B, turns twice.
  CHECK(two.turns(ANY_FACE, { face_word(0, 0, 0) }) == 2);
}

TEST_CASE("cost bound: an unnamed end is seated on the face its aim escapes by") {
  // B lies further below A than beside it: both ends seat on the faces toward each other
  // along y, whose runs share no x, so the route jogs.
  TwoBoxes const two{ { .x = 0, .y = 0, .w = 100, .h = 400 },
                      { .x = 300, .y = 600, .w = 100, .h = 400 } };
  CHECK(two.turns(ANY_FACE) == 1);
  CHECK(two.turns(ORTHO) == 2);
  // A named left face turns away from B and comes round to B's top.
  CHECK(two.turns(ORTHO, { face_word(0, 0, 0) }) == 3);
}

TEST_CASE("cost bound: a named face too short to seat on takes the escape face") {
  scav_profile const p{ profile() };
  int32_t const clear{ route_clearance(p) };
  TwoBoxes const two{ { .x = 0, .y = 0, .w = 100, .h = 2 * clear },
                      { .x = 300, .y = 600, .w = 100, .h = 400 } };
  // The left face cannot seat; the end takes A's bottom, as an unnamed end does.
  CHECK(two.turns(ORTHO, { face_word(0, 0, 0) }) == two.turns(ORTHO));
  CHECK(two.turns(ORTHO) == 2);
}

TEST_CASE("cost bound: a seat keeps off its face's corners") {
  // A's bottom right corner touches B's top left corner: no face of one meets a face of
  // the other away from a corner, so no straight line joins them.
  TwoBoxes const two{ { .x = 0, .y = 0, .w = 100, .h = 400 },
                      { .x = 300, .y = 400, .w = 100, .h = 400 } };
  CHECK(two.turns(ANY_FACE) == 1);
  CHECK(two.turns(ORTHO) == 2);  // right face to left face, at different heights
  CHECK(two.turns(ANY_FACE, { face_word(0, 0, 1), face_word(0, 1, 0) }) == 2);
}

TEST_CASE("cost bound: an inscribed glyph is left at a face's middle") {
  // B's left face starts below the middle of the glyph's right face.
  TwoBoxes const two{ { .x = 0, .y = 0, .w = 100, .h = 100 },
                      { .x = 300, .y = 60, .w = 100, .h = 400 },
                      {},
                      StateKind::Choice };
  CHECK(two.turns(ANY_FACE) == 1);  // down from the bottom middle, into B's left face
  CHECK(two.turns(ORTHO) == 1);
  // The same box as a plain state seats level with B on its right face.
  TwoBoxes const plain{ { .x = 0, .y = 0, .w = 100, .h = 100 },
                        { .x = 300, .y = 60, .w = 100, .h = 400 } };
  CHECK(plain.turns(ANY_FACE) == 0);
}

TEST_CASE("cost bound: a box holding loops may be left by any face") {
  TwoBoxes two{ { .x = 0, .y = 0, .w = 100, .h = 400 },
                { .x = 300, .y = 600, .w = 100, .h = 400 } };
  CHECK(two.turns(ORTHO) == 2);
  two.z.loop[0] = { .x = 10, .y = 10, .w = 20, .h = 20 };
  CHECK(two.turns(ORTHO) == 1);  // A's right face, then down into B's top
}

TEST_CASE("cost bound: a waypoint bounds no turn") {
  // A route may leave its waypoint as a spike its simplification drops.
  TwoBoxes two{ { .x = 0, .y = 0, .w = 100, .h = 400 },
                { .x = 300, .y = 0, .w = 100, .h = 400 } };
  two.z.node = { { .x = 200, .y = 900 } };
  two.bends[0] = { 0 };
  CHECK(two.turns(ANY_FACE, { face_word(0, 0, 1), face_word(0, 1, 0) }) == 0);
  // The waypoint still aims an unnamed end: A and B take their bottom faces.
  CHECK(two.turns(ORTHO) == 2);
}

TEST_CASE("cost bound: a straight line every sibling blocks takes two turns") {
  scav_rect const a{ .x = 0, .y = 0, .w = 100, .h = 400 };
  scav_rect const b{ .x = 600, .y = 0, .w = 100, .h = 400 };
  // A sibling across the whole run between the faces.
  TwoBoxes const wall{ a, b, { { .x = 250, .y = -100, .w = 100, .h = 600 } } };
  CHECK(wall.turns(ORTHO) == 2);
  // A sibling across part of it leaves a straight line clear.
  TwoBoxes const post{ a, b, { { .x = 250, .y = 100, .w = 100, .h = 50 } } };
  CHECK(post.turns(ORTHO) == 0);
  // A sibling whose border lies along the run within the border band blocks that line.
  scav_profile const p{ profile() };
  int32_t const band{ border_band(p) };
  TwoBoxes const strip{ { .x = 0, .y = 0, .w = 100, .h = 3 * band },
                        { .x = 600, .y = 0, .w = 100, .h = 3 * band },
                        { { .x = 250, .y = -band, .w = 100, .h = 2 * band },
                          { .x = 250, .y = 2 * band, .w = 100, .h = 2 * band } } };
  CHECK(strip.turns(ORTHO) == 2);
  // A sibling beyond the faces' span blocks nothing.
  TwoBoxes const beside{ a, b, { { .x = 900, .y = -100, .w = 100, .h = 600 } } };
  CHECK(beside.turns(ORTHO) == 0);
}

TEST_CASE("cost bound: an inner loop takes two bends, a self-transition none") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::Internal, {});
  build_trans(c, a, a, TransKind::External, {});
  SplitGraph const g{ decompose(c) };
  SizedLayout z{ sized(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 400, .h = 400 };
  z.chart = z.state[a.v];
  scav_profile const p{ profile() };
  CostTerms const t{ cost_bound(c, g, {}, z, {}, p, route_clearance(p), ORTHO) };
  CHECK(t.bends == 2);
  CHECK(t.length == 0);
}

TEST_CASE("cost bound: unseated, an unnamed end may leave from anywhere on its box") {
  scav_profile const p{ profile() };
  int32_t const clear{ route_clearance(p) };
  auto const unseated = [&](scav_rect const &a,
                            scav_rect const &b,
                            Vector<uint32_t> const &faces = {}) {
    TwoBoxes const two{ a, b };
    return cost_bound(two.c, two.g, two.bends, two.z, faces, p, clear, ORTHO, false).bends;
  };
  scav_rect const a{ .x = 0, .y = 0, .w = 100, .h = 400 };
  scav_rect const diagonal{ .x = 300, .y = 600, .w = 100, .h = 400 };
  CHECK(unseated(a, diagonal) == 1);
  CHECK(unseated(a, { .x = 300, .y = 400, .w = 100, .h = 400 }) == 0);
  CHECK(unseated(a, { .x = 300, .y = 100, .w = 100, .h = 400 }) == 0);
  // A named face is left square from anywhere on it.
  CHECK(unseated(a, diagonal, { face_word(0, 0, 0) }) == 2);
}

TEST_CASE("cost bound: each transition's share of the bends sums to the bound") {
  TwoBoxes const two{ { .x = 0, .y = 0, .w = 100, .h = 400 },
                      { .x = 300, .y = 600, .w = 100, .h = 400 } };
  scav_profile const p{ profile() };
  std::vector<int32_t> share;
  CostTerms const t{ cost_bound(two.c,
                                two.g,
                                two.bends,
                                two.z,
                                {},
                                p,
                                route_clearance(p),
                                ORTHO,
                                true,
                                &share) };
  REQUIRE(share.size() == two.c.transitions.size());
  CHECK(share[0] == t.bends);
}
