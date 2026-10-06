// Level 1 search shortcuts checked against runs without them: the search memo, the
// candidate memo, unscored no-op faces, culled moves, and face moves scored from the
// incumbent's prefix.

#include "layout/candidate_memo.h"
#include "layout/pack.h"
#include "layout/size.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "core/tests/corpus.h"
#include "doctest.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace scav {

void search_key(scav_profile const &objective,
                Row const &row,
                uint32_t budget,
                bool refold,
                SearchPins const &seed,
                std::vector<uint8_t> const *scope,
                std::vector<uint32_t> &key);
void layout_test_prefix_shortcut(bool on);
void layout_test_prefix_verify(bool on);
uint64_t layout_test_prefix_used();
uint64_t layout_test_prefix_mismatches();
void layout_test_skip_noop_faces(bool on);
uint64_t layout_test_noop_faces();
void layout_test_cull(bool on, bool verify);
std::array<uint64_t, TRACE_MOVES> layout_test_culled();
uint64_t layout_test_cull_mismatches();
void layout_test_label_bound(bool on, bool verify);
uint64_t layout_test_label_bound_skipped();
uint64_t layout_test_label_bound_labelled();
uint64_t layout_test_label_bound_mismatches();
void layout_test_search_memo(bool on);
void layout_test_search_memo_verify(bool on);
uint32_t layout_test_search_memo_hits();
uint32_t layout_test_search_memo_mismatches();
void layout_test_candidate_memo(bool on, bool verify);
void layout_test_candidate_memo_budget(uint64_t bytes);
uint64_t layout_test_candidate_memo_deduped();
uint64_t layout_test_candidate_memo_drawn();
uint64_t layout_test_candidate_memo_faced();
uint64_t layout_test_candidate_memo_mismatches();
void layout_test_route_bound(bool on, bool verify);
void layout_test_cost_stop(bool on);
uint64_t layout_test_cost_stopped();
uint64_t layout_test_route_bound_pruned();
uint64_t layout_test_route_bound_checked();
uint64_t layout_test_route_bound_mismatches();
void layout_test_row_alias(bool on);
std::vector<Cost> const &layout_test_schedule_first();
std::vector<Cost> const &layout_test_schedule_second();
std::vector<Cost> const &layout_test_schedule_kept();

}  // namespace scav

namespace {

using namespace scav;

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

struct Inputs {
  scav_profile objective{};
  Row row;
  uint32_t budget{ 64 };
  bool refold{ false };
  SearchPins seed;
  std::vector<uint8_t> scope;
  bool scoped{ false };
};

std::vector<uint32_t> key_of(Inputs const &in) {
  std::vector<uint32_t> key;
  search_key(in.objective,
             in.row,
             in.budget,
             in.refold,
             in.seed,
             in.scoped ? &in.scope : nullptr,
             key);
  return key;
}

// Re-enables the memo and disables its verifier on scope exit.
struct MemoGuard {
  MemoGuard() = default;
  MemoGuard(MemoGuard const &) = delete;
  MemoGuard &operator=(MemoGuard const &) = delete;
  ~MemoGuard() {
    layout_test_search_memo(true);
    layout_test_search_memo_verify(false);
  }
};

struct Laid {
  uint32_t structural{ 0 };
  uint32_t coordinate{ 0 };
  bool ok{ false };
};

// With `labelled`, a path box for every routed transition, so a layout places labels;
// with `culled`, the culled search.
Laid lay_out(char const *name, bool labelled = false, bool culled = false) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  Chart c;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
  std::vector<scav_path_box> boxes;
  for (uint32_t t = 0; labelled && (t < c.transitions.size()); ++t) {
    Transition const &tr{ c.transitions[t] };
    if ((tr.live == 0) || ((tr.src == tr.dst) && (tr.kind != TransKind::Default))) {
      continue;
    }
    boxes.push_back({ .subject = t, .w = 40, .h = 12, .order = 0 });
  }
  scav_spaces const s{ .box_state_stride = sizeof(scav_box_space),
                       .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()),
                       .path_box_stride = sizeof(scav_path_box) };
  std::vector<scav_placed> placed;
  scav_profile p{ readable() };
  p.search_cull = culled ? 1 : 0;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  Laid out;
  out.ok = layout_run(c, s, opts, placed, diags);
  out.structural = layout_structural_hash(c);
  out.coordinate = layout_coordinate_hash(c);
  return out;
}

}  // namespace

TEST_CASE("search memo: the key tells apart every input a search is a function of") {
  Inputs base;
  base.objective = readable();
  base.row.knobs = readable();
  base.seed.ranks.push_back({ .state = StateId{ 3 }, .rank = 1 });
  base.seed.ranks.push_back({ .state = StateId{ 5 }, .rank = 2 });
  base.seed.ends.push_back({ .trans = TransId{ 2 }, .leg = 0, .end = 1, .face = 3 });
  base.seed.ends.push_back({ .trans = TransId{ 2 }, .leg = 0, .end = 0, .face = 1 });
  base.scope.assign(4, 0);

  std::vector<Inputs> variants(22, base);
  variants[0].objective.node_sep += 1;
  variants[1].row.knobs.node_sep += 1;
  variants[2].row.dar = DarSource::OwnerHole;
  variants[3].row.pack = Compaction::On;
  variants[4].row.fold = Fold::Always;
  variants[5].budget += 1;
  variants[6].seed.ranks[1].rank = 3;
  variants[7].seed.cuts.push_back({ .trans = TransId{ 1 }, .leg = 0 });
  variants[8].seed.reverses.push_back({ .trans = TransId{ 1 }, .leg = 0 });
  variants[9].seed.ends[0].face = 2;
  variants[10].seed.orients.push_back({ .frame = SubmachineId{ 0 } });
  // The same two rank pins in the other order key differently.
  std::swap(variants[11].seed.ranks[0], variants[11].seed.ranks[1]);
  variants[12].scoped = true;
  variants[13].scoped = true;
  variants[13].scope[2] = 1;
  variants[14].scoped = true;
  variants[14].scope[3] = 1;
  variants[15].seed.ranks.pop_back();
  variants[16].seed.ends.push_back(
      { .trans = TransId{ 1 }, .leg = 0, .end = 1, .face = 0 });
  variants[17].seed.ends[1].face = 2;
  variants[18].seed.folds.push_back({ .frame = SubmachineId{ 2 }, .mode = FOLD_NEVER });
  variants[19].seed.folds.push_back({ .frame = SubmachineId{ 2 }, .mode = FOLD_ALWAYS });
  variants[20].seed.folds.push_back(
      { .frame = SubmachineId{ 2 }, .mode = FOLD_ALWAYS, .layer = 2 });
  variants[21].refold = true;

  std::vector<std::vector<uint32_t>> keys{ key_of(base) };
  for (Inputs const &v : variants) { keys.push_back(key_of(v)); }
  CHECK(key_of(base) == keys[0]);
  for (uint32_t a = 0; a < keys.size(); ++a) {
    for (uint32_t b = a + 1; b < keys.size(); ++b) {
      CAPTURE(a);
      CAPTURE(b);
      CHECK(keys[a] != keys[b]);
    }
  }
}

TEST_CASE("search memo: a layout searched through it is the one searched without it" *
          doctest::test_suite("full")) {
  MemoGuard const guard;
  constexpr std::array<char const *, 5> CHARTS{ "axis.scav",
                                                "brew.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint32_t hits{ 0 };
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    layout_test_search_memo(true);
    Laid const with{ lay_out(name) };
    hits += layout_test_search_memo_hits();
    layout_test_search_memo(false);
    Laid const without{ lay_out(name) };
    CHECK(layout_test_search_memo_hits() == 0);
    REQUIRE(with.ok);
    REQUIRE(without.ok);
    CHECK(with.structural == without.structural);
    CHECK(with.coordinate == without.coordinate);
  }
  CHECK(hits > 0);
}

TEST_CASE("search memo: every search it answers is the search run afresh" *
          doctest::test_suite("full")) {
  // The verifier checks each memo answer, taken or not, against the search run afresh.
  MemoGuard const guard;
  layout_test_search_memo_verify(true);
  constexpr std::array<char const *, 5> CHARTS{ "axis.scav",
                                                "brew.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint32_t hits{ 0 };
  for (bool const labelled : { false, true }) {
    for (char const *name : CHARTS) {
      if (scav::test::corpus_skipped(name)) { continue; }
      if (scav::test::corpus_skipped(name)) { continue; }
      CAPTURE(labelled);
      CAPTURE(name);
      REQUIRE(lay_out(name, labelled).ok);
      hits += layout_test_search_memo_hits();
      CHECK(layout_test_search_memo_mismatches() == 0);
    }
  }
  CHECK(hits > 0);
}

TEST_CASE(
    "search: faces with no effect, left unscored, find the layout scoring them finds" *
    doctest::test_suite("full")) {
  struct Restore {
    Restore() = default;
    Restore(Restore const &) = delete;
    Restore &operator=(Restore const &) = delete;
    ~Restore() { layout_test_skip_noop_faces(true); }
  } const restore;
  constexpr std::array<char const *, 5> CHARTS{ "axis.scav",
                                                "brew.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint64_t skipped{ 0 };
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    layout_test_skip_noop_faces(true);
    Laid const with{ lay_out(name) };
    skipped += layout_test_noop_faces();
    layout_test_skip_noop_faces(false);
    Laid const without{ lay_out(name) };
    CHECK(layout_test_noop_faces() == 0);
    REQUIRE(with.ok);
    REQUIRE(without.ok);
    CHECK(with.structural == without.structural);
    CHECK(with.coordinate == without.coordinate);
  }
  CHECK(skipped > 0);
}

namespace {

// Turns culling back on, unverified, on scope exit.
struct CullGuard {
  CullGuard() = default;
  CullGuard(CullGuard const &) = delete;
  CullGuard &operator=(CullGuard const &) = delete;
  ~CullGuard() { layout_test_cull(true, false); }
};

// Lays out each chart at both scales with every culled move checked; returns the culls
// per move kind.
std::array<uint64_t, TRACE_MOVES> culls_checked(
    std::initializer_list<char const *> charts) {
  std::array<uint64_t, TRACE_MOVES> culled{};
  for (bool const labelled : { false, true }) {
    for (char const *name : charts) {
      if (scav::test::corpus_skipped(name)) { continue; }
      CAPTURE(labelled);
      CAPTURE(name);
      layout_test_cull(true, true);
      REQUIRE(lay_out(name, labelled).ok);
      std::array<uint64_t, TRACE_MOVES> const got{ layout_test_culled() };
      for (uint32_t k = 0; k < TRACE_MOVES; ++k) { culled[k] += got[k]; }
      CHECK(layout_test_cull_mismatches() == 0);
    }
  }
  return culled;
}

}  // namespace

TEST_CASE("search: every move culled as changing nothing lays out as the incumbent") {
  // Each culled move is laid out whole and compared with the incumbent it was culled from.
  CullGuard const guard;
  std::array<uint64_t, TRACE_MOVES> const culled{ culls_checked(
      { "brew.scav", "dock.scav", "estop.scav", "kiln.scav", "led.scav" }) };
  CHECK(culled[TRACE_MOVE_RANK] > 0);
  CHECK(culled[TRACE_MOVE_LOOP] > 0);
}

TEST_CASE("search: every move culled on the corpus lays out as the incumbent" *
          doctest::test_suite("full")) {
  CullGuard const guard;
  std::array<uint64_t, TRACE_MOVES> const culled{ culls_checked({ "axis.scav",
                                                                  "bottler.scav",
                                                                  "elevator.scav",
                                                                  "ota.scav",
                                                                  "printer.scav",
                                                                  "tcp.scav",
                                                                  "toolchanger.scav",
                                                                  "vac.scav" }) };
  CHECK(culled[TRACE_MOVE_RANK] > 0);
  CHECK(culled[TRACE_MOVE_LOOP] > 0);
}

TEST_CASE("search: with culling off, nothing is culled") {
  CullGuard const guard;
  layout_test_cull(false, false);
  REQUIRE(lay_out("kiln.scav").ok);
  for (uint64_t const n : layout_test_culled()) { CHECK(n == 0); }
}

namespace {

struct PrefixGuard {
  PrefixGuard() = default;
  PrefixGuard(PrefixGuard const &) = delete;
  PrefixGuard &operator=(PrefixGuard const &) = delete;
  ~PrefixGuard() {
    layout_test_prefix_shortcut(true);
    layout_test_prefix_verify(false);
  }
};

}  // namespace

TEST_CASE(
    "search: face moves scored from the incumbent's prefix find what scoring them whole "
    "finds" *
    doctest::test_suite("full")) {
  PrefixGuard const guard;
  constexpr std::array<char const *, 5> CHARTS{ "axis.scav",
                                                "brew.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint64_t used{ 0 };
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    layout_test_prefix_shortcut(true);
    Laid const with{ lay_out(name) };
    used += layout_test_prefix_used();
    layout_test_prefix_shortcut(false);
    Laid const without{ lay_out(name) };
    CHECK(layout_test_prefix_used() == 0);
    REQUIRE(with.ok);
    REQUIRE(without.ok);
    CHECK(with.structural == without.structural);
    CHECK(with.coordinate == without.coordinate);
  }
  CHECK(used > 0);
}

TEST_CASE("search: every face move scored from the prefix scores as the whole way does") {
  // Each shortcut is also scored whole and compared, including moves that lose.
  PrefixGuard const guard;
  layout_test_prefix_verify(true);
  constexpr std::array<char const *, 5> CHARTS{ "axis.scav",
                                                "brew.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint64_t used{ 0 };
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    layout_test_prefix_verify(true);
    REQUIRE(lay_out(name).ok);
    used += layout_test_prefix_used();
    CHECK(layout_test_prefix_mismatches() == 0);
  }
  CHECK(used > 0);
}

namespace {

struct BoundGuard {
  BoundGuard() = default;
  BoundGuard(BoundGuard const &) = delete;
  BoundGuard &operator=(BoundGuard const &) = delete;
  ~BoundGuard() { layout_test_label_bound(true, false); }
};

}  // namespace

TEST_CASE("search: a labelled round scored by bound lays out what scoring it whole does" *
          doctest::test_suite("full")) {
  BoundGuard const guard;
  constexpr std::array<char const *, 3> CHARTS{ "estop.scav", "brew.scav", "dock.scav" };
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    layout_test_label_bound(true, false);
    Laid const with{ lay_out(name, true) };
    uint64_t const skipped{ layout_test_label_bound_skipped() };
    uint64_t const labelled{ layout_test_label_bound_labelled() };
    layout_test_label_bound(false, false);
    Laid const without{ lay_out(name, true) };
    REQUIRE(with.ok);
    REQUIRE(without.ok);
    CHECK(with.structural == without.structural);
    CHECK(with.coordinate == without.coordinate);
    CHECK(skipped > 0);
    CHECK(labelled > 0);
    CHECK(layout_test_label_bound_skipped() == 0);
  }
}

TEST_CASE("search: every bounded round picks what scoring each candidate whole picks" *
          doctest::test_suite("full")) {
  // Each round is also scored whole: the same pick at the same cost, no bound above a
  // cost; each candidate labelled on its kept routes lays out what a full lay-out does.
  BoundGuard const guard;
  constexpr std::array<char const *, 4> CHARTS{ "estop.scav",
                                                "brew.scav",
                                                "dock.scav",
                                                "gauntlet/carried.scav" };
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    layout_test_label_bound(true, true);
    REQUIRE(lay_out(name, true).ok);
    CHECK(layout_test_label_bound_skipped() > 0);
    CHECK(layout_test_label_bound_labelled() > 0);
    CHECK(layout_test_label_bound_mismatches() == 0);
  }
}

TEST_CASE("search: with no path boxes nothing is bounded") {
  BoundGuard const guard;
  layout_test_label_bound(true, false);
  REQUIRE(lay_out("brew.scav").ok);
  CHECK(layout_test_label_bound_skipped() == 0);
  CHECK(layout_test_label_bound_labelled() == 0);
}

TEST_CASE("search schedules: each row keeps the cheaper of its two searches" *
          doctest::test_suite("full")) {
  constexpr std::array<char const *, 6> CHARTS{ "axis.scav", "brew.scav",
                                                "ota.scav",  "tcp.scav",
                                                "vac.scav",  "gauntlet/carried.scav" };
  uint32_t won_somewhere{ 0 };  // charts where some row took the second search
  uint32_t won_nowhere{ 0 };    // charts where every row kept the first
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    REQUIRE(lay_out(name).ok);
    std::vector<Cost> const &first{ layout_test_schedule_first() };
    std::vector<Cost> const &second{ layout_test_schedule_second() };
    std::vector<Cost> const &kept{ layout_test_schedule_kept() };
    REQUIRE(first.size() == kept.size());
    REQUIRE(second.size() == kept.size());
    uint32_t wins{ 0 };
    for (uint32_t i = 0; i < kept.size(); ++i) {
      CAPTURE(i);
      bool const won{ cost_less(second[i], first[i]) };
      Cost const &want{ won ? second[i] : first[i] };
      CHECK(kept[i].t0_violations == want.t0_violations);
      CHECK(kept[i].t2 == want.t2);
      CHECK_FALSE(cost_less(first[i], kept[i]));
      wins += won ? 1U : 0U;
    }
    ++((wins != 0) ? won_somewhere : won_nowhere);
  }
  CHECK(won_somewhere > 0);
  CHECK(won_nowhere > 0);
}

namespace {

// Turns the candidate memo back on, unverified, on scope exit.
struct CandidateGuard {
  CandidateGuard() = default;
  CandidateGuard(CandidateGuard const &) = delete;
  CandidateGuard &operator=(CandidateGuard const &) = delete;
  ~CandidateGuard() { layout_test_candidate_memo(true, false); }
};

uint64_t total(std::array<uint64_t, TRACE_MOVES> const &by_kind) {
  uint64_t out{ 0 };
  for (uint64_t const n : by_kind) { out += n; }
  return out;
}

}  // namespace

TEST_CASE("search: the candidate memo lays out what scoring every move in full does" *
          doctest::test_suite("full")) {
  CandidateGuard const guard;
  constexpr std::array<char const *, 6> CHARTS{ "axis.scav", "brew.scav", "kiln.scav",
                                                "ota.scav",  "tcp.scav",  "vac.scav" };
  uint64_t deduped{ 0 };
  uint64_t faced{ 0 };
  for (bool const labelled : { false, true }) {
    for (char const *name : CHARTS) {
      if (scav::test::corpus_skipped(name)) { continue; }
      CAPTURE(labelled);
      CAPTURE(name);
      layout_test_candidate_memo(true, false);
      Laid const with{ lay_out(name, labelled) };
      deduped += layout_test_candidate_memo_deduped();
      faced += layout_test_candidate_memo_faced();
      layout_test_candidate_memo(false, false);
      Laid const without{ lay_out(name, labelled) };
      CHECK(layout_test_candidate_memo_deduped() == 0);
      REQUIRE(with.ok);
      REQUIRE(without.ok);
      CHECK(with.structural == without.structural);
      CHECK(with.coordinate == without.coordinate);
    }
  }
  CHECK(deduped > 0);
  CHECK(faced > 0);
}

TEST_CASE(
    "search: every move the candidate memo answers scores as that move laid out "
    "afresh") {
  // Each answer, taken or not, by laid ordering, by drawing or from the facing memo, is
  // checked.
  CandidateGuard const guard;
  constexpr std::array<char const *, 3> CHARTS{ "brew.scav", "dock.scav", "estop.scav" };
  uint64_t deduped{ 0 };
  uint64_t drawn{ 0 };
  uint64_t faced{ 0 };
  for (bool const labelled : { false, true }) {
    for (char const *name : CHARTS) {
      CAPTURE(labelled);
      CAPTURE(name);
      layout_test_candidate_memo(true, true);
      REQUIRE(lay_out(name, labelled).ok);
      deduped += layout_test_candidate_memo_deduped();
      drawn += layout_test_candidate_memo_drawn();
      faced += layout_test_candidate_memo_faced();
      CHECK(layout_test_candidate_memo_mismatches() == 0);
    }
  }
  CHECK(drawn > 0);
  CHECK(deduped > drawn);
  CHECK(faced > 0);
}

TEST_CASE(
    "search: every move the candidate memo answers on the corpus scores as laid out "
    "afresh" *
    doctest::test_suite("full")) {
  CandidateGuard const guard;
  constexpr std::array<char const *, 5> CHARTS{ "axis.scav",
                                                "kiln.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint64_t deduped{ 0 };
  for (bool const labelled : { false, true }) {
    for (char const *name : CHARTS) {
      if (scav::test::corpus_skipped(name)) { continue; }
      CAPTURE(labelled);
      CAPTURE(name);
      layout_test_candidate_memo(true, true);
      REQUIRE(lay_out(name, labelled).ok);
      deduped += layout_test_candidate_memo_deduped();
      CHECK(layout_test_candidate_memo_mismatches() == 0);
    }
  }
  CHECK(deduped > 0);
}

TEST_CASE(
    "search: a candidate memo that empties every few kilobytes lays out the same and "
    "answers exactly") {
  struct Budget {
    Budget() = default;
    Budget(Budget const &) = delete;
    Budget &operator=(Budget const &) = delete;
    ~Budget() { layout_test_candidate_memo_budget(CandidateMemo::BUDGET); }
  } const budget;
  CandidateGuard const guard;
  constexpr std::array<char const *, 2> CHARTS{ "brew.scav", "dock.scav" };
  uint64_t deduped{ 0 };
  for (bool const labelled : { false, true }) {
    for (char const *name : CHARTS) {
      CAPTURE(labelled);
      CAPTURE(name);
      layout_test_candidate_memo_budget(CandidateMemo::BUDGET);
      layout_test_candidate_memo(false, false);
      Laid const without{ lay_out(name, labelled) };
      layout_test_candidate_memo_budget(4096);
      layout_test_candidate_memo(true, true);
      Laid const with{ lay_out(name, labelled) };
      deduped += layout_test_candidate_memo_deduped();
      CHECK(layout_test_candidate_memo_mismatches() == 0);
      REQUIRE(with.ok);
      REQUIRE(without.ok);
      CHECK(with.structural == without.structural);
      CHECK(with.coordinate == without.coordinate);
    }
  }
  CHECK(deduped > 0);
}

namespace {

// `name` laid out on one thread at `readable` with `rows` Level 2 rows: its search counts.
SearchStats counted(char const *name, uint32_t rows) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  Chart c;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
  scav_profile p{ readable() };
  p.portfolio_m = static_cast<int32_t>(rows);
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 1 };
  std::vector<scav_placed> placed;
  SearchStats out;
  search_stats_set(&out);
  bool const laid{ layout_run(c, {}, opts, placed, diags) };
  search_stats_set(nullptr);
  REQUIRE(laid);
  return out;
}

}  // namespace

TEST_CASE("search: two rows that draw every candidate alike lay each out once") {
  // Rows 0 and 1 differ only in `trybox`, and no frame of these charts packs by box. On
  // one thread row 1's searches follow row 0's, and the memo answers each by its drawing.
  CandidateGuard const guard;
  layout_test_candidate_memo(true, true);
  constexpr std::array<char const *, 3> CHARTS{ "gauntlet/above.scav",
                                                "gauntlet/long.scav",
                                                "gauntlet/ring.scav" };
  for (char const *name : CHARTS) {
    CAPTURE(name);
    SearchStats const one{ counted(name, 1) };
    SearchStats const two{ counted(name, 2) };
    CHECK(total(two.offered) > total(one.offered));
    CHECK((total(two.offered) - total(two.deduped) - total(two.pruned)) ==
          (total(one.offered) - total(one.deduped) - total(one.pruned)));
    CHECK(two.drawn > one.drawn);
  }
  CHECK(layout_test_candidate_memo_mismatches() == 0);
}

TEST_CASE(
    "search stats: a layout's moves by kind, the memo's answers among them, and "
    "the same moves without it") {
  CandidateGuard const guard;
  SearchStats on;
  search_stats_set(&on);
  bool const laid_on{ lay_out("brew.scav", true).ok };
  search_stats_set(nullptr);
  layout_test_candidate_memo(false, false);
  SearchStats off;
  search_stats_set(&off);
  bool const laid_off{ lay_out("brew.scav", true).ok };
  search_stats_set(nullptr);
  REQUIRE(laid_on);
  REQUIRE(laid_off);
  // With no sink set, nothing is counted.
  layout_test_candidate_memo(true, false);
  SearchStats const before{ on };
  REQUIRE(lay_out("brew.scav", true).ok);
  CHECK(total(on.offered) == total(before.offered));

  for (uint32_t k = 0; k < TRACE_MOVES; ++k) {
    CAPTURE(k);
    CHECK(on.offered[k] == off.offered[k]);
    CHECK(on.taken[k] == off.taken[k]);
    CHECK(on.deduped[k] <= on.offered[k]);
    CHECK(off.deduped[k] == 0);
  }
  CHECK(total(on.offered) > 0);
  CHECK(total(on.deduped) > 0);
  CHECK(total(on.taken) > 0);
  CHECK(on.drawn > 0);
  CHECK(on.drawn <= total(on.deduped));
  CHECK(off.drawn == 0);
  CHECK(on.faced > 0);
  CHECK(off.faced == 0);
  CHECK(on.searches > 0);
  CHECK(on.searches == off.searches);
  CHECK(on.recalled == off.recalled);
  CHECK(on.memo_bytes > 0);
  CHECK(off.memo_bytes == 0);
}

namespace {

// Re-enables pruning by route bound and disables its check on scope exit.
struct RouteBoundGuard {
  RouteBoundGuard() = default;
  RouteBoundGuard(RouteBoundGuard const &) = delete;
  RouteBoundGuard &operator=(RouteBoundGuard const &) = delete;
  ~RouteBoundGuard() { layout_test_route_bound(true, false); }
};

// Lays out each chart of `charts` at both scales and both searches with pruning checked,
// and again with pruning off; the two drawings match and every check holds.
template <size_t N>
void check_route_bound(std::array<char const *, N> const &charts) {
  RouteBoundGuard const guard;
  uint64_t pruned{ 0 };
  uint64_t checked{ 0 };
  for (bool const culled : { false, true }) {
    for (bool const labelled : { false, true }) {
      for (char const *name : charts) {
        if (scav::test::corpus_skipped(name)) { continue; }
        CAPTURE(culled);
        CAPTURE(labelled);
        CAPTURE(name);
        layout_test_route_bound(true, true);
        Laid const with{ lay_out(name, labelled, culled) };
        pruned += layout_test_route_bound_pruned();
        checked += layout_test_route_bound_checked();
        CHECK(layout_test_route_bound_mismatches() == 0);
        layout_test_route_bound(false, false);
        Laid const without{ lay_out(name, labelled, culled) };
        CHECK(layout_test_route_bound_pruned() == 0);
        REQUIRE(with.ok);
        REQUIRE(without.ok);
        CHECK(with.structural == without.structural);
        CHECK(with.coordinate == without.coordinate);
      }
    }
  }
  CHECK(pruned > 0);
  CHECK(checked > pruned);
}

}  // namespace

TEST_CASE("search: a move pruned by its route bound scores at least that bound") {
  check_route_bound(std::array<char const *, 3>{ "estop.scav", "led.scav", "dock.scav" });
}

namespace {

// Re-enables the cost stop and pruning by route bound, its check off, on scope exit.
struct CostStopGuard {
  CostStopGuard() = default;
  CostStopGuard(CostStopGuard const &) = delete;
  CostStopGuard &operator=(CostStopGuard const &) = delete;
  ~CostStopGuard() {
    layout_test_cost_stop(true);
    layout_test_route_bound(true, false);
  }
};

}  // namespace

TEST_CASE("search: a move stopped at its Tier 2 scores at or above it, and draws alike") {
  CostStopGuard const guard;
  uint64_t stopped{ 0 };
  for (bool const culled : { false, true }) {
    for (bool const labelled : { false, true }) {
      for (char const *name : { "estop.scav", "led.scav", "dock.scav" }) {
        CAPTURE(culled);
        CAPTURE(labelled);
        CAPTURE(name);
        layout_test_cost_stop(true);
        layout_test_route_bound(true, true);
        Laid const with{ lay_out(name, labelled, culled) };
        stopped += layout_test_cost_stopped();
        CHECK(layout_test_route_bound_mismatches() == 0);
        layout_test_cost_stop(false);
        layout_test_route_bound(true, false);
        Laid const without{ lay_out(name, labelled, culled) };
        CHECK(layout_test_cost_stopped() == 0);
        REQUIRE(with.ok);
        REQUIRE(without.ok);
        CHECK(with.structural == without.structural);
        CHECK(with.coordinate == without.coordinate);
      }
    }
  }
  CHECK(stopped > 0);
}

TEST_CASE("search: on the corpus, every route bound lies at or below its move's score" *
          doctest::test_suite("full")) {
  check_route_bound(std::array<char const *, 6>{ "axis.scav",
                                                 "brew.scav",
                                                 "kiln.scav",
                                                 "ota.scav",
                                                 "tcp.scav",
                                                 "vac.scav" });
}

namespace {

bool same_costs(std::vector<Cost> const &a, std::vector<Cost> const &b) {
  if (a.size() != b.size()) { return false; }
  for (size_t i = 0; i < a.size(); ++i) {
    if ((a[i].t0_violations != b[i].t0_violations) || (a[i].t2 != b[i].t2)) {
      return false;
    }
  }
  return true;
}

// Each row's cost after its first search, its refold, and the one kept.
struct Schedules {
  std::vector<Cost> first, second, kept;
};

Schedules schedules() {
  return { .first = layout_test_schedule_first(),
           .second = layout_test_schedule_second(),
           .kept = layout_test_schedule_kept() };
}

}  // namespace

TEST_CASE("search: rows differing only in knobs no sizing reads search as one, alike") {
  // No state of estop or led owns a submachine, so no row reads the owner's hole.
  struct Guard {
    Guard() = default;
    Guard(Guard const &) = delete;
    Guard &operator=(Guard const &) = delete;
    ~Guard() { layout_test_row_alias(true); }
  } const guard;
  for (bool const culled : { false, true }) {
    for (bool const labelled : { false, true }) {
      for (char const *name : { "estop.scav", "led.scav" }) {
        CAPTURE(culled);
        CAPTURE(labelled);
        CAPTURE(name);
        layout_test_row_alias(true);
        SearchStats on;
        search_stats_set(&on);
        Laid const with{ lay_out(name, labelled, culled) };
        search_stats_set(nullptr);
        Schedules const aliased{ schedules() };
        layout_test_row_alias(false);
        SearchStats off;
        search_stats_set(&off);
        Laid const without{ lay_out(name, labelled, culled) };
        search_stats_set(nullptr);
        CHECK(on.aliased > 0);
        CHECK(off.aliased == 0);
        CHECK(on.searches < off.searches);
        REQUIRE(with.ok);
        REQUIRE(without.ok);
        CHECK(with.structural == without.structural);
        CHECK(with.coordinate == without.coordinate);
        // Each row reaches the same cost at every stage of its search.
        Schedules const apart{ schedules() };
        CHECK(same_costs(aliased.first, apart.first));
        CHECK(same_costs(aliased.second, apart.second));
        CHECK(same_costs(aliased.kept, apart.kept));
      }
    }
  }
}
