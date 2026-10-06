// The culled search: its row table, the rows it kicks and refolds, its don't-look bits
// against the whole last round, its independence of threads, the label bound and the
// trace, and the jitter seed.

#include "layout/route.h"
#include "layout/size.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "core/tests/corpus.h"
#include "doctest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace scav {

void search_table(scav_profile const &p, std::vector<uint32_t> &rows);
Row search_row(scav_profile const &p, uint32_t index);
void kick_order(std::vector<uint32_t> &rest,
                std::vector<Cost> const &cost,
                std::vector<int64_t> const &jit);
void search_changes(Chart const &c,
                    SizedLayout const &was,
                    Routes const &was_routes,
                    SizedLayout const &now,
                    Routes const &now_routes,
                    std::vector<uint8_t> &frame,
                    std::vector<uint8_t> &route,
                    std::vector<uint8_t> &resized);
void layout_test_dont_look_verify(bool on);
uint64_t layout_test_dont_look_checked();
uint64_t layout_test_dont_look_mismatches();
void layout_test_label_bound(bool on, bool verify);
void layout_test_search_memo(bool on);
void layout_test_candidate_memo_empty(uint32_t every);
void layout_test_bound_replay(bool on);
uint64_t layout_test_bound_replayed();
uint64_t layout_test_bound_replay_mismatches();

}  // namespace scav

namespace {

using namespace scav;

scav_profile readable() {  // the full search
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  p.search_cull = 0;
  return p;
}

scav_profile culled() {
  scav_profile p{ readable() };
  p.search_cull = 1;
  return p;
}

// Restores the shipped test switches on scope exit.
struct SwitchGuard {
  SwitchGuard() = default;
  SwitchGuard(SwitchGuard const &) = delete;
  SwitchGuard &operator=(SwitchGuard const &) = delete;
  ~SwitchGuard() {
    layout_test_dont_look_verify(false);
    layout_test_label_bound(true, false);
    layout_test_search_memo(true);
    layout_test_candidate_memo_empty(0);
    layout_test_bound_replay(false);
  }
};

// One run's drawing, its row and pins, and the search's counts summed over move kinds.
struct Laid {
  uint32_t structural{ 0 };
  uint32_t coordinate{ 0 };
  uint32_t tuple{ INVALID };
  std::vector<uint32_t> pins;
  uint64_t offered{ 0 };
  uint64_t taken{ 0 };
  uint64_t skipped{ 0 };
};

// Every pin as words, in `SearchPins` order.
std::vector<uint32_t> words_of(SearchPins const &p) {
  std::vector<uint32_t> w;
  for (RankPin const &r : p.ranks) { w.insert(w.end(), { 0, r.state.v, r.rank }); }
  for (ChainCut const &k : p.cuts) { w.insert(w.end(), { 1, k.trans.v, k.leg }); }
  for (ReversePin const &r : p.reverses) { w.insert(w.end(), { 2, r.trans.v, r.leg }); }
  for (EndPin const &e : p.ends) {
    w.insert(w.end(), { 3, e.trans.v, e.leg, e.end, e.face });
  }
  for (OrientPin const &o : p.orients) { w.insert(w.end(), { 4, o.frame.v }); }
  for (FoldPin const &f : p.folds) {
    w.insert(w.end(), { 5, f.frame.v, f.mode, f.layer });
  }
  for (LoopPin const &l : p.loops) { w.insert(w.end(), { 6, l.state.v, l.face, l.end }); }
  return w;
}

Chart loaded(char const *name) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  Chart c;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
  return c;
}

// A path box on every routed transition, so a layout places labels.
std::vector<scav_path_box> label_boxes(Chart const &c) {
  std::vector<scav_path_box> boxes;
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    Transition const &tr{ c.transitions[t] };
    if ((tr.live == 0) || ((tr.src == tr.dst) && (tr.kind != TransKind::Default))) {
      continue;
    }
    boxes.push_back({ .subject = t, .w = 40, .h = 12, .order = 0 });
  }
  return boxes;
}

Laid lay(char const *name, scav_profile const &p, bool labelled, uint32_t threads = 0) {
  Chart c{ loaded(name) };
  std::vector<scav_path_box> boxes;
  if (labelled) { boxes = label_boxes(c); }
  scav_spaces const s{ .box_state_stride = sizeof(scav_box_space),
                       .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()),
                       .path_box_stride = sizeof(scav_path_box) };
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = threads };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  Laid out;
  SearchPins taken;
  SearchStats counted;
  bool const ran{ layout_run(c,
                             s,
                             opts,
                             placed,
                             diags,
                             nullptr,
                             &out.tuple,
                             INVALID,
                             nullptr,
                             &taken,
                             nullptr,
                             &counted) };
  REQUIRE(ran);
  out.structural = layout_structural_hash(c);
  out.coordinate = layout_coordinate_hash(c);
  out.pins = words_of(taken);
  for (uint32_t k = 0; k < TRACE_MOVES; ++k) {
    out.offered += counted.offered[k];
    out.taken += counted.taken[k];
    out.skipped += counted.skipped[k];
  }
  return out;
}

void check_same_drawing(Laid const &got, Laid const &want) {
  CHECK(got.structural == want.structural);
  CHECK(got.coordinate == want.coordinate);
  CHECK(got.tuple == want.tuple);
  CHECK(got.pins == want.pins);
}

// The row and kick events of a one-thread run.
std::vector<TraceEvent> outline(char const *name, scav_profile const &p) {
  Chart c{ loaded(name) };
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 1 };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  LayoutTrace t;
  trace_outline_set(&t);
  bool const ran{ layout_run(c, {}, opts, placed, diags) };
  trace_outline_set(nullptr);
  REQUIRE(ran);
  return t.events;
}

}  // namespace

TEST_CASE("search: the culled table is the first portfolio_m rows less compaction") {
  scav_profile p{ readable() };
  std::vector<uint32_t> rows;
  search_table(p, rows);
  CHECK(rows ==
        std::vector<uint32_t>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 });
  p.portfolio_m = 4;
  search_table(p, rows);
  CHECK(rows == std::vector<uint32_t>{ 0, 1, 2, 3 });

  p.search_cull = 1;
  search_table(p, rows);
  CHECK(rows == std::vector<uint32_t>{ 0, 1 });
  p.portfolio_m = 1;
  search_table(p, rows);
  CHECK(rows == std::vector<uint32_t>{ 0 });
  p.portfolio_m = 16;
  search_table(p, rows);
  CHECK(rows == std::vector<uint32_t>{ 0, 1, 4, 5, 8, 9, 12, 13 });
  for (uint32_t const r : rows) {
    CAPTURE(r);
    CHECK((search_row(p, r).pack == Compaction::Off));
  }
}

TEST_CASE(
    "search: the culled search kicks the kick_rows rows cheapest after their first") {
  // Per row: its first search's cost, whether it repeats an earlier row, and whether a
  // kick was scored for it.
  for (int32_t const k : { 1, 2, 16 }) {
    CAPTURE(k);
    scav_profile p{ culled() };
    p.kick_rows = k;
    std::map<uint32_t, Cost> first;
    std::set<uint32_t> repeated;
    std::set<uint32_t> kicked;
    for (TraceEvent const &ev : outline("brew.scav", p)) {
      uint32_t const row{ ev.search.row };
      if ((ev.kind == TraceKind::RowSearched) &&
          (ev.pass == static_cast<uint16_t>(RowPass::First))) {
        first[row] = { .t0_violations = ev.search.t0, .t2 = ev.search.t2 };
      }
      if (ev.kind == TraceKind::RowRepeated) { repeated.insert(row); }
      if (ev.kind == TraceKind::KickScored) { kicked.insert(row); }
    }
    // Every compaction-free row searched once, and none of the others.
    CHECK(first.size() == 8);
    for (auto const &[row, cost] : first) { CHECK((row & 2U) == 0); }
    std::vector<uint32_t> want;
    for (auto const &[row, cost] : first) {
      if (!repeated.contains(row)) { want.push_back(row); }
    }
    REQUIRE(want.size() > 2);  // the chart has rows to choose between
    std::ranges::stable_sort(want, [&](uint32_t a, uint32_t b) {
      return cost_less(first[a], first[b]);
    });
    if (want.size() > static_cast<size_t>(k)) { want.resize(static_cast<size_t>(k)); }
    CHECK(kicked == std::set<uint32_t>(want.begin(), want.end()));
  }
}

TEST_CASE("search: the culled search refolds no repeated row, the full search every row") {
  for (bool const cull : { false, true }) {
    CAPTURE(cull);
    std::set<uint32_t> repeated;
    std::set<uint32_t> refolded;
    for (TraceEvent const &ev : outline("estop.scav", cull ? culled() : readable())) {
      if (ev.kind == TraceKind::RowRepeated) { repeated.insert(ev.search.row); }
      if ((ev.kind == TraceKind::RowSearched) &&
          (ev.pass == static_cast<uint16_t>(RowPass::Refold))) {
        refolded.insert(ev.search.row);
      }
    }
    REQUIRE(!repeated.empty());
    for (uint32_t const row : repeated) {
      CAPTURE(row);
      CHECK(refolded.contains(row) == !cull);
    }
    CHECK(refolded.size() + (cull ? repeated.size() : 0) == (cull ? 8U : 16U));
  }
}

TEST_CASE("search: search_changes flags the frames, routes and extents that moved") {
  // Root holds A and B; B's submachine holds C and D; A -> B and C -> D.
  Chart c;
  SubmachineId const root{ build_chart(c, "changes", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, b, "inner", {}) };
  StateId const cc{ build_state(c, inner, "C", StateKind::Normal, {}) };
  StateId const d{ build_state(c, inner, "D", StateKind::Normal, {}) };
  TransId const ab{ build_trans(c, a, b, TransKind::Default, {}) };
  TransId const cd{ build_trans(c, cc, d, TransKind::Default, {}) };

  SizedLayout was;
  was.state = { { .x = 0, .y = 0, .w = 10, .h = 10 },
                { .x = 20, .y = 0, .w = 50, .h = 30 },
                { .x = 25, .y = 5, .w = 10, .h = 10 },
                { .x = 45, .y = 5, .w = 10, .h = 10 } };
  was.loop.assign(4, scav_rect{});
  Routes was_routes;
  was_routes.points = { { .x = 10, .y = 5 },
                        { .x = 20, .y = 5 },
                        { .x = 35, .y = 10 },
                        { .x = 45, .y = 10 } };
  was_routes.route = { { .off = 0, .len = 2 }, { .off = 2, .len = 2 } };

  std::vector<uint8_t> frame;
  std::vector<uint8_t> route;
  std::vector<uint8_t> resized;
  auto const changes = [&](SizedLayout const &now, Routes const &now_routes) {
    search_changes(c, was, was_routes, now, now_routes, frame, route, resized);
  };

  changes(was, was_routes);
  CHECK(frame == std::vector<uint8_t>{ 0, 0 });
  CHECK(route == std::vector<uint8_t>{ 0, 0 });
  CHECK(resized == std::vector<uint8_t>{ 0, 0, 0, 0 });

  // D moves inside B: B's frame and nothing else.
  SizedLayout moved{ was };
  moved.state[d.v].x += 5;
  changes(moved, was_routes);
  CHECK(frame[inner.v] == 1);
  CHECK(frame[root.v] == 0);
  CHECK(resized[d.v] == 0);

  // C grows: its frame and its extent.
  SizedLayout grown{ was };
  grown.state[cc.v].w += 4;
  changes(grown, was_routes);
  CHECK(frame[inner.v] == 1);
  CHECK(resized[cc.v] == 1);
  CHECK(resized[d.v] == 0);

  // A loop room in the root frame appears: the root frame alone.
  SizedLayout looped{ was };
  looped.loop[a.v] = { .x = 2, .y = 2, .w = 4, .h = 4 };
  changes(looped, was_routes);
  CHECK(frame[root.v] == 1);
  CHECK(frame[inner.v] == 0);

  // C -> D bends: that route alone; one point fewer is a change too.
  Routes bent{ was_routes };
  bent.points[3].y += 3;
  changes(was, bent);
  CHECK(route[cd.v] == 1);
  CHECK(route[ab.v] == 0);
  Routes shorter{ was_routes };
  shorter.route[ab.v].len = 1;
  changes(was, shorter);
  CHECK(route[ab.v] == 1);
  CHECK(route[cd.v] == 0);
}

namespace {

// Lays each chart out at both scales with each search's stop checked against its whole
// last round; returns the moves the bits skipped.
uint64_t stops_checked(scav_profile const &p, std::initializer_list<char const *> charts) {
  SwitchGuard const guard;
  uint64_t skipped{ 0 };
  for (bool const labelled : { false, true }) {
    for (char const *name : charts) {
      if (scav::test::corpus_skipped(name)) { continue; }
      CAPTURE(labelled);
      std::string const chart{ name };
      CAPTURE(chart);
      layout_test_dont_look_verify(true);
      skipped += lay(name, p, labelled).skipped;
      CHECK(layout_test_dont_look_checked() > 0);
      CHECK(layout_test_dont_look_mismatches() == 0);
    }
  }
  return skipped;
}

}  // namespace

TEST_CASE("search: every culled search stops where no move of its last round improves") {
  // The bits skip moves, and a round that takes nothing scores what they skipped.
  CHECK(stops_checked(culled(), { "brew.scav", "dock.scav", "kiln.scav" }) > 0);
  // The check holds the full search to the same, with nothing skipped.
  CHECK(stops_checked(readable(), { "brew.scav", "dock.scav" }) == 0);
}

TEST_CASE("search: every culled search on the corpus stops at a local optimum" *
          doctest::test_suite("full")) {
  CHECK(stops_checked(
            culled(),
            { "axis.scav", "ota.scav", "tcp.scav", "vac.scav", "elevator.scav" }) > 0);
}

TEST_CASE("search: the culled search is one search at every thread count") {
  // With the search memo off every search runs; drawing, pins and counts all agree.
  SwitchGuard const guard;
  layout_test_search_memo(false);
  for (bool const labelled : { false, true }) {
    for (char const *name : { "brew.scav", "dock.scav", "kiln.scav" }) {
      CAPTURE(labelled);
      std::string const chart{ name };
      CAPTURE(chart);
      Laid const want{ lay(name, culled(), labelled, 1) };
      CHECK(want.skipped > 0);
      for (uint32_t const threads : { 3U, 0U }) {
        CAPTURE(threads);
        Laid const got{ lay(name, culled(), labelled, threads) };
        check_same_drawing(got, want);
        CHECK(got.offered == want.offered);
        CHECK(got.taken == want.taken);
        CHECK(got.skipped == want.skipped);
      }
    }
  }
}

TEST_CASE("search: the culled search reaches one drawing with the label bound on or off") {
  // Off, every candidate is labelled and the bits read a bound laid out beside it.
  SwitchGuard const guard;
  layout_test_search_memo(false);
  for (char const *name : { "brew.scav", "dock.scav", "kiln.scav" }) {
    std::string const chart{ name };
    CAPTURE(chart);
    Laid const bounded{ lay(name, culled(), true) };
    layout_test_label_bound(false, false);
    Laid const whole{ lay(name, culled(), true) };
    layout_test_label_bound(true, false);
    check_same_drawing(whole, bounded);
    CHECK(whole.offered == bounded.offered);
    CHECK(whole.skipped == bounded.skipped);
  }
}

TEST_CASE(
    "search: a candidate memo emptied before labelling leaves the culled search as it "
    "is") {
  // Labelling then lays out afresh moves the bound pass answered from the memo.
  SwitchGuard const guard;
  layout_test_search_memo(false);
  for (char const *name : { "brew.scav", "dock.scav", "kiln.scav" }) {
    std::string const chart{ name };
    CAPTURE(chart);
    Laid const want{ lay(name, culled(), true, 1) };
    for (uint32_t const every : { 1U, 3U }) {
      CAPTURE(every);
      layout_test_candidate_memo_empty(every);
      Laid const got{ lay(name, culled(), true, 1) };
      check_same_drawing(got, want);
      CHECK(got.offered == want.offered);
      CHECK(got.taken == want.taken);
      CHECK(got.skipped == want.skipped);
    }
    layout_test_candidate_memo_empty(0);
  }
}

TEST_CASE("search: a bound pass scored again after its labelling sets the same bits") {
  SwitchGuard const guard;
  layout_test_search_memo(false);
  for (uint32_t const every : { 0U, 3U }) {
    for (char const *name : { "brew.scav", "dock.scav", "kiln.scav" }) {
      CAPTURE(every);
      std::string const chart{ name };
      CAPTURE(chart);
      layout_test_candidate_memo_empty(every);
      layout_test_bound_replay(true);
      (void)lay(name, culled(), true, 1);
      CHECK(layout_test_bound_replayed() > 0);
      CHECK(layout_test_bound_replay_mismatches() == 0);
    }
  }
}

TEST_CASE("search: a traced culled search draws what the untraced one ships") {
  Chart traced{ loaded("dock.scav") };
  std::vector<scav_path_box> const boxes{ label_boxes(traced) };
  scav_spaces const s{ .box_state_stride = sizeof(scav_box_space),
                       .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()),
                       .path_box_stride = sizeof(scav_path_box) };
  scav_layout_opts const opts{ .profile = culled(), .router = 0, .threads = 0 };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  TraceWrite const discard = [](void * /*ctx*/, char const * /*text*/, size_t /*n*/) {
    return true;
  };
  bool streamed{ false };
  REQUIRE(layout_trace(traced,
                       s,
                       opts,
                       placed,
                       diags,
                       { .path = nullptr, .write = discard, .ctx = nullptr },
                       streamed,
                       INVALID,
                       TraceScope::Search));
  CHECK(streamed);
  Laid const shipped{ lay("dock.scav", culled(), true) };
  CHECK(layout_coordinate_hash(traced) == shipped.coordinate);
  CHECK(layout_structural_hash(traced) == shipped.structural);
}

TEST_CASE("search: kicks stacked after a round's pick rank by cost plus jitter") {
  std::vector<Cost> const cost{ { .t0_violations = 0, .t1_hints = 0, .t2 = 100 },
                                { .t0_violations = 0, .t1_hints = 0, .t2 = 101 },
                                { .t0_violations = 0, .t1_hints = 0, .t2 = 102 },
                                { .t0_violations = 0, .t1_hints = 0, .t2 = 101 } };
  std::vector<uint32_t> rest{ 3, 0, 1, 2 };
  kick_order(rest, cost, { 0, 0, 0, 0 });
  CHECK(rest == std::vector<uint32_t>{ 0, 3, 1, 2 });  // ties keep their order
  rest = { 0, 1, 2, 3 };
  kick_order(rest, cost, { 5, 0, 0, 3 });
  CHECK(rest == std::vector<uint32_t>{ 1, 2, 3, 0 });
}

TEST_CASE("search: a jitter seed draws one drawing at every thread count") {
  // A seed breaks near-ties the same way on any thread; seed 7 moves kiln.
  for (bool const cull : { false, true }) {
    CAPTURE(cull);
    scav_profile p{ cull ? culled() : readable() };
    REQUIRE(p.jitter_seed == 0);
    Laid const plain{ lay("kiln.scav", p, false) };
    p.jitter_seed = 7;
    Laid const one{ lay("kiln.scav", p, false, 1) };
    CHECK(one.pins != plain.pins);
    check_same_drawing(lay("kiln.scav", p, false, 0), one);
  }
}
