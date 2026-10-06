// The trace sink's frame stamping, scoping and JSON output, then traced layout runs.

#include "layout/trace.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scav {
void layout_test_skip_noop_faces(bool on);
uint64_t layout_test_noop_faces();
}  // namespace scav

namespace {

using namespace scav;

// Attaches `t` as the trace sink for its scope and clears the sink on exit.
struct Attached {
  explicit Attached(LayoutTrace &t) { trace_sink_set(&t); }
  ~Attached() { trace_sink_set(nullptr); }
  Attached(Attached const &) = delete;
  Attached &operator=(Attached const &) = delete;
};

std::string json_of(LayoutTrace const &t, Chart const &c) {
  std::vector<char> out;
  trace_to_json(t, c, out);
  return { out.begin(), out.end() };
}

uint32_t count_kind(LayoutTrace const &t, TraceKind k) {
  uint32_t n{ 0 };
  for (TraceEvent const &e : t.events) {
    if (e.kind == k) { ++n; }
  }
  return n;
}

// One Level 1 round: a maximal run of score events. `orderings` counts the EdgeReversed
// events since the previous round, one per ordering on a one-cycle chart.
struct Round {
  uint32_t orderings{ 0 };
  uint32_t scored{ 0 };
  uint32_t first{ 0 };  // event index of its first score
  uint32_t end{ 0 };
};

std::vector<Round> rounds_of(LayoutTrace const &t) {
  std::vector<Round> out;
  uint32_t orderings{ 0 };
  bool open{ false };
  for (uint32_t i = 0; i < t.events.size(); ++i) {
    TraceEvent const &e{ t.events[i] };
    if ((e.kind != TraceKind::CandidateScored) && (e.kind != TraceKind::CandidateTerms)) {
      open = false;
      orderings += (e.kind == TraceKind::EdgeReversed) ? 1U : 0U;
      continue;
    }
    if (!open) {
      out.push_back({ .orderings = orderings, .scored = 0, .first = i, .end = i });
      orderings = 0;
      open = true;
    }
    out.back().end = i + 1;
    out.back().scored += (e.kind == TraceKind::CandidateScored) ? 1U : 0U;
  }
  return out;
}

}  // namespace

TEST_CASE("trace: no sink is the shipped state, and emitting into one is a no-op") {
  CHECK(trace_sink() == nullptr);
  trace_emit({ .kind = TraceKind::RankAssigned, .rank = { .state = 0, .rank = 0 } });
  CHECK(trace_sink() == nullptr);
}

TEST_CASE("trace: the sink stamps its frame, and an explicit one wins") {
  LayoutTrace t;
  Attached const held{ t };
  t.frame = 7;
  trace_emit({ .kind = TraceKind::EdgeReversed, .seg = { .seg = 1 } });
  trace_emit({ .kind = TraceKind::EdgeReversed, .frame = 9, .seg = { .seg = 2 } });
  REQUIRE(t.events.size() == 2);
  CHECK(t.events[0].frame == 7);
  CHECK(t.events[1].frame == 9);
}

TEST_CASE("trace: a frame scope restores its caller's, including from INVALID") {
  LayoutTrace t;
  Attached const held{ t };
  CHECK(t.frame == INVALID);
  {
    TraceFrame const outer{ SubmachineId{ 2 } };
    CHECK(t.frame == 2);
    {
      TraceFrame const inner{ SubmachineId{ 5 } };
      CHECK(t.frame == 5);
    }
    CHECK(t.frame == 2);
  }
  CHECK(t.frame == INVALID);
}

TEST_CASE("trace: a frame scope with no sink attached touches nothing") {
  REQUIRE(trace_sink() == nullptr);
  TraceFrame const scoped{ SubmachineId{ 3 } };
  CHECK(trace_sink() == nullptr);
}

TEST_CASE("trace: an empty trace serializes to an empty array") {
  Chart const c;
  CHECK(json_of({}, c) == "[\n]\n");
}

TEST_CASE("trace: a state is named, a nameless one is its ordinal, a stranger is null") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "Alpha", StateKind::Normal, {}) };
  StateId const anon{ build_state(c, root, "", StateKind::Initial, {}) };

  LayoutTrace t;
  t.events.push_back(
      { .kind = TraceKind::RankAssigned, .rank = { .state = a.v, .rank = 4 } });
  t.events.push_back(
      { .kind = TraceKind::RankAssigned, .rank = { .state = anon.v, .rank = 0 } });
  t.events.push_back(
      { .kind = TraceKind::RankAssigned, .rank = { .state = 99, .rank = 0 } });
  t.events.push_back(
      { .kind = TraceKind::RankAssigned, .rank = { .state = INVALID, .rank = 0 } });

  std::string const out{ json_of(t, c) };
  CHECK(out.find("\"state_id\":" + std::to_string(a.v) +
                 ",\"state\":\"Alpha\",\"rank\":4") != std::string::npos);
  CHECK(out.find("\"state\":\"#" + std::to_string(anon.v) + "\"") != std::string::npos);
  CHECK(out.find("\"state\":null") != std::string::npos);
  // The out-of-range ordinal and INVALID both print null.
  CHECK(out.find("\"state\":null") != out.rfind("\"state\":null"));
}

TEST_CASE("trace: every payload shape serializes its own fields") {
  Chart const c;
  LayoutTrace t;
  t.events.push_back({ .kind = TraceKind::EdgeChained,
                       .chain = { .seg = 3, .rank = 2, .index = 1, .count = 1 } });
  t.events.push_back({ .kind = TraceKind::NodePlaced,
                       .place = { .state = INVALID, .seg = 3, .x = 2823, .y = 1489 } });
  t.events.push_back({ .kind = TraceKind::SpacingInflated,
                       .inflate = { .node_sep = 320, .rank_sep = 640 } });
  t.events.push_back({ .kind = TraceKind::NetPlanned,
                       .net = { .seg = 3,
                                .trans = 3,
                                .waypoints = 1,
                                .sx = 1997,
                                .sy = 2469,
                                .dx = 1344,
                                .dy = 509 } });
  t.events.push_back({ .kind = TraceKind::NetWaypoint, .point = { .x = -8, .y = 1489 } });
  t.events.push_back(
      { .kind = TraceKind::SeatMoved,
        .pass = static_cast<uint16_t>(SeatPass::Separate),
        .seat = { .net = 2, .end = 1, .from_x = 1, .from_y = 2, .to_x = 3, .to_y = 4 } });
  t.events.push_back(
      { .kind = TraceKind::LaneAssigned, .lane = { .net = 5, .lane = 1, .at = 96 } });
  t.events.push_back({ .kind = TraceKind::LaneFound,
                       .found = { .horizontal = 1,
                                  .at = -40,
                                  .members = 3,
                                  .bundles = 2,
                                  .merged = 1,
                                  .reordered = 1,
                                  .spread = 0 } });
  t.events.push_back({ .kind = TraceKind::BundleRefused,
                       .bundle = { .net = 4, .lane = 0, .members = 2, .to = 76 } });
  t.events.push_back({ .kind = TraceKind::CandidateScored,
                       .pass = static_cast<uint16_t>(MoveVerdict::NotBetter),
                       .score = { .row = INVALID,
                                  .state = INVALID,
                                  .rank = 0,
                                  .t0 = 0,
                                  .t2 = 4294967296 } });
  t.events.push_back({ .kind = TraceKind::GapCharged,
                       .pass = static_cast<uint16_t>(GapCause::Lanes),
                       .gap = { .boundary = 2, .seg = 7, .width = 538 } });
  t.events.push_back({ .kind = TraceKind::RowSearched,
                       .pass = static_cast<uint16_t>(RowPass::Refold),
                       .search = { .row = 3, .of = INVALID, .t0 = 1, .t2 = 2461 } });
  t.events.push_back({ .kind = TraceKind::RowRepeated, .search = { .row = 5, .of = 1 } });
  t.events.push_back({ .kind = TraceKind::KickScored,
                       .pass = static_cast<uint16_t>(KickVerdict::Improves),
                       .frame = 2,
                       .search = { .row = 1,
                                   .of = INVALID,
                                   .move = TRACE_MOVE_REVERSE,
                                   .trans = 6,
                                   .leg = 1,
                                   .t0 = 0,
                                   .framed_t0 = 2,
                                   .t2 = 1495,
                                   .framed = 3715 } });
  t.events.push_back({ .kind = TraceKind::KickScored,
                       .pass = static_cast<uint16_t>(KickVerdict::NotBetter),
                       .search = { .row = 0, .move = TRACE_MOVE_ORIENT, .t2 = 7 } });
  t.events.push_back({ .kind = TraceKind::KickTaken,
                       .pass = static_cast<uint16_t>(KickHow::Stacked),
                       .search = { .row = 1, .t0 = 0, .t2 = 1441 } });

  std::string const out{ json_of(t, c) };
  CHECK(out.find("\"seg\":3,\"rank\":2,\"index\":1,\"count\":1") != std::string::npos);
  CHECK(out.find("\"bend_of_seg\":3,\"at\":[2823,1489]") != std::string::npos);
  CHECK(out.find("\"node_sep\":320,\"rank_sep\":640") != std::string::npos);
  CHECK(out.find("\"waypoints\":1,\"src\":[1997,2469],\"dst\":[1344,509]") !=
        std::string::npos);
  CHECK(out.find("\"at\":[-8,1489]") != std::string::npos);  // a negative coordinate
  CHECK(out.find("\"pass\":\"separate\",\"net\":2,\"end\":\"dst\"") != std::string::npos);
  CHECK(out.find("\"net\":5,\"lane\":1,\"at\":96") != std::string::npos);
  CHECK(out.find("\"kind\":\"lane_found\",\"horizontal\":1,\"at\":-40,\"members\":3,"
                 "\"bundles\":2,\"merged\":1,\"reordered\":1,\"spread\":0") !=
        std::string::npos);
  CHECK(out.find(
            "\"kind\":\"bundle_refused\",\"net\":4,\"lane\":0,\"members\":2,\"to\":76") !=
        std::string::npos);
  // `t2` prints past 32 bits; an INVALID row prints unsigned, as 4294967295.
  CHECK(out.find("\"t2\":4294967296") != std::string::npos);
  CHECK(out.find("\"verdict\":\"not_better\",\"row\":4294967295") != std::string::npos);
  CHECK(out.find("\"cause\":\"lanes\",\"boundary\":2,\"seg\":7,\"width\":538") !=
        std::string::npos);
  CHECK(out.find("\"kind\":\"row_searched\",\"pass\":\"refold\",\"row\":3,\"t0\":1,"
                 "\"t2\":2461") != std::string::npos);
  CHECK(out.find("\"kind\":\"row_repeated\",\"row\":5,\"of\":1") != std::string::npos);
  CHECK(out.find("\"kind\":\"kick_scored\",\"frame\":2,\"verdict\":\"improves\",\"row\":1,"
                 "\"move\":\"reverse\",\"trans\":6,\"leg\":1,\"t0\":0,\"t2\":1495,"
                 "\"framed_t0\":2,\"framed\":3715") != std::string::npos);
  // An orientation names no transition.
  CHECK(out.find("\"verdict\":\"not_better\",\"row\":0,\"move\":\"orient\",\"t0\":0,"
                 "\"t2\":7") != std::string::npos);
  CHECK(out.find("\"kind\":\"kick_taken\",\"how\":\"stacked\",\"row\":1,\"t0\":0,"
                 "\"t2\":1441") != std::string::npos);
  // No comma after the last object.
  CHECK(out.find("},\n]") == std::string::npos);
}

TEST_CASE("trace: a traced run writes the geometry an untraced one does") {
  Chart traced;
  Chart plain;
  for (Chart *c : { &traced, &plain }) {
    SubmachineId const root{ build_chart(*c, "t", {}) };
    StateId const a{ build_state(*c, root, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(*c, root, "B", StateKind::Normal, {}) };
    StateId const d{ build_state(*c, root, "C", StateKind::Normal, {}) };
    build_trans(*c, a, b, TransKind::Default, {});
    build_trans(*c, b, d, TransKind::Default, {});
    build_trans(*c, d, a, TransKind::Default, {});  // the back edge that chains
  }

  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  std::vector<scav_placed> pa;
  std::vector<scav_placed> pb;
  std::vector<Diagnostic> da;
  std::vector<Diagnostic> db;
  std::vector<char> events;
  REQUIRE(layout_trace_json(traced, {}, opts, pa, da, events, INVALID));
  REQUIRE(layout_run(plain, {}, opts, pb, db, nullptr, nullptr, INVALID));

  CHECK(layout_structural_hash(traced) == layout_structural_hash(plain));
  CHECK(layout_coordinate_hash(traced) == layout_coordinate_hash(plain));
  CHECK(!events.empty());
  CHECK(trace_sink() == nullptr);  // the run detaches its sink on return
}

TEST_CASE("trace: a fold pin names the frame it decides and the mode it takes") {
  // A five-state chain: the scale measure folds it; a FOLD_NEVER pin keeps it one row.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> run;
  for (char const *name : { "A", "B", "C", "D", "E" }) {
    run.push_back(build_state(c, root, name, StateKind::Normal, {}));
  }
  for (uint32_t k = 0; (k + 1) < run.size(); ++k) {
    build_trans(c, run[k], run[k + 1], TransKind::Default, {});
  }
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.profile.portfolio_k = 0;
  opts.threads = 1;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  LayoutTrace bare;
  {
    Attached const held{ bare };
    REQUIRE(layout_run(c, {}, opts, placed, diags, nullptr, nullptr, 0));
  }
  CHECK(count_kind(bare, TraceKind::FoldCut) > 0);
  CHECK(count_kind(bare, TraceKind::FoldPinned) == 0);

  SearchPins const pins{ .folds = { { .frame = root, .mode = FOLD_NEVER } } };
  LayoutTrace t;
  {
    Attached const held{ t };
    REQUIRE(layout_run(c,
                       {},
                       opts,
                       placed,
                       diags,
                       nullptr,
                       nullptr,
                       0,
                       nullptr,
                       nullptr,
                       &pins));
  }
  CHECK(count_kind(t, TraceKind::FoldCut) == 0);
  REQUIRE(count_kind(t, TraceKind::FoldPinned) == 1);
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::FoldPinned) { continue; }
    CHECK(e.frame == root.v);
    CHECK(e.pass == FOLD_NEVER);
  }
  CHECK(json_of(t, c).find("\"kind\":\"fold_pinned\",\"frame\":0,\"mode\":\"never\"") !=
        std::string::npos);
}

TEST_CASE("trace: a back edge's kinks are one chained bend, and the trace says so") {
  // A -> B -> C -> A ranks the three in a row; the back edge spans two ranks, chains
  // through a bend in B's rank, and the router gets that bend as a waypoint.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, d, TransKind::Default, {});
  TransId const back{ build_trans(c, d, a, TransKind::Default, {}) };

  LayoutTrace t;
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.threads = 1;
  // Level 1 off, and the row argument 0 pins the Level 2 tuple: phase 1 orders once.
  opts.profile.portfolio_k = 0;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  {
    Attached const held{ t };
    REQUIRE(layout_run(c, {}, opts, placed, diags, nullptr, nullptr, 0));
  }
  CHECK(count_kind(t, TraceKind::CandidateScored) == 0);

  // A, B and C take increasing ranks.
  std::array<uint32_t, 3> ranks{ INVALID, INVALID, INVALID };
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::RankAssigned) { continue; }
    for (uint32_t i = 0; i < 3; ++i) {
      if (e.rank.state == (StateId{ a.v + i }).v) { ranks[i] = e.rank.rank; }
    }
  }
  CHECK(ranks[0] < ranks[1]);
  CHECK(ranks[1] < ranks[2]);

  // One bend, at B's rank, and one reversal.
  CHECK(count_kind(t, TraceKind::EdgeChained) == 1);
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::EdgeChained) { continue; }
    CHECK(e.chain.rank == ranks[1]);
    CHECK(e.chain.count == 1);
  }
  CHECK(count_kind(t, TraceKind::EdgeReversed) == 1);

  // Only the back edge's net carries a waypoint, and it carries one.
  uint32_t carrying{ 0 };
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::NetPlanned) { continue; }
    if (e.net.waypoints == 0) { continue; }
    ++carrying;
    CHECK(e.net.trans == back.v);
    CHECK(e.net.waypoints == 1);
  }
  CHECK(carrying == 1);
  CHECK(count_kind(t, TraceKind::NetWaypoint) == 1);

  // The waypoint equals the bend's placed coordinate.
  scav_point bend{ .x = INT32_MIN, .y = INT32_MIN };
  for (TraceEvent const &e : t.events) {
    if ((e.kind == TraceKind::NodePlaced) && (e.place.state == INVALID)) {
      bend = { .x = e.place.x, .y = e.place.y };
    }
    if (e.kind != TraceKind::NetWaypoint) { continue; }
    CHECK(e.point.x == bend.x);
    CHECK(e.point.y == bend.y);
  }
}

TEST_CASE("trace: a label's charge names its frame, its boundary and its segment") {
  // D -> C/T splits in two pieces, the first in the root; the root's label charge is on
  // the D/C boundary, from the first piece, at the box's width.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const t{ build_state(c, inner, "T", StateKind::Normal, {}) };
  TransId const into{ build_trans(c, d, t, TransKind::Default, {}) };
  scav_path_box const box{ .subject = into.v, .w = 1511, .h = 269, .order = 0 };
  scav_spaces const s{ .path_box = &box, .n_path_box = 1 };

  LayoutTrace trace;
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.threads = 1;
  opts.profile.portfolio_k = 0;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  {
    Attached const held{ trace };
    REQUIRE(layout_run(c, s, opts, placed, diags, nullptr, nullptr, 0));
  }
  uint32_t labels{ 0 };
  for (TraceEvent const &e : trace.events) {
    if ((e.kind != TraceKind::GapCharged) ||
        (static_cast<GapCause>(e.pass) != GapCause::Label)) {
      continue;
    }
    ++labels;
    CHECK(e.frame == root.v);
    CHECK(e.gap.boundary == 0);
    CHECK(e.gap.seg == 0);  // the transition's first segment, the root's
    CHECK(e.gap.width == 1511);
  }
  // One charge per phase-1 run.
  CHECK(labels >= 1);
}

TEST_CASE("trace: the search re-orders per move, and every move states its verdict") {
  // The same chart with Level 1 on: each scored move runs a fresh phase 1.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, d, TransKind::Default, {});
  TransId const back{ build_trans(c, d, a, TransKind::Default, {}) };

  LayoutTrace t;
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.profile.search_cull = 0;  // the full search
  opts.threads = 1;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  {
    Attached const held{ t };
    REQUIRE(layout_run(c, {}, opts, placed, diags, nullptr, nullptr, 0));
  }

  // Every verdict is one of the four, and rejected moves are traced too.
  uint32_t const scored{ count_kind(t, TraceKind::CandidateScored) };
  uint32_t taken{ 0 };
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::CandidateScored) { continue; }
    CHECK(e.pass <= static_cast<uint16_t>(MoveVerdict::NotBetter));
    // A placement names a state and no transition; every other move names a transition
    // and no state.
    CHECK(e.score.move <= TRACE_MOVE_FACE);
    CHECK((e.score.state == INVALID) == (e.score.move != TRACE_MOVE_RANK));
    CHECK((e.score.trans == INVALID) == (e.score.move == TRACE_MOVE_RANK));
    taken += (e.pass == static_cast<uint16_t>(MoveVerdict::Taken)) ? 1U : 0U;
  }
  CHECK(scored > 0);
  CHECK(taken < scored);  // at least one move is rejected

  // Each round reorders once per scored move plus once before it, twice before the first.
  std::vector<Round> const rounds{ rounds_of(t) };
  REQUIRE(!rounds.empty());
  for (uint32_t k = 0; k < rounds.size(); ++k) {
    CAPTURE(k);
    CHECK(rounds[k].orderings >= (rounds[k].scored + ((k == 0) ? 2U : 1U)));
  }

  // Each ordering of the three-cycle gives at most one net a waypoint, and some
  // ordering gives one.
  (void)back;
  uint32_t planned{ 0 };
  uint32_t carrying{ 0 };
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::NetPlanned) { continue; }
    ++planned;
    carrying += (e.net.waypoints != 0) ? 1U : 0U;
  }
  CHECK(planned > 0);
  CHECK(carrying > 0);
  CHECK(carrying * 3 <= planned);  // three nets an ordering, at most one bent
}

TEST_CASE("trace: a reversal is offered only on a segment that lies on a cycle") {
  // A cycle `A -> B -> C -> A` with a tail `C -> D`.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  StateId const tail{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, d, TransKind::Default, {});
  build_trans(c, d, a, TransKind::Default, {});
  TransId const off{ build_trans(c, d, tail, TransKind::Default, {}) };

  LayoutTrace t;
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.threads = 1;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  {
    Attached const held{ t };
    REQUIRE(layout_run(c, {}, opts, placed, diags, nullptr, nullptr, 0));
  }
  uint32_t reversals{ 0 };
  for (TraceEvent const &e : t.events) {
    if ((e.kind != TraceKind::CandidateScored) || (e.score.move != TRACE_MOVE_REVERSE)) {
      continue;
    }
    ++reversals;
    CHECK(e.score.trans != off.v);
  }
  CHECK(reversals > 0);
}

TEST_CASE("trace: a face is offered only where its transition bends or is priced") {
  // `A -> B` routes straight and uncharged; of `C -> D -> E` and `C -> E`, one passes D
  // and some route of the three bends.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  StateId const e{ build_state(c, root, "D", StateKind::Normal, {}) };
  StateId const f{ build_state(c, root, "E", StateKind::Normal, {}) };
  TransId const straight{ build_trans(c, a, b, TransKind::Default, {}) };
  build_trans(c, d, e, TransKind::Default, {});
  build_trans(c, e, f, TransKind::Default, {});
  build_trans(c, d, f, TransKind::Default, {});

  LayoutTrace t;
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.threads = 1;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  layout_test_skip_noop_faces(true);
  {
    Attached const held{ t };
    REQUIRE(layout_run(c, {}, opts, placed, diags, nullptr, nullptr, 0));
  }
  CHECK(layout_test_noop_faces() > 0);  // faces with no effect are skipped and counted
  uint32_t faced{ 0 };
  uint32_t ranked{ 0 };
  for (TraceEvent const &ev : t.events) {
    if (ev.kind != TraceKind::CandidateScored) { continue; }
    ranked += (ev.score.move == TRACE_MOVE_RANK) ? 1U : 0U;
    if (ev.score.move != TRACE_MOVE_FACE) { continue; }
    CHECK(ev.score.trans != straight.v);
    ++faced;
  }
  CHECK(ranked > 0);  // the search scored placement moves
  CHECK(faced > 0);
}

TEST_CASE("trace: the search scores unchain moves beside placement moves") {
  // A -> B -> C -> A: the back edge is the one chained segment.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, d, TransKind::Default, {});
  TransId const back{ build_trans(c, d, a, TransKind::Default, {}) };

  LayoutTrace t;
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.threads = 1;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  {
    Attached const held{ t };
    REQUIRE(layout_run(c, {}, opts, placed, diags, nullptr, nullptr, 0));
  }

  // One cut in the first round, on the back edge; in every round every cut precedes
  // every placement move.
  std::vector<Round> const rounds{ rounds_of(t) };
  REQUIRE(!rounds.empty());
  bool seen_pin{ false };
  for (uint32_t k = 0; k < rounds.size(); ++k) {
    CAPTURE(k);
    uint32_t cuts{ 0 };
    bool pinned{ false };
    bool cut_after_pin{ false };
    for (uint32_t i = rounds[k].first; i < rounds[k].end; ++i) {
      TraceEvent const &e{ t.events[i] };
      if (e.kind != TraceKind::CandidateScored) { continue; }
      if (e.score.move == TRACE_MOVE_RANK) {
        pinned = true;
        continue;
      }
      if (e.score.move != TRACE_MOVE_CUT) { continue; }
      ++cuts;
      CHECK(e.score.state == INVALID);  // a cut names no state
      if (k == 0) {
        CHECK(e.score.trans == back.v);
        CHECK(e.score.leg == 0);
      }
      cut_after_pin = cut_after_pin || pinned;
    }
    if (k == 0) { CHECK(cuts == 1); }
    CHECK(!cut_after_pin);
    seen_pin = seen_pin || pinned;
  }
  CHECK(seen_pin);

  // Scored moves carry their Tier-2 term shares.
  CHECK(count_kind(t, TraceKind::CandidateTerms) > 0);
}

TEST_CASE("trace: what a run reports as taken re-derives the run") {
  // The reported tuple and taken pins, with Level 1 off, reproduce the searched layout.
  Chart searched;
  Chart rederived;
  for (Chart *c : { &searched, &rederived }) {
    SubmachineId const root{ build_chart(*c, "t", {}) };
    StateId const a{ build_state(*c, root, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(*c, root, "B", StateKind::Normal, {}) };
    StateId const d{ build_state(*c, root, "C", StateKind::Normal, {}) };
    StateId const e{ build_state(*c, root, "D", StateKind::Normal, {}) };
    build_trans(*c, a, b, TransKind::Default, {});
    build_trans(*c, b, d, TransKind::Default, {});
    build_trans(*c, d, e, TransKind::Default, {});
    build_trans(*c, e, a, TransKind::Default, {});
    build_trans(*c, d, a, TransKind::Default, {});
  }

  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  std::vector<scav_placed> pa;
  std::vector<scav_placed> pb;
  std::vector<Diagnostic> da;
  std::vector<Diagnostic> db;
  uint32_t tuple{ INVALID };
  SearchPins taken;
  REQUIRE(
      layout_run(searched, {}, opts, pa, da, nullptr, &tuple, INVALID, nullptr, &taken));

  scav_layout_opts again{ opts };
  again.profile.portfolio_k = 0;  // the pins already hold what Level 1 found
  REQUIRE(layout_run(rederived,
                     {},
                     again,
                     pb,
                     db,
                     nullptr,
                     nullptr,
                     tuple,
                     nullptr,
                     nullptr,
                     &taken));

  CHECK(layout_structural_hash(searched) == layout_structural_hash(rederived));
  CHECK(layout_coordinate_hash(searched) == layout_coordinate_hash(rederived));
}

TEST_CASE("trace: the outline names each row's searches, every kick and what a row took") {
  // `gauntlet/kicked.scav` built in its loader's order: Open's two regions, each a run
  // whose last state leaves for Wait.
  auto const kicked = [](Chart &c) {
    SubmachineId const root{ build_chart(c, "kicked", {}) };
    StateId const closed{ build_state(c, root, "Closed", StateKind::Normal, {}) };
    StateId const open{ build_state(c, root, "Open", StateKind::Normal, {}) };
    SubmachineId const in{ build_submachine(c, open, "inbound", {}) };
    StateId const a{ build_state(c, in, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(c, in, "B", StateKind::Normal, {}) };
    SubmachineId const out{ build_submachine(c, open, "outbound", {}) };
    StateId const d{ build_state(c, out, "C", StateKind::Normal, {}) };
    StateId const e{ build_state(c, out, "D", StateKind::Normal, {}) };
    StateId const wait{ build_state(c, root, "Wait", StateKind::Normal, {}) };
    StateId const in_start{ build_state(c, in, "", StateKind::Initial, {}) };
    StateId const out_start{ build_state(c, out, "", StateKind::Initial, {}) };
    StateId const start{ build_state(c, root, "", StateKind::Initial, {}) };
    build_trans(c, in_start, a, TransKind::Default, {});
    build_trans(c, a, b, TransKind::Default, {});
    build_trans(c, out_start, d, TransKind::Default, {});
    build_trans(c, d, e, TransKind::Default, {});
    build_trans(c, start, closed, TransKind::Default, {});
    build_trans(c, closed, open, TransKind::Default, {});
    build_trans(c, b, wait, TransKind::Default, {});
    build_trans(c, e, wait, TransKind::Default, {});
    build_trans(c, wait, closed, TransKind::Default, {});
  };
  Chart traced;
  Chart plain;
  kicked(traced);
  kicked(plain);
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.profile.search_cull = 0;  // the full search
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  REQUIRE(layout_run(plain, {}, opts, placed, diags));
  opts.threads = 1;
  LayoutTrace t;
  trace_outline_set(&t);
  bool const ran{ layout_run(traced, {}, opts, placed, diags) };
  trace_outline_set(nullptr);
  REQUIRE(ran);
  CHECK(layout_coordinate_hash(traced) == layout_coordinate_hash(plain));

  // Each row's first search in row order, a repeat naming an earlier row, each kick's own
  // frame's cost no lower than its converged one, a verdict against the row's drawing,
  // each take below it, then each row's refold.
  std::vector<Cost> drawn(LAYOUT_SEARCH_ROWS);
  uint32_t firsts{ 0 };
  uint32_t refolds{ 0 };
  uint32_t outward{ 0 };  // kicks dearer in their own frame that improve once it re-ranks
  for (TraceEvent const &ev : t.events) {
    CAPTURE(static_cast<uint32_t>(ev.kind));
    uint32_t const row{ ev.search.row };
    REQUIRE(row < LAYOUT_SEARCH_ROWS);
    Cost const reached{ .t0_violations = ev.search.t0, .t2 = ev.search.t2 };
    if (ev.kind == TraceKind::RowSearched) {
      if (ev.pass == static_cast<uint16_t>(RowPass::First)) {
        CHECK(row == firsts);
        CHECK(refolds == 0);
        ++firsts;
        drawn[row] = reached;
      } else {
        ++refolds;
      }
      continue;
    }
    CHECK(firsts == LAYOUT_SEARCH_ROWS);
    if (ev.kind == TraceKind::RowRepeated) {
      CHECK(ev.search.of < row);
      continue;
    }
    if (ev.kind == TraceKind::KickTaken) {
      CHECK(cost_less(reached, drawn[row]));
      drawn[row] = reached;
      continue;
    }
    REQUIRE(ev.kind == TraceKind::KickScored);
    if (ev.pass == static_cast<uint16_t>(KickVerdict::NotViable)) { continue; }
    Cost const framed{ .t0_violations = ev.search.framed_t0, .t2 = ev.search.framed };
    CHECK_FALSE(cost_less(framed, reached));
    bool const improves{ ev.pass == static_cast<uint16_t>(KickVerdict::Improves) };
    CHECK(improves == cost_less(reached, drawn[row]));
    outward += (improves && !cost_less(framed, drawn[row])) ? 1U : 0U;
  }
  CHECK(firsts == LAYOUT_SEARCH_ROWS);
  CHECK(refolds == LAYOUT_SEARCH_ROWS);
  CHECK(outward > 0);
}
