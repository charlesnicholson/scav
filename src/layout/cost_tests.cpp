// Scoring against hand-written geometry: two rects and one route are enough
// to assert a single term, with no model beyond the entities they belong to
// and no pipeline run to produce them.

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

#include <array>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace scav {

// The containment walk, the grid and the three sweeps `cost.cpp` brackets with
// SCAV_INTERNAL, declared here rather than in a header so the shipping build
// keeps them internal.
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

// The flat segment list `cost_terms` builds, so a sweep taking one can be
// handed a route rather than a hand-numbered table.
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
  CHECK(t.length == 200);
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

namespace {

// Src in the root and Dst two composites down, so Src -> Dst only passes through Arm;
// then Dst -> Src, and Src -> Arm, which ends at Arm.
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
  build_trans(c, out.src, out.dst, TransKind::External, {});
  build_trans(c, out.dst, out.src, TransKind::External, {});
  build_trans(c, out.src, out.arm, TransKind::External, {});
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
  // On Arm's border is not inside it.
  CHECK(transit_terms(k, { jog_at(200) }).transit_bends == 0);
}

TEST_CASE("cost: a crossing route's bend in a state it only passes through costs") {
  Transit const k{ transit_chart() };
  CostTerms const t{ transit_terms(k, { jog_at(250) }) };
  CHECK(t.bends == 2);
  CHECK(t.transit_bends == 2);
  // Scored on top of the bends themselves.
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
  // Inside the end itself.
  CHECK(transit_terms(k,
                      { { { .x = 100, .y = 130 },
                          { .x = 450, .y = 130 },
                          { .x = 450, .y = 230 },
                          { .x = 400, .y = 230 } } })
            .transit_bends == 0);
  // Src -> Arm ends at the state the other routes pass through, so inside it
  // is inside its own end.
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

TEST_CASE(
    "cost: party marks a bent route, both of a crossing, and neither of a clear one") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  for (uint32_t i = 0; i < 4; ++i) { build_trans(c, a, b, TransKind::External, {}); }

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

TEST_CASE("cost: party marks every transition while the drawing breaks Tier 0") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, b, TransKind::External, {});

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

  // The stranger moved clear leaves both straight, unpriced and unmarked.
  z.state[other.v] = { .x = 150, .y = 3000, .w = 100, .h = 100 };
  t = cost_terms(cost_context(c, g), c, g, z, r, {}, profile(), &party);
  CHECK(cost_of(t, profile()).t0_violations == 0);
  REQUIRE(party.size() == 2);
  CHECK(party[0] == 0);
  CHECK(party[1] == 0);
}

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

TEST_CASE("cost: two routes leaving as one line are charged for it") {
  Chart const c{ edges(2) };
  auto const lines = [](int32_t start_x) {
    return std::vector<std::vector<scav_point>>{
      { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
      { { .x = start_x, .y = 0 }, { .x = 150, .y = 0 }, { .x = 150, .y = -100 } }
    };
  };
  // A fan-out is not the exemption (11.9.3): these two leave one state and
  // part, so sharing the start buys nothing. The run is 150 either way, where
  // one unit of offset used to be free against charged.
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
  // A degraded net is a straight line (11.5); along another's final leg it is
  // the arrival they share, not a second lane.
  CHECK(corridor_of(c,
                    { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
                      { { .x = 200, .y = 0 }, { .x = 200, .y = 100 } } }) == 0);
  // The head is charged now (11.9.3): two routes leaving one state as one line
  // go somewhere ambiguous. The 200 along y=0 is that fan-out.
  CHECK(corridor_of(c,
                    { { { .x = 0, .y = 0 }, { .x = 200, .y = 0 }, { .x = 200, .y = 100 } },
                      { { .x = 0, .y = 0 }, { .x = 200, .y = 0 } } }) == 200);
}

TEST_CASE("cost: a run the two find again after they part is charged") {
  Chart const c{ edges(2) };
  // 100 along y=0 out of the shared start, charged now (11.9.3), and 100 more
  // along y=100 where they meet again -- two lanes on one line either way.
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

TEST_CASE(
    "cost: a route along a state's border is a Tier-0 violation, square off it is not") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 100, .h = 40 };
  z.state[other.v] = { .x = 200, .y = 20, .w = 100, .h = 100 };
  // Along the stranger's top edge: nothing enters it, so only this sees it.
  Routes const along{ routes_of(c, { { { .x = 100, .y = 20 }, { .x = 400, .y = 20 } } }) };
  CostTerms const t{ cost_terms(c, decompose(c), z, along, {}, profile()) };
  CHECK(t.through_box == 0);
  CHECK(t.flush == 1);
  CHECK(cost_of(t, profile()).t0_violations == 1);

  // Clear of it by the band a route keeps inside a state is clear; one unit
  // nearer is on it, because a reader cannot tell a line a unit off a border
  // from the border (11.10g).
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

  // Along its own endpoint's border counts as well: a reader sees the border.
  Routes const own{ routes_of(c,
                              { { { .x = 50, .y = 0 },
                                  { .x = 100, .y = 0 },
                                  { .x = 150, .y = 0 },
                                  { .x = 150, .y = 20 - band },
                                  { .x = 400, .y = 20 - band } } }) };
  CHECK(cost_terms(c, decompose(c), z, own, {}, profile()).flush == 1);
}

TEST_CASE("cost: a route that turns straight back along itself is a Tier-0 violation") {
  // Down to a port and back up the same line reads as two routes meeting, not
  // one turning; a square turn and a U through a jog are both still turns.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
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

TEST_CASE("cost: a route through a region neither end is in is a Tier-0 violation") {
  // `On` holds two concurrent regions side by side, and a transition leaves
  // `Ready` in the left one for `X` outside. Crossing the right region is
  // crossing a state nobody is in; `through_box` excuses it because `On` is
  // an ancestor of the source.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const main_sub{ build_submachine(c, on, "main", {}) };
  SubmachineId const aux_sub{ build_submachine(c, on, "aux", {}) };
  StateId const ready{ build_state(c, main_sub, "Ready", StateKind::Normal, {}) };
  StateId const idle{ build_state(c, aux_sub, "Idle", StateKind::Normal, {}) };
  StateId const x{ build_state(c, root, "X", StateKind::Normal, {}) };
  build_trans(c, ready, x, TransKind::External, {});

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
  CostTerms const t{ cost_terms(c, decompose(c), z, through, {}, profile()) };
  CHECK(t.through_box == 0);
  CHECK(t.through_region == 1);
  CHECK(cost_of(t, profile()).t0_violations == 1);

  // Out of `main` downward and round below `On`: its own region, then outside.
  Routes const round{ routes_of(c,
                                { { { .x = 90, .y = 100 },
                                    { .x = 90, .y = 260 },
                                    { .x = 650, .y = 260 },
                                    { .x = 650, .y = 100 } } }) };
  CHECK(cost_terms(c, decompose(c), z, round, {}, profile()).through_region == 0);
}

TEST_CASE("cost: a placed box over a state neither endpoint is under breaks Tier 0") {
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
  CostTerms const over{ cost_terms(c, decompose(c), z, r, s, profile()) };
  CHECK(over.label_over_box == 1);
  CHECK(over.label == 0);
  CHECK(cost_of(over, profile()).t0_violations == 1);

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
  build_trans(c, outer, a, TransKind::External, {});
  build_trans(c, a, outer, TransKind::External, {});
  build_trans(c, outer, away, TransKind::External, {});

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
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label_over_route == 0);

  r.placed = { { .x = 200, .y = 70, .w = 60, .h = 20 } };
  CostTerms const over{ cost_terms(c, decompose(c), z, r, s, profile()) };
  CHECK(over.label_over_route == 1);
  CHECK(over.label == 0);
  CHECK(cost_of(over, profile()).t0_violations == 1);

  r.placed = { { .x = 200, .y = 82, .w = 60, .h = 20 } };  // beside it, a line clear
  CHECK(cost_terms(c, decompose(c), z, r, s, profile()).label_over_route == 0);
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
  CHECK(scored({ .path_box = &named, .n_path_box = 1 }).label_over_route == 0);
  CHECK(scored({}).label_over_route == 1);
  CHECK(scored({}).label_near == 0);

  // A table too short to reach the box, and one naming a transition that is not
  // there, leave it a stranger's the same way.
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
  // A crossing is a count and 1000 square grid units is a fraction of one em
  // squared, which ceils to one rather than away (11.6).
  CHECK(scored.t2 == ((int64_t{ p.w_crossings } * 2) + int64_t{ p.w_area }));

  // Tier 0 dominates whatever Tier 2 says, because the tiers are compared in
  // order and never summed.
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

  // A count is already a count; a length is ems and an area ems squared, and
  // the division ceils, so one grid unit of either still costs a whole one.
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

  // A whole unit each way, and one grid unit past it.
  CHECK(only(&CostTerms::corridor, em) == int64_t{ p.w_corridor });
  CHECK(only(&CostTerms::corridor, em + 1) == (2 * int64_t{ p.w_corridor }));
  CHECK(only(&CostTerms::length, em) == int64_t{ p.w_length });
  CHECK(only(&CostTerms::length, em + 1) == (2 * int64_t{ p.w_length }));
  CHECK(only(&CostTerms::area, em * em) == int64_t{ p.w_area });
  CHECK(only(&CostTerms::area, (em * em) + 1) == (2 * int64_t{ p.w_area }));
  // Whitespace ships unpriced, so its unit is read at a weight of its own.
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

  // Nothing scored is still nothing, which is what makes an unlaid chart zero.
  CHECK(cost_of(CostTerms{}, p).t2 == 0);
  CHECK(only(&CostTerms::corridor, 0) == 0);
  CHECK(only(&CostTerms::area, 0) == 0);
}

TEST_CASE("cost: the shipped weights sum a hand-built term vector") {
  scav_profile const p{ profile() };
  REQUIRE(p.font_size_grid == 192);  // the ceilings below are read against it
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
  // The smallest font_size_grid the profile's bound allows, which is also the
  // divisor that makes the conversion an identity.
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
  // Floored per term, so a row is at most the whole and short of it by under
  // one basis point per term that scored anything.
  CHECK(total <= 10000);
  CHECK(total > (10000 - static_cast<int64_t>(TIER2_TERMS)));

  for (int64_t const bp : cost_shares(CostTerms{}, p)) { CHECK(bp == 0); }
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
  CHECK(live.label_over_box == 1);

  // A dead submachine's children are no longer siblings of each other.
  c.submachines[dropped.v].live = 0;
  CHECK(scored().box_overlap == 1);

  // And a dead state is no box at all, first or second of its pair.
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

TEST_CASE("cost: a chart that was never laid out scores nothing") {
  // Two siblings and a transition, and not one geometry column: the sibling
  // pairs and the route table are both indexed by ordinal, so scoring this
  // reads past the columns unless the geometry is answered for as a whole.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

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

  // Rects but no route table, which is the same gap one column over: the
  // chart rect is there to be scored and is not, because nothing else is.
  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 0, .w = 100, .h = 40 };
  z.chart = { .x = 0, .y = 0, .w = 400, .h = 40 };
  CostTerms const routeless{ cost_terms(c, decompose(c), z, {}, {}, profile()) };
  CHECK(routeless.area == 0);
  CHECK(routeless.aspect == 0);
  CHECK(routeless.box_overlap == 0);
  CHECK(cost_of(routeless, profile()).t2 == 0);

  // Every rect column is read by entity ordinal, so any one of them stopping
  // short is the same gap: the geometry that scores, minus one column.
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
  build_trans(c, a, b, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 100, .h = 40 };
  z.state[b.v] = { .x = 300, .y = 0, .w = 100, .h = 40 };
  z.chart = { .x = 0, .y = 0, .w = 400, .h = 40 };
  Routes r{ routes_of(c, { { { .x = 100, .y = 20 }, { .x = 300, .y = 20 } } }) };

  // One point more than the route was given, so the last segment would be read
  // from past the end of the point column.
  r.route[0].len += 1;
  CostTerms const past{ cost_terms(c, decompose(c), z, r, {}, profile()) };
  CHECK(past.bends == 0);
  CHECK(past.area == 0);
  CHECK(past.aspect == 0);
  CHECK(cost_of(past, profile()).t2 == 0);

  // The same geometry with the span it actually has, scored as it always was.
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

  // One unit off the ratio is one unit of the term, weighted like any other --
  // and both the deviation and the area are ems before the weight applies.
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
  build_trans(k.c, k.a, k.b, TransKind::External, {});
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
  // Tight: the hole is 220 by 60 and all of it but the gap between A and B is theirs.
  CHECK(composite_terms(k, composite_sizing(k, 240, 130), p).whitespace == 20 * 60);
  CHECK(composite_terms(k, composite_sizing(k, 440, 330), p).whitespace ==
        (420 * 260) - (2 * 100 * 60));

  // A tombstone holds nothing, and a state with no live region is no composite.
  SizedLayout const tight{ composite_sizing(k, 240, 130) };
  k.c.states[k.b.v].live = 0;
  CHECK(composite_terms(k, tight, p).whitespace == (20 * 60) + (100 * 60));
  k.c.states[k.b.v].live = 1;
  k.c.submachines[k.inner.v].live = 0;
  CHECK(composite_terms(k, tight, p).whitespace == 0);
  k.c.submachines[k.inner.v].live = 1;

  // Disjoint inside the chart when Tier 0 holds, so capped at its area.
  SizedLayout small{ tight };
  small.chart = { .x = 0, .y = 0, .w = 10, .h = 10 };
  CHECK(composite_terms(k, small, p).whitespace == 100);
}

TEST_CASE("cost: a padded composite costs more than the same composite tight") {
  Composite const k{ composite_chart() };
  scav_profile p{ profile() };
  REQUIRE(p.font_size_grid == 192);  // the ceilings below are read against it
  p.w_whitespace = 1;
  CostTerms const tight{ composite_terms(k, composite_sizing(k, 240, 130), p) };
  CostTerms const padded{ composite_terms(k, composite_sizing(k, 440, 330), p) };
  CHECK(padded.whitespace > tight.whitespace);
  REQUIRE(padded.area == tight.area);
  REQUIRE(padded.aspect == tight.aspect);
  // One chart either way, so the whole difference is whitespace: 1 em squared against 3.
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

  // The two comparisons answer what the climb answers, every way round.
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
  build_trans(c, a, b, TransKind::External, {});
  // The span stops one short of `Lost`, so the walk reaches neither it nor what
  // it holds; `parent` still says where the two of them are.
  --c.submachines[root.v].children.len;

  Ancestry const an{ cost_flatten_ancestry(c) };
  CHECK(an.tin[lost.v] == 0);
  CHECK(an.tin[below.v] == 0);
  CHECK(an.detached.size() == 2);
  CHECK(cost_ancestor(c, an, lost, below));
  CHECK_FALSE(cost_ancestor(c, an, below, lost));

  // Both are off the descent, so Tier 0 tests them where they stand.
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
  build_trans(c, a, b, TransKind::External, {});
  c.states[gone.v].live = 0;

  Ancestry const an{ cost_flatten_ancestry(c) };
  CHECK(an.detached.size() == 1);
  CHECK(an.detached[0] == hidden.v);

  SizedLayout z{ blank(c) };
  z.state[a.v] = { .x = 0, .y = 0, .w = 20, .h = 20 };
  z.state[b.v] = { .x = 400, .y = 0, .w = 20, .h = 20 };
  // The tombstone's rect is zero, so it says nothing about where Hidden sits.
  z.state[hidden.v] = { .x = 150, .y = -50, .w = 100, .h = 100 };
  Routes const r{ routes_of(c, { { { .x = 10, .y = 10 }, { .x = 410, .y = 10 } } }) };
  CostTerms const t{ cost_terms(c, decompose(c), z, r, {}, profile()) };
  CHECK(t.through_box == 1);
  CHECK(t.box_overlap == 0);  // the tombstone is not a box, and Hidden is alone
}

namespace {

// `n` boxes on a diagonal and one over all of them, so the frame's grid is
// more than one cell and one child sits in every cell of it.
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

  // Moved off them, `bar` meets nothing, and the others never meet each other.
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

// 11.8's shape: one state holding two concurrent regions, each with a child,
// and the two endpoints of the transition outside it. Every child rect lies
// inside its parent's, which is what the descent prunes on.
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
  build_trans(c, out.src, out.dst, TransKind::External, {});

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
  // The transition now runs out of the first region, so the owner encloses its
  // source and is carved out -- and the sibling region's child is not.
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
  build_trans(c, src, dst, TransKind::External, {});

  SizedLayout z{ blank(c) };
  z.state[outer.v] = { .x = 0, .y = 0, .w = 400, .h = 200 };
  z.state[src.v] = { .x = 20, .y = 60, .w = 60, .h = 80 };
  z.state[stranger.v] = { .x = 150, .y = 60, .w = 60, .h = 80 };
  z.state[dst.v] = { .x = 600, .y = 80, .w = 40, .h = 40 };
  // Out of the source, through its sibling, and out of the composite the two
  // of them share: three boxes entered, one of them charged.
  Routes const route{ routes_of(c,
                                { { { .x = 80, .y = 100 }, { .x = 600, .y = 100 } } }) };
  CHECK(cost_terms(c, decompose(c), z, route, {}, profile()).through_box == 1);
}

TEST_CASE("cost: a degraded diagonal crosses what the axis-aligned sweep cannot") {
  // The band sweep pairs a vertical with the horizontals inside its span and
  // never two of a kind; a diagonal is in neither set and goes against all.
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

  // Two horizontals on one line are collinear, and a pair of one transition's
  // own legs is not a crossing however they meet.
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
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, b, TransKind::External, {});

  // One bucket, the horizontals at y = 0, sharing [50, 100]; the two verticals
  // sit at different x and so in buckets of their own.
  Routes const apart{ routes_of(
      c,
      { { { .x = 0, .y = 0 }, { .x = 100, .y = 0 }, { .x = 100, .y = 40 } },
        { { .x = 50, .y = 0 }, { .x = 150, .y = 0 }, { .x = 150, .y = 40 } } }) };
  CHECK(cost_corridor(apart, pieces_of(apart)) == 50);

  // The same run, with the two finishing at one point: that is the trunk.
  Routes const merged{ routes_of(c,
                                 { { { .x = 0, .y = 0 }, { .x = 150, .y = 0 } },
                                   { { .x = 50, .y = 0 }, { .x = 150, .y = 0 } } }) };
  CHECK(cost_corridor(merged, pieces_of(merged)) == 0);

  // Perpendicular legs share no line at all, whatever they touch.
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
  // Just inside an em apart charges almost nothing; an em apart, nothing at all.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 191, 1000, 191, 1) }, em) == 5);
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 192, 1000, 192, 1) }, em) == 0);
  // Verticals are the same question on the other axis.
  CHECK(cost_crowding({ seg(0, 0, 0, 1000, 0), seg(48, 0, 48, 1000, 1) }, em) == 750);
  // The overlap is what is charged, not either length.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(600, 96, 3000, 96, 1) }, em) == 200);
}

TEST_CASE("cost: crowding leaves what is not a tight lane to the terms that price it") {
  int32_t const em{ 192 };
  // Collinear is `corridor`'s, so the two terms never charge one pair twice.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 0, 1000, 0, 1) }, em) == 0);
  // One transition's own two legs are one route, not two lanes.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 96, 1000, 96, 0) }, em) == 0);
  // Perpendicular is a crossing or nothing.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(500, 50, 500, 900, 1) }, em) == 0);
  // Parallel and tight but not alongside: end to end, or merely touching.
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(2000, 96, 3000, 96, 1) }, em) == 0);
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(1000, 96, 3000, 96, 1) }, em) == 0);
  // A degenerate piece has no axis; a zero em prices nothing rather than dividing.
  CHECK(cost_crowding({ seg(5, 5, 5, 5, 0), seg(5, 50, 900, 50, 1) }, em) == 0);
  CHECK(cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, 96, 1000, 96, 1) }, 0) == 0);
}

TEST_CASE("cost: crowding is continuous with corridor at the line they share") {
  // The design claim of 11.6, pinned: as two lanes close to one line, crowding
  // approaches the shared length `corridor` would charge them at zero.
  int32_t const em{ 192 };
  Wide last{ 0 };
  for (int32_t apart = 191; apart >= 1; --apart) {
    Wide const now{ cost_crowding({ seg(0, 0, 1000, 0, 0), seg(0, apart, 1000, apart, 1) },
                                  em) };
    CHECK(now >= last);  // closer never charges less
    last = now;
  }
  CHECK(last == 994);  // one unit apart: 1000 * 191 / 192, a hair short of 1000
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
  // Every Tier-2 term scores an undrawn route perfect -- no bends, no length, no
  // excess, no crowding -- so a search that can reach one prefers it. Seen:
  // a composite's transition to its own child, collapsed onto one point by an
  // arrangement that put the child flush against the composite's wall (11.6).
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, a, TransKind::External, {});

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

  // One collapsed to a single point: it scores nothing in Tier 2, which is the
  // trap, and Tier 0 is what refuses it.
  Routes const collapsed{ routes_of(
      c,
      { { { .x = 100, .y = 10 }, { .x = 300, .y = 10 } }, { { .x = 300, .y = 30 } } }) };
  CostTerms const t{ cost_terms(c, decompose(c), z, collapsed, {}, p) };
  CHECK(t.vanished == 1);
  CHECK(cost_of(t, p).t0_violations == 1);
  CHECK(
      cost_less(cost_of(cost_terms(c, decompose(c), z, drawn, {}, p), p), cost_of(t, p)));

  // And none at all is the same defect.
  Routes const empty{
    routes_of(c, { { { .x = 100, .y = 10 }, { .x = 300, .y = 10 } }, {} })
  };
  CHECK(cost_terms(c, decompose(c), z, empty, {}, p).vanished == 1);
}

namespace {

// Every term by brute force over every pair, the oracle for the indexed scorer. It
// shares only `overlaps`, `along_border`, `chebyshev_gap` and the containment walk.
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

Trunk trunk_of(std::vector<scav_point> const &pts, scav_span a, scav_span b) {
  Trunk out;
  uint32_t const shortest{ imin(imin(a.len, b.len), 2U) };
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

int32_t through_boxes(Chart const &c,
                      SizedLayout const &z,
                      Ancestry const &an,
                      std::vector<Piece> const &pieces) {
  int32_t total{ 0 };
  for (Piece const &piece : pieces) {
    Transition const &tr{ c.transitions[piece.trans] };
    scav_rect const reach{ span_rect(piece.a, piece.b) };
    auto const charge = [&](uint32_t st) {
      if (enters(piece.a, piece.b, z.state[st]) && !cost_ancestor(c, an, { st }, tr.src) &&
          !cost_ancestor(c, an, { st }, tr.dst)) {
        ++total;
      }
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
  }
  return total;
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
  }
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    if ((tr < g.trans_segments.size()) && (g.trans_segments[tr].len != 0) &&
        (r.route[tr].len < 2)) {
      ++t.vanished;
    }
  }
  t.crossings = crossings(pieces, crossings_of);
  t.corridor = corridor(r, pieces);
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
      if ((tr.src == tr.dst) && (tr.kind != TransKind::External)) { host = tr.src.v; }
    }
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if (c.states[st].live == 0) { continue; }
      // A band with no extent is no band.
      auto const wall = [&](std::vector<scav_rect> const &v) {
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
      if (piece.trans == subject) {
        own = (own < 0) ? away : imin(own, away);
        continue;
      }
      other = (other < 0) ? away : imin(other, away);
      if (overlaps(r.placed[i], seg)) { ++t.label_over_route; }
    }
    if ((own >= 0) && (other >= 0)) {
      Wide const shortfall{ (own + height) - other };
      if (shortfall > 0) { t.label_near += shortfall; }
    }
    if (subject != INVALID) { mark(c.transitions[subject].src, 0); }
  }

  for (SplitSegment const &seg : g.segments) {
    if (seg.separator == 0) { continue; }
    Transition const &tr{ c.transitions[seg.trans.v] };
    StateKind const src_kind{ c.states[tr.src.v].kind };
    StateKind const dst_kind{ c.states[tr.dst.v].kind };
    if ((src_kind == StateKind::Fork) || (src_kind == StateKind::Join) ||
        (dst_kind == StateKind::Fork) || (dst_kind == StateKind::Join)) {
      continue;
    }
    SubmachineId const from{ g.ports[seg.src_port].sub };
    SubmachineId const to{ g.ports[seg.dst_port].sub };
    if ((from.v == INVALID) || (to.v == INVALID)) { continue; }
    if (!adjacent(z.sub[from.v], z.sub[to.v], p.sub_sep)) { ++t.adjacency; }
  }

  Ancestry const an{ cost_flatten_ancestry(c) };
  t.box_overlap = box_overlaps(c, z);
  t.through_box = through_boxes(c, z, an, pieces);
  for (Piece const &piece : pieces) {
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if ((c.states[st].live != 0) &&
          along_border(piece.a, piece.b, z.state[st], border_band(p) - 1)) {
        ++t.flush;
        break;
      }
    }
  }
  for (Piece const &piece : pieces) {
    Transition const &trans{ c.transitions[piece.trans] };
    scav_rect const reach{ span_rect(piece.a, piece.b) };
    for (uint32_t m = 0; m < c.submachines.size(); ++m) {
      if ((c.submachines[m].live == 0) || (c.submachines[m].owner.v == INVALID)) {
        continue;
      }
      scav_rect const &region{ z.sub[m] };
      if (!overlaps(reach, grow(region, 1)) || !enters(piece.a, piece.b, region)) {
        continue;
      }
      if (within(c, trans.src, m) || within(c, trans.dst, m)) { continue; }
      ++t.through_region;
      break;
    }
  }
  return t;
}

}  // namespace reference

constexpr uint32_t TERMS{ 18 };

std::array<int64_t, TERMS> terms_of(CostTerms const &t) {
  return { t.bends,    t.corridor,      t.crossings,     t.excess_len,  t.adjacency,
           t.label,    t.label_near,    t.aspect,        t.area,        t.crowding,
           t.length,   t.transit_bends, t.whitespace,    t.through_box, t.box_overlap,
           t.vanished, t.flush,         t.through_region };
}

constexpr std::array<char const *, TERMS> TERM_NAMES{
  "bends",      "corridor",    "crossings",   "excess_len", "adjacency", "label",
  "label_near", "aspect",      "area",        "crowding",   "length",    "transit_bends",
  "whitespace", "through_box", "box_overlap", "vanished",   "flush",     "through_region"
};

// The first term the two disagree on, or empty.
std::string first_difference(CostTerms const &got, CostTerms const &want) {
  std::array<int64_t, TERMS> const a{ terms_of(got) };
  std::array<int64_t, TERMS> const b{ terms_of(want) };
  for (uint32_t i = 0; i < TERMS; ++i) {
    if (a[i] != b[i]) { return TERM_NAMES[i]; }
  }
  if (got.retrace != want.retrace) { return "retrace"; }
  if (got.label_over_box != want.label_over_box) { return "label_over_box"; }
  if (got.label_over_route != want.label_over_route) { return "label_over_route"; }
  return {};
}

// Coordinates on a lattice of five with an occasional unit nudge, straddling zero, so
// shared borders, collinear legs and negative lane keys are common.
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
    build_trans(c, src, dst, TransKind::External, {});
  }
  for (StateId const st : states) {
    if (dead[st.v] != 0) { c.states[st.v].live = 0; }
  }
  for (uint32_t m = 1; m < c.submachines.size(); ++m) {
    if (r.next(12) == 0) { c.submachines[m].live = 0; }
  }
  return c;
}

// One candidate's geometry over `c`, all drawn at random; some routes end on another's
// last points, so trunks come up.
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

// `k` moved by `by` on both axes. The lattice straddles zero, so a shift changes which
// lane-key bytes agree and so which sort passes run.
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
  // One composite holding two siblings and a region, and one state outside it,
  // so a label can sit in the composite both ends share.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "Outer", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, "main", {}) };
  StateId const a{ build_state(c, inner, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, inner, "B", StateKind::Normal, {}) };
  StateId const far{ build_state(c, root, "Far", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, far, TransKind::External, {});
  SplitGraph const g{ decompose(c) };
  CostContext const ctx{ cost_context(c, g) };
  // A band of five and an em of twenty.
  scav_profile p{ profile() };
  p.node_sep = 30;
  p.pad = 12;
  p.font_size_grid = 20;
  int32_t const near{ border_band(p) - 1 };
  REQUIRE(near == 4);

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

  // Along A's top border out to just past the band, each on a cell edge of the grid of
  // grown states; then a lone point on the border.
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
  // pairs grow and shrink every buffer between calls.
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

TEST_CASE("cost: a context built once scores every candidate as one built for it") {
  // The context also comes out as it went in.
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
