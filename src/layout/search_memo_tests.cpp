// What a Level 1 search leaves undone and still finds the same: the memo of
// whole searches, whose key tells apart every input a search is a function
// of; the faces a pin has no effect at, left unscored; and a face
// move scored from the incumbent's phases 1 and 2. Each is compared against a
// layout that does the work, with the shortcut taken.

#include "layout/pack.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

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
                SearchPins const &seed,
                std::vector<uint8_t> const *scope,
                std::vector<uint32_t> &key);
void layout_test_prefix_shortcut(bool on);
void layout_test_prefix_verify(bool on);
uint64_t layout_test_prefix_used();
uint64_t layout_test_prefix_mismatches();
void layout_test_skip_noop_faces(bool on);
uint64_t layout_test_noop_faces();
void layout_test_search_memo(bool on);
void layout_test_search_memo_verify(bool on);
uint32_t layout_test_search_memo_hits();
uint32_t layout_test_search_memo_mismatches();

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

Laid lay_out(char const *name) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  Chart c;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
  std::vector<scav_placed> placed;
  scav_layout_opts const opts{ .profile = readable(), .router = 0, .threads = 0 };
  Laid out;
  out.ok = layout_run(c, {}, opts, placed, diags);
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
  base.scope.assign(4, 0);

  std::vector<Inputs> variants(16, base);
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

TEST_CASE("search memo: a layout searched through it is the one searched without it") {
  MemoGuard const guard;
  constexpr std::array<char const *, 5> CHARTS{ "axis.scav",
                                                "brew.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint32_t hits{ 0 };
  for (char const *name : CHARTS) {
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
  // The memo answered searches, so the equality above held across real hits.
  CHECK(hits > 0);
}

TEST_CASE("search memo: every search it answers is the search run afresh") {
  // Each answer is checked against running that search anyway -- the pins it
  // reached, its cost, and the drawing re-derived from them -- including the
  // many answers to searches whose result is not taken.
  MemoGuard const guard;
  layout_test_search_memo_verify(true);
  constexpr std::array<char const *, 4> CHARTS{ "axis.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint32_t hits{ 0 };
  for (char const *name : CHARTS) {
    CAPTURE(name);
    REQUIRE(lay_out(name).ok);
    hits += layout_test_search_memo_hits();
    CHECK(layout_test_search_memo_mismatches() == 0);
  }
  CHECK(hits > 0);
}

TEST_CASE(
    "search: faces with no effect, left unscored, find the layout scoring them finds") {
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
    "finds") {
  PrefixGuard const guard;
  constexpr std::array<char const *, 5> CHARTS{ "axis.scav",
                                                "brew.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint64_t used{ 0 };
  for (char const *name : CHARTS) {
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
  // Each shortcut is scored the whole way as well and the two compared -- the
  // score, its shares, the drawing and the pins it was laid with -- including
  // the many moves that lose, whose score no drawing would show.
  PrefixGuard const guard;
  layout_test_prefix_verify(true);
  constexpr std::array<char const *, 4> CHARTS{ "axis.scav",
                                                "ota.scav",
                                                "tcp.scav",
                                                "vac.scav" };
  uint64_t used{ 0 };
  for (char const *name : CHARTS) {
    CAPTURE(name);
    layout_test_prefix_verify(true);
    REQUIRE(lay_out(name).ok);
    used += layout_test_prefix_used();
    CHECK(layout_test_prefix_mismatches() == 0);
  }
  CHECK(used > 0);
}
