// The sink in isolation -- stamping, scoping, serialization -- then one whole
// traced run, held to the decisions that explain a route a reader can see.

#include "layout/trace.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

// Sets the sink for a scope and always clears it: a leaked sink would have
// every later test in the binary appending to a dead vector.
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

// One round of Level 1 as the trace shows it. A round runs every move's phase 1
// before it emits any verdict, so it is a maximal run of scores, and
// `orderings` counts the reversals since the round before -- one per ordering
// on a chart whose every ordering breaks one cycle.
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
  CHECK(out.find("\"state\":\"Alpha\",\"rank\":4") != std::string::npos);
  CHECK(out.find("\"state\":\"#" + std::to_string(anon.v) + "\"") != std::string::npos);
  CHECK(out.find("\"state\":null") != std::string::npos);
  // Two of them: the out-of-range ordinal and INVALID both have no name to print.
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
  t.events.push_back({ .kind = TraceKind::CandidateScored,
                       .pass = static_cast<uint16_t>(MoveVerdict::NotBetter),
                       .score = { .row = INVALID,
                                  .state = INVALID,
                                  .rank = 0,
                                  .t0 = 0,
                                  .t2 = 4294967296 } });

  std::string const out{ json_of(t, c) };
  CHECK(out.find("\"seg\":3,\"rank\":2,\"index\":1,\"count\":1") != std::string::npos);
  CHECK(out.find("\"bend_of_seg\":3,\"at\":[2823,1489]") != std::string::npos);
  CHECK(out.find("\"node_sep\":320,\"rank_sep\":640") != std::string::npos);
  CHECK(out.find("\"waypoints\":1,\"src\":[1997,2469],\"dst\":[1344,509]") !=
        std::string::npos);
  CHECK(out.find("\"at\":[-8,1489]") != std::string::npos);  // a negative coordinate
  CHECK(out.find("\"pass\":\"separate\",\"net\":2,\"end\":\"dst\"") != std::string::npos);
  CHECK(out.find("\"net\":5,\"lane\":1,\"at\":96") != std::string::npos);
  // Past 32 bits, and a row nothing named is omitted rather than printed as -1.
  CHECK(out.find("\"t2\":4294967296") != std::string::npos);
  CHECK(out.find("\"verdict\":\"not_better\",\"row\":4294967295") != std::string::npos);
  // One object per event and a comma between each pair, never a trailing one.
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
    build_trans(*c, a, b, TransKind::External, {});
    build_trans(*c, b, d, TransKind::External, {});
    build_trans(*c, d, a, TransKind::External, {});  // the back edge that chains
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
  CHECK(trace_sink() == nullptr);  // cleared even though the run succeeded
}

TEST_CASE("trace: a back edge's kinks are one chained bend, and the trace says so") {
  // 11.16's measurement, as a chart: A -> B -> C -> A ranks the three in a row
  // and the back edge spans two, so it chains through a bend in B's rank and
  // the router is handed that bend as a waypoint. Estop's shape exactly.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  TransId const back{ build_trans(c, d, a, TransKind::External, {}) };

  LayoutTrace t;
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  opts.threads = 1;
  // One ordering, so the counts below are the algorithm's and not the search's:
  // `row` pins the Level 2 tuple, and Level 1 re-orders once per move it scores.
  opts.profile.portfolio_k = 0;
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  {
    Attached const held{ t };
    REQUIRE(layout_run(c, {}, opts, placed, diags, nullptr, nullptr, 0));
  }
  CHECK(count_kind(t, TraceKind::CandidateScored) == 0);

  // Ranked in a row, so the back edge is the only multi-rank one.
  uint32_t ranks[3]{ INVALID, INVALID, INVALID };
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::RankAssigned) { continue; }
    for (uint32_t i = 0; i < 3; ++i) {
      if (e.rank.state == (StateId{ a.v + i }).v) { ranks[i] = e.rank.rank; }
    }
  }
  CHECK(ranks[0] < ranks[1]);
  CHECK(ranks[1] < ranks[2]);

  // Exactly one bend, at B's rank, and it is the back edge that grew it.
  CHECK(count_kind(t, TraceKind::EdgeChained) == 1);
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::EdgeChained) { continue; }
    CHECK(e.chain.rank == ranks[1]);
    CHECK(e.chain.count == 1);
  }
  CHECK(count_kind(t, TraceKind::EdgeReversed) == 1);

  // One net carries a waypoint and it is the back edge's; every other net is
  // handed none. This is the property the kinks come from.
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

  // The waypoint is the bend's placed coordinate and not a second guess at it.
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

TEST_CASE("trace: the search re-orders per move, and every move states its verdict") {
  // The same chart under Level 1, which is the composition the case above
  // isolates one ordering out of: each scored move is a whole fresh phase 1.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  TransId const back{ build_trans(c, d, a, TransKind::External, {}) };

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

  // Every verdict is one of the four, and a rejected move is as visible as a
  // taken one -- which is the whole reason the event carries a verdict.
  uint32_t const scored{ count_kind(t, TraceKind::CandidateScored) };
  uint32_t taken{ 0 };
  for (TraceEvent const &e : t.events) {
    if (e.kind != TraceKind::CandidateScored) { continue; }
    CHECK(e.pass <= static_cast<uint16_t>(MoveVerdict::NotBetter));
    // A placement names a state and nothing else; every other move names a
    // transition and no state.
    CHECK(e.score.move <= TRACE_MOVE_FACE);
    CHECK((e.score.state == INVALID) == (e.score.move != TRACE_MOVE_RANK));
    CHECK((e.score.trans == INVALID) == (e.score.move == TRACE_MOVE_RANK));
    taken += (e.pass == static_cast<uint16_t>(MoveVerdict::Taken)) ? 1U : 0U;
  }
  CHECK(scored > 0);
  CHECK(taken < scored);  // a greedy pass rejects more than it takes

  // One ordering per move scored, and one more before each round: the take
  // that rebuilt the orders it reads, or the start of the search it opens.
  // Level 1 is several searches -- the row's, then a kick's from each edge of
  // the cycle (11.10f) -- so this holds per round rather than per run. The
  // first round follows `layout_run`'s own ordering as well.
  std::vector<Round> const rounds{ rounds_of(t) };
  REQUIRE(!rounds.empty());
  for (uint32_t k = 0; k < rounds.size(); ++k) {
    CAPTURE(k);
    CHECK(rounds[k].orderings >= (rounds[k].scored + ((k == 0) ? 2U : 1U)));
  }

  // A three-cycle leaves at most one edge spanning two ranks, so no ordering
  // hands more than one net a waypoint -- and *which* net is the search's
  // choice, since a reversal turns a different edge around (11.10d), while a
  // cut removes the bend and leaves none (11.10b). At most one, and some.
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
  // A cycle `A -> B -> C -> A` with a tail `C -> D`. Turning the tail round
  // makes a cycle for the walk to break somewhere else, which is how `dock`'s
  // initial arrow came to run backwards (11.10g); only the three cycle edges
  // choose anything.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  StateId const tail{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  build_trans(c, d, a, TransKind::External, {});
  TransId const off{ build_trans(c, d, tail, TransKind::External, {}) };

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

TEST_CASE("trace: the search scores unchain moves beside placement moves") {
  // 11.10b's dimension, seen through the trace: the back edge of a cycle is
  // the one segment phase 1 chains, so it is the one a cut can free, and the
  // sweep must offer it before it starts moving states between ranks.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, b, d, TransKind::External, {});
  TransId const back{ build_trans(c, d, a, TransKind::External, {}) };

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

  // Exactly one cut in the first round -- the back edge, the one segment
  // phase 1 chained -- and in every round every cut comes before every
  // placement move. Only the first round is pinned to the back edge: later
  // ones include kicks (11.10f), which turn another edge of the cycle around
  // and so chain that one instead. The other transition-naming moves are
  // reversals and faces (11.10d, 11.10e), told apart by the event's `move`
  // rather than by which fields are set.
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

  // Every scored move states which term rejected it.
  CHECK(count_kind(t, TraceKind::CandidateTerms) > 0);
}

TEST_CASE("trace: what a run reports as taken re-derives the run") {
  // The whole re-derivation contract, over a chart whose search has both
  // dimensions to choose from: tuple plus pins, and nothing left to search.
  Chart searched;
  Chart rederived;
  for (Chart *c : { &searched, &rederived }) {
    SubmachineId const root{ build_chart(*c, "t", {}) };
    StateId const a{ build_state(*c, root, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(*c, root, "B", StateKind::Normal, {}) };
    StateId const d{ build_state(*c, root, "C", StateKind::Normal, {}) };
    StateId const e{ build_state(*c, root, "D", StateKind::Normal, {}) };
    build_trans(*c, a, b, TransKind::External, {});
    build_trans(*c, b, d, TransKind::External, {});
    build_trans(*c, d, e, TransKind::External, {});
    build_trans(*c, e, a, TransKind::External, {});
    build_trans(*c, d, a, TransKind::External, {});
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
