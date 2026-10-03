// Level 1 search shortcuts, each against a layout that does the work: the search memo,
// unscored no-op faces, and face moves scored from the incumbent's prefix.

#include "layout/pack.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "core/tests/corpus.h"
#include "doctest.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scav {

void search_key(scav_profile const &objective,
                scav_profile const &knobs,
                DarSource dar,
                Compaction pack,
                Fold fold,
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
void layout_test_label_bound(bool on, bool verify);
uint64_t layout_test_label_bound_skipped();
uint64_t layout_test_label_bound_labelled();
uint64_t layout_test_label_bound_mismatches();
void layout_test_search_memo(bool on);
void layout_test_search_memo_verify(bool on);
uint32_t layout_test_search_memo_hits();
uint32_t layout_test_search_memo_mismatches();
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
  scav_profile knobs{};
  DarSource dar{ DarSource::Profile };
  Compaction pack{ Compaction::Off };
  Fold fold{ Fold::Scale };
  uint32_t budget{ 64 };
  bool refold{ false };
  SearchPins seed;
  std::vector<uint8_t> scope;
  bool scoped{ false };
};

std::vector<uint32_t> key_of(Inputs const &in) {
  std::vector<uint32_t> key;
  search_key(in.objective,
             in.knobs,
             in.dar,
             in.pack,
             in.fold,
             in.budget,
             in.refold,
             in.seed,
             in.scoped ? &in.scope : nullptr,
             key);
  return key;
}

// Restores the memo whatever a case leaves it as.
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

// With `labelled`, a path box for every routed transition, so a layout places labels.
Laid lay_out(char const *name, bool labelled = false) {
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
    if ((tr.live == 0) || ((tr.src == tr.dst) && (tr.kind != TransKind::External))) {
      continue;
    }
    boxes.push_back({ .subject = t, .w = 40, .h = 12, .order = 0 });
  }
  scav_spaces const s{ .box_state_stride = sizeof(scav_box_space),
                       .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()),
                       .path_box_stride = sizeof(scav_path_box) };
  std::vector<scav_placed> placed;
  scav_layout_opts const opts{ .profile = readable(), .router = 0, .threads = 0 };
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
  base.knobs = readable();
  base.seed.ranks.push_back({ .state = StateId{ 3 }, .rank = 1 });
  base.seed.ranks.push_back({ .state = StateId{ 5 }, .rank = 2 });
  base.seed.faces.push_back({ .trans = TransId{ 2 }, .leg = 0, .end = 1, .face = 3 });
  base.seed.sides.push_back({ .trans = TransId{ 2 }, .leg = 0, .end = 0, .side = 1 });
  base.scope.assign(4, 0);

  std::vector<Inputs> variants(22, base);
  variants[0].objective.node_sep += 1;
  variants[1].knobs.node_sep += 1;
  variants[2].dar = DarSource::OwnerHole;
  variants[3].pack = Compaction::On;
  variants[4].fold = Fold::Always;
  variants[5].budget += 1;
  variants[6].seed.ranks[1].rank = 3;
  variants[7].seed.cuts.push_back({ .trans = TransId{ 1 }, .leg = 0 });
  variants[8].seed.reverses.push_back({ .trans = TransId{ 1 }, .leg = 0 });
  variants[9].seed.faces[0].face = 2;
  variants[10].seed.orients.push_back({ .frame = SubmachineId{ 0 } });
  // Rank pins apply in order, so the same two in the other order are another start.
  std::swap(variants[11].seed.ranks[0], variants[11].seed.ranks[1]);
  variants[12].scoped = true;
  variants[13].scoped = true;
  variants[13].scope[2] = 1;
  variants[14].scoped = true;
  variants[14].scope[3] = 1;
  variants[15].seed.ranks.pop_back();
  variants[16].seed.sides.push_back(
      { .trans = TransId{ 1 }, .leg = 0, .end = 1, .side = 0 });
  variants[17].seed.sides[0].side = 2;
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
  // Each answer is checked against the search run anyway, including those not taken.
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
