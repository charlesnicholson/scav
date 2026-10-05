// The candidate memo's keys and records on a real chart's orderings.

#include "layout/candidate_memo.h"
#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace scav;

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

struct Fixture {
  Chart c;
  SplitGraph g;
  scav_spaces s{ .box_state_stride = sizeof(scav_box_space),
                 .path_box_stride = sizeof(scav_path_box) };
  scav_profile p{ readable() };
  Row row{ .knobs = readable() };

  Fixture() {
    std::string path{ SCAV_TEST_DATA_DIR "/charts/kiln.scav" };
    Loader loader;
    std::vector<Diagnostic> diags;
    std::string failed;
    REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
    g = decompose(c);
  }

  [[nodiscard]] SubmachineOrders order(SearchPins const &pins = {}) const {
    return order_submachines(c, g, s, p, 1, pins);
  }

  // A rank pin that changes the ordering.
  [[nodiscard]] SearchPins moved_rank() const {
    SubmachineOrders const plain{ order() };
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if ((c.states[st].live == 0) || (c.states[st].kind == StateKind::Initial) ||
          (plain.state_node[st] == INVALID)) {
        continue;
      }
      uint32_t const ranks{ plain.sub_ranks[c.states[st].parent.v] };
      uint32_t const at{ plain.nodes[plain.state_node[st]].rank };
      for (uint32_t r = 0; r < ranks; ++r) {
        SearchPins pins;
        pins.ranks.push_back({ .state = StateId{ st }, .rank = r });
        if ((r != at) && (order(pins).nodes != plain.nodes)) { return pins; }
      }
    }
    FAIL("no rank pin moves kiln");
    return {};
  }

  // The first leg end of each kind: with a port, and without.
  void ends(EndPin &boxed, EndPin &ported) const {
    bool have_box{ false };
    bool have_port{ false };
    for (uint32_t t = 0; t < g.trans_segments.size(); ++t) {
      Span const segs{ g.trans_segments[t] };
      for (uint32_t leg = 0; leg < segs.len; ++leg) {
        SplitSegment const &sg{ g.segments[segs.off + leg] };
        if (!have_box && (sg.src_port == INVALID)) {
          boxed = { .trans = TransId{ t }, .leg = leg, .end = 0, .face = 2 };
          have_box = true;
        }
        if (!have_port && (sg.src_port != INVALID)) {
          ported = { .trans = TransId{ t }, .leg = leg, .end = 0, .face = 2 };
          have_port = true;
        }
      }
    }
    REQUIRE(have_box);
    REQUIRE(have_port);
  }
};

}  // namespace

TEST_CASE(
    "candidate memo: an ordering numbers its frames alike each time, a moved frame "
    "anew") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  SubmachineOrders const moved{ f.order(f.moved_rank()) };
  std::vector<uint32_t> a;
  std::vector<uint32_t> again;
  std::vector<uint32_t> b;
  REQUIRE(memo.frame_ids(plain, a));
  REQUIRE(memo.frame_ids(moved, b));
  REQUIRE(memo.frame_ids(plain, again));
  CHECK(a.size() == (f.c.submachines.size() + 1));
  CHECK(again == a);
  CHECK(b != a);
  // Frames the pin left alone keep their numbers.
  uint32_t kept{ 0 };
  for (uint32_t m = 0; m < a.size(); ++m) { kept += (a[m] == b[m]) ? 1U : 0U; }
  CHECK(kept > 0);
  CHECK(kept < a.size());
  // A second memo numbers the same frames from scratch, consistently.
  CandidateMemo other{ f.c, f.g };
  std::vector<uint32_t> fresh;
  REQUIRE(other.frame_ids(plain, fresh));
  CHECK(fresh == a);

  uint32_t const arranged{ memo.arrangement(plain) };
  REQUIRE(arranged != INVALID);
  CHECK(memo.arrangement(moved) != arranged);
  CHECK(memo.arrangement(plain) == arranged);
}

TEST_CASE(
    "candidate memo: a score entry is the row, the laid arrangement and the box-end "
    "faces") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  uint32_t const plain{ memo.arrangement(f.order()) };
  uint32_t const moved{ memo.arrangement(f.order(f.moved_rank())) };
  REQUIRE(plain != INVALID);
  REQUIRE(moved != INVALID);
  uint32_t const row{ memo.row_word(f.row) };
  Row other_row{ f.row };
  other_row.pack = Compaction::On;
  uint32_t const row2{ memo.row_word(other_row) };
  CHECK(row2 != row);
  CHECK(memo.row_word(f.row) == row);

  EndPin boxed;
  EndPin ported;
  f.ends(boxed, ported);
  auto const entry = [&](uint32_t r, uint32_t arranged, std::vector<EndPin> const &ends) {
    SearchPins pins;
    pins.ends = ends;
    return memo.find_score(r, arranged, &pins, true).entry;
  };
  uint32_t const base{ entry(row, plain, {}) };
  REQUIRE(base != INVALID);
  CHECK(entry(row, plain, {}) == base);
  CHECK(entry(row2, plain, {}) != base);
  CHECK(entry(row, moved, {}) != base);
  // The router reads no pin at a port end.
  CHECK(entry(row, plain, { ported }) == base);
  uint32_t const faced{ entry(row, plain, { boxed }) };
  CHECK(faced != base);
  // The last pin naming an end decides it, and pins naming different ends commute.
  EndPin other_face{ boxed };
  other_face.face = 3;
  CHECK(entry(row, plain, { other_face, boxed }) == faced);
  CHECK(entry(row, plain, { boxed, other_face }) != faced);
  EndPin arrival{ boxed };
  arrival.end = 1;
  CHECK(entry(row, plain, { boxed, arrival }) == entry(row, plain, { arrival, boxed }));
  CHECK(entry(row, plain, { boxed, arrival }) != faced);
}

TEST_CASE("candidate memo: a score comes back as stored, each of its two kinds apart") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  uint32_t const plain{ memo.arrangement(f.order()) };
  uint32_t const row{ memo.row_word(f.row) };
  CandidateMemo::Recalled const first{ memo.find_score(row, plain, nullptr, false) };
  REQUIRE(first.entry != INVALID);
  CHECK_FALSE(first.found);
  uint32_t const e{ first.entry };

  MemoScore const bound{ .cost = { .t0_violations = 2, .t1_hints = 0, .t2 = 5 },
                         .viable = true };
  MemoScore const labelled{
    .cost = { .t0_violations = 0, .t1_hints = 0, .t2 = (int64_t{ 1 } << 40) + 7 },
    .viable = true
  };
  memo.set_score(e, false, bound);
  MemoScore got;
  CHECK_FALSE(memo.score(e, true, got));
  REQUIRE(memo.score(e, false, got));
  CHECK(got.viable);
  CHECK_FALSE(got.inflated);
  CHECK(got.cost.t0_violations == 2);
  CHECK(got.cost.t2 == 5);
  // A labelled request waits for the labelled score; a bound request takes the bound.
  CHECK_FALSE(memo.find_score(row, plain, nullptr, true).found);
  CandidateMemo::Recalled const as_bound{ memo.find_score(row, plain, nullptr, false) };
  CHECK(as_bound.found);
  CHECK_FALSE(as_bound.labelled);
  CHECK(as_bound.entry == e);

  memo.set_score(e, true, labelled);
  CandidateMemo::Recalled const as_labelled{ memo.find_score(row, plain, nullptr, true) };
  REQUIRE(as_labelled.found);
  CHECK(as_labelled.labelled);
  CHECK(as_labelled.score.cost.t2 == labelled.cost.t2);
  CHECK(as_labelled.score.cost.t0_violations == 0);

  // A bound request with only the labelled score set takes the labelled one.
  uint32_t const moved{ memo.arrangement(f.order(f.moved_rank())) };
  uint32_t const e2{ memo.find_score(row, moved, nullptr, true).entry };
  memo.set_score(e2, true, { .viable = true, .inflated = true });
  CandidateMemo::Recalled const inflated{ memo.find_score(row, moved, nullptr, false) };
  REQUIRE(inflated.found);
  CHECK(inflated.labelled);
  CHECK(inflated.score.viable);
  CHECK(inflated.score.inflated);
  memo.set_score(e2, false, {});
  REQUIRE(memo.score(e2, false, got));
  CHECK_FALSE(got.viable);
  CHECK(memo.peak_bytes() > 0);
}

TEST_CASE(
    "candidate memo: the facing memo gives back the turns stored under the ordering "
    "and its sided ports") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  SubmachineOrders const moved{ f.order(f.moved_rank()) };
  uint32_t const plain_at{ memo.arrangement(plain) };
  uint32_t const moved_at{ memo.arrangement(moved) };
  uint32_t const row{ memo.row_word(f.row) };

  Facing out;
  CHECK(memo.find_facing(row, plain_at, plain, out) == FacingFound::Absent);
  Facing turns;
  turns.reverses.push_back({ .trans = TransId{ 1 }, .leg = 0 });
  turns.reverses.push_back({ .trans = TransId{ 4 }, .leg = 2 });
  turns.sides.push_back({ .trans = TransId{ 3 }, .leg = 1, .end = 1, .face = 2 });
  memo.store_facing(row, plain_at, plain, &turns);
  REQUIRE(memo.find_facing(row, plain_at, plain, out) == FacingFound::Turned);
  REQUIRE(out.reverses.size() == 2);
  REQUIRE(out.sides.size() == 1);
  CHECK(out.reverses[0].trans.v == 1);
  CHECK(out.reverses[1].trans.v == 4);
  CHECK(out.reverses[1].leg == 2);
  CHECK(out.sides[0].trans.v == 3);
  CHECK(out.sides[0].leg == 1);
  CHECK(out.sides[0].end == 1);
  CHECK(out.sides[0].face == 2);

  memo.store_facing(row, moved_at, moved, nullptr);
  CHECK(memo.find_facing(row, moved_at, moved, out) == FacingFound::Failed);
  CHECK(memo.find_facing(memo.row_word({ .knobs = f.p, .fold = Fold::Always }),
                         plain_at,
                         plain,
                         out) == FacingFound::Absent);
  // The facing pass reads which ports an end pin sided.
  SubmachineOrders sided{ plain };
  REQUIRE(!sided.seg_sided.empty());
  sided.seg_sided[0] = (plain.seg_sided[0] == 0) ? 1 : 0;
  CHECK(memo.find_facing(row, plain_at, sided, out) == FacingFound::Absent);
}

TEST_CASE("candidate memo: past its budget it empties, and issues no number twice") {
  Fixture const f;
  // A budget one entry fills: every insert past the first empties the tables.
  CandidateMemo memo{ f.c, f.g, 1 };
  uint32_t const row{ memo.row_word(f.row) };
  uint32_t const plain{ memo.arrangement(f.order()) };
  REQUIRE(plain != INVALID);
  uint32_t const e{ memo.find_score(row, plain, nullptr, true).entry };
  REQUIRE(e != INVALID);
  memo.set_score(
      e,
      true,
      { .cost = { .t0_violations = 0, .t1_hints = 0, .t2 = 9 }, .viable = true });
  // Emptied away, the entry reads unset and takes no score.
  MemoScore got;
  CHECK_FALSE(memo.score(e, true, got));
  // The same key comes back under a new number.
  uint32_t const again{ memo.find_score(row, plain, nullptr, true).entry };
  REQUIRE(again != INVALID);
  CHECK(again != e);
  CHECK_FALSE(memo.find_score(row, plain, nullptr, true).found);
  uint32_t const moved{ memo.arrangement(f.order(f.moved_rank())) };
  REQUIRE(moved != INVALID);
  CHECK(moved != plain);
  CHECK(memo.peak_bytes() > 0);
}
