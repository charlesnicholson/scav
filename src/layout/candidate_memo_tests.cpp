// The candidate memo's keys and records on a real chart's orderings.

#include "layout/candidate_memo.h"
#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_pod_vector.h"

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

  [[nodiscard]] SizedLayout size(SubmachineOrders const &o) const {
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, o, s, p, z, diags));
    return z;
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

  // A rank pin under which segment `seg` chains through two bends or more.
  [[nodiscard]] SearchPins chained_rank(uint32_t &seg) const {
    SubmachineOrders const plain{ order() };
    PodVector<uint32_t> reversed;
    std::vector<PodVector<uint32_t>> bends;
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if ((c.states[st].live == 0) || (plain.state_node[st] == INVALID)) { continue; }
      for (uint32_t r = 0; r < plain.sub_ranks[c.states[st].parent.v]; ++r) {
        SearchPins pins;
        pins.ranks.push_back({ .state = StateId{ st }, .rank = r });
        segment_bends(order(pins),
                      static_cast<uint32_t>(g.segments.size()),
                      reversed,
                      bends);
        for (seg = 0; seg < bends.size(); ++seg) {
          if (bends[seg].size() >= 2) { return pins; }
        }
      }
    }
    FAIL("no rank pin chains a kiln segment through two bends");
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
  PodVector<uint32_t> a;
  PodVector<uint32_t> again;
  PodVector<uint32_t> b;
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
  PodVector<uint32_t> fresh;
  REQUIRE(other.frame_ids(plain, fresh));
  CHECK(fresh == a);

  uint32_t const arranged{ memo.arrangement(plain) };
  REQUIRE(arranged != INVALID);
  CHECK(memo.arrangement(moved) != arranged);
  CHECK(memo.arrangement(plain) == arranged);

  // Numbered against another ordering's encoding, a frame takes the number it would take
  // anyway. A call to `other` between clears the thread's last numbering in `memo`.
  CandidateMemo::Blocks encoded;
  REQUIRE(memo.frame_ids(plain, fresh, nullptr, &encoded));
  CHECK(fresh == a);
  CHECK(encoded.ids == a);
  REQUIRE(other.frame_ids(plain, fresh));
  PodVector<uint32_t> alone;
  REQUIRE(memo.frame_ids(moved, alone));
  REQUIRE(other.frame_ids(plain, fresh));
  PodVector<uint32_t> like;
  REQUIRE(memo.frame_ids(moved, like, &encoded));
  CHECK(like == alone);
  CHECK(like == b);
  (void)other.arrangement(plain);
  uint32_t const moved_alone{ memo.arrangement(moved) };
  (void)other.arrangement(plain);
  CHECK(memo.arrangement(moved, &encoded) == moved_alone);
}

TEST_CASE("candidate memo: a grow pin numbers the arrangement holding its state anew") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  uint32_t st{ INVALID };
  for (uint32_t k = 0; (k < f.c.states.size()) && (st == INVALID); ++k) {
    if ((f.c.states[k].live != 0) && (f.c.states[k].kind == StateKind::Normal)) { st = k; }
  }
  REQUIRE(st != INVALID);
  auto const grown = [&](uint32_t w, uint32_t h) {
    return memo.arrangement(
        f.order({ .grows = { { .state = StateId{ st }, .w = w, .h = h } } }));
  };
  uint32_t const plain{ memo.arrangement(f.order()) };
  REQUIRE(plain != INVALID);
  uint32_t const wide{ grown(4, 1) };
  CHECK(wide != plain);
  CHECK(grown(4, 2) != wide);
  CHECK(grown(4, 1) == wide);
}

TEST_CASE(
    "candidate memo: an ordering key is the row, the laid arrangement and the box-end "
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
  auto const key = [&](uint32_t r, uint32_t arranged, std::vector<EndPin> const &ends) {
    SearchPins pins;
    pins.ends = ends;
    PodVector<uint32_t> faces;
    memo.box_faces(&pins, faces);
    return memo.find_ordering(r, arranged, faces).key;
  };
  uint32_t const base{ key(row, plain, {}) };
  REQUIRE(base != INVALID);
  CHECK(key(row, plain, {}) == base);
  CHECK(key(row2, plain, {}) != base);
  CHECK(key(row, moved, {}) != base);
  CHECK(key(row, plain, { ported }) == base);  // the router reads no pin at a port end
  uint32_t const faced{ key(row, plain, { boxed }) };
  CHECK(faced != base);
  // The last pin naming an end decides it, and pins naming different ends commute.
  EndPin other_face{ boxed };
  other_face.face = 3;
  CHECK(key(row, plain, { other_face, boxed }) == faced);
  CHECK(key(row, plain, { boxed, other_face }) != faced);
  EndPin arrival{ boxed };
  arrival.end = 1;
  CHECK(key(row, plain, { boxed, arrival }) == key(row, plain, { arrival, boxed }));
  CHECK(key(row, plain, { boxed, arrival }) != faced);
}

TEST_CASE(
    "candidate memo: a drawing is what routing reads of the sizing and the ordering") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  SizedLayout const z{ f.size(plain) };
  uint32_t const profile{ memo.profile_word(f.p) };
  scav_profile boxing{ f.p };
  boxing.trybox ^= 1;
  CHECK(memo.profile_word(boxing) == profile);  // only sizing reads `trybox`
  scav_profile wider{ f.p };
  wider.node_sep += 1;
  uint32_t const wider_word{ memo.profile_word(wider) };
  CHECK(wider_word != profile);
  // A drawing in `other` clears the thread's last numbering in `memo`.
  CandidateMemo other{ f.c, f.g };
  uint32_t const other_profile{ other.profile_word(f.p) };
  auto const clear = [&]() { (void)other.drawing(plain, z, other_profile); };
  uint32_t const drawn{ memo.drawing(plain, z, profile) };
  REQUIRE(drawn != INVALID);
  clear();
  CHECK(memo.drawing(plain, z, profile) == drawn);
  CHECK(memo.drawing(plain, z, wider_word) != drawn);
  CandidateMemo::Blocks kept;
  CHECK(memo.drawing(plain, z, profile, nullptr, &kept) == drawn);

  // The drawing of `plain` and `z` after `edit`, alike numbered against `kept` or not.
  auto const after = [&](auto const &edit) {
    SubmachineOrders o{ plain };
    SizedLayout w{ z };
    edit(o, w);
    clear();
    uint32_t const alone{ memo.drawing(o, w, profile) };
    clear();
    uint32_t const out{ memo.drawing(o, w, profile, &kept) };
    CHECK(out == alone);
    return out;
  };
  // A nested frame and its first state, a state node, and a segment with a boundary node.
  uint32_t nested{ INVALID };
  for (uint32_t m = 0; (m < f.c.submachines.size()) && (nested == INVALID); ++m) {
    if (f.c.submachines[m].owner.v != INVALID) { nested = m; }
  }
  REQUIRE(nested != INVALID);
  uint32_t const kid{ f.c.state_ids[f.c.submachines[nested].children.off].v };
  uint32_t state_node{ INVALID };
  for (uint32_t n = 0; (n < plain.nodes.size()) && (state_node == INVALID); ++n) {
    if (plain.nodes[n].kind == OrderKind::State) { state_node = n; }
  }
  uint32_t noded{ INVALID };
  for (uint32_t seg = 0; (seg < f.g.segments.size()) && (noded == INVALID); ++seg) {
    if (plain.seg_node[seg] != INVALID) { noded = seg; }
  }
  REQUIRE(state_node != INVALID);
  REQUIRE(noded != INVALID);

  // What routing does not read leaves the drawing alone: fold flags, a state node's
  // point, and the ordering's rank gaps.
  CHECK(after([](SubmachineOrders &, SizedLayout &w) {
          for (uint8_t &fold : w.folded) { fold ^= 1U; }
        }) == drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.node[state_node].x; }) ==
        drawn);
  CHECK(after([](SubmachineOrders &o, SizedLayout &) {
          for (int32_t &gap : o.gaps) { ++gap; }
        }) == drawn);

  // Each input routing, labels or cost read moves it.
  CHECK(after([](SubmachineOrders &, SizedLayout &w) { ++w.chart.w; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.state[kid].y; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.before[kid].h; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.after[kid].h; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.lead[kid].w; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.trail[kid].w; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.loop[kid].x; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { w.loop_place[kid] ^= 1U; }) !=
        drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.sub[nested].x; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { ++w.sub[nested].h; }) != drawn);
  CHECK(after([&](SubmachineOrders &, SizedLayout &w) { w.lean[noded] ^= 1U; }) != drawn);
  CHECK(after([&](SubmachineOrders &o, SizedLayout &w) {
          ++w.node[o.seg_node[noded]].y;
        }) != drawn);
  CHECK(after([&](SubmachineOrders &o, SizedLayout &) { o.seg_node[noded] = INVALID; }) !=
        drawn);
  CHECK(after([&](SubmachineOrders &o, SizedLayout &) { o.seg_side[noded] ^= 1U; }) !=
        drawn);
  CHECK(after([&](SubmachineOrders &o, SizedLayout &) { o.seg_port[noded] = INVALID; }) !=
        drawn);
  // A frame moved whole, with its states and every node, is another drawing.
  auto const shift = [&](SubmachineOrders &, SizedLayout &w) {
    ++w.sub[nested].x;
    Span const kids{ f.c.submachines[nested].children };
    for (uint32_t k = 0; k < kids.len; ++k) { ++w.state[f.c.state_ids[kids.off + k].v].x; }
    for (scav_point &n : w.node) { ++n.x; }
  };
  CHECK(after(shift) != drawn);

  // A bend's point moves the drawing, and so does a reversed edge, which turns its chain
  // of bends end for end.
  uint32_t chained{ INVALID };
  SubmachineOrders const longer{ f.order(f.chained_rank(chained)) };
  REQUIRE(chained < f.g.segments.size());
  SizedLayout const lz{ f.size(longer) };
  uint32_t const long_drawn{ memo.drawing(longer, lz, profile) };
  PodVector<uint32_t> reversed;
  std::vector<PodVector<uint32_t>> bends;
  segment_bends(longer, static_cast<uint32_t>(f.g.segments.size()), reversed, bends);
  REQUIRE(bends[chained].size() >= 2);
  auto const bent = [&](auto const &edit) {
    SubmachineOrders o{ longer };
    SizedLayout w{ lz };
    edit(o, w);
    return memo.drawing(o, w, profile);
  };
  CHECK(bent([&](SubmachineOrders &, SizedLayout &w) { ++w.node[bends[chained][0]].x; }) !=
        long_drawn);
  auto const turned = [&](SubmachineOrders &o, SizedLayout &) {
    for (OrderEdge &e : o.edges) {
      if (e.segment == chained) { e.reversed ^= 1U; }
    }
  };
  CHECK(bent(turned) != long_drawn);
}

TEST_CASE("candidate memo: a score comes back as stored, each of its two kinds apart") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  REQUIRE(drawn != INVALID);
  PodVector<uint32_t> const none;
  CandidateMemo::Recalled const first{ memo.find_score(drawn, none, false) };
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
  CHECK_FALSE(memo.find_score(drawn, none, true).found);
  CandidateMemo::Recalled const as_bound{ memo.find_score(drawn, none, false) };
  CHECK(as_bound.found);
  CHECK_FALSE(as_bound.labelled);
  CHECK(as_bound.entry == e);

  memo.set_score(e, true, labelled);
  CandidateMemo::Recalled const as_labelled{ memo.find_score(drawn, none, true) };
  REQUIRE(as_labelled.found);
  CHECK(as_labelled.labelled);
  CHECK(as_labelled.score.cost.t2 == labelled.cost.t2);
  CHECK(as_labelled.score.cost.t0_violations == 0);
  CandidateMemo::Recalled const by_entry{ memo.recall(e, true) };
  CHECK(by_entry.found);
  CHECK(by_entry.entry == e);
  CHECK(by_entry.score.cost.t2 == labelled.cost.t2);

  // A bound request with only the labelled score set takes the labelled one.
  PodVector<uint32_t> const faced{ 1U };
  uint32_t const e2{ memo.find_score(drawn, faced, true).entry };
  CHECK(e2 != e);
  memo.set_score(e2, true, { .viable = true, .inflated = true });
  CandidateMemo::Recalled const inflated{ memo.find_score(drawn, faced, false) };
  REQUIRE(inflated.found);
  CHECK(inflated.labelled);
  CHECK(inflated.score.viable);
  CHECK(inflated.score.inflated);
  memo.set_score(e2, false, {});
  REQUIRE(memo.score(e2, false, got));
  CHECK_FALSE(got.viable);
  CHECK(memo.peak_bytes() > 0);
}

TEST_CASE("candidate memo: an ordering key reads the score entry linked to it") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  uint32_t const row{ memo.row_word(f.row) };
  uint32_t const arranged{ memo.arrangement(plain) };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  REQUIRE(arranged != INVALID);
  REQUIRE(drawn != INVALID);
  PodVector<uint32_t> const none;
  CandidateMemo::Linked const fresh{ memo.find_ordering(row, arranged, none) };
  REQUIRE(fresh.key != INVALID);
  CHECK(fresh.entry == INVALID);
  CHECK_FALSE(memo.recall(fresh.entry, true).found);
  uint32_t const e{ memo.find_score(drawn, none, true).entry };
  memo.link(fresh.key, e);
  CandidateMemo::Linked const again{ memo.find_ordering(row, arranged, none) };
  CHECK(again.key == fresh.key);
  CHECK(again.entry == e);
  memo.set_score(
      e,
      true,
      { .cost = { .t0_violations = 1, .t1_hints = 0, .t2 = 3 }, .viable = true });
  CandidateMemo::Recalled const got{ memo.recall(again.entry, true) };
  REQUIRE(got.found);
  CHECK(got.score.cost.t2 == 3);
}

TEST_CASE("candidate memo: one claim per score kind, until that score is set") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  REQUIRE(drawn != INVALID);
  PodVector<uint32_t> const none;
  uint32_t const e{ memo.find_score(drawn, none, false).entry };
  REQUIRE(e != INVALID);
  MemoScore got;
  CHECK(memo.claim(e, false, got) == Claim::Taken);
  CHECK(memo.claim(e, false, got) == Claim::Busy);
  CHECK(memo.claim(e, true, got) == Claim::Taken);  // the other kind is apart
  // A claimed score reads as unset.
  CHECK_FALSE(memo.score(e, false, got));
  CHECK_FALSE(memo.find_score(drawn, none, false).found);
  MemoScore const s{ .cost = { .t0_violations = 0, .t1_hints = 0, .t2 = 9 },
                     .viable = true };
  memo.set_score(e, false, s);
  REQUIRE(memo.claim(e, false, got) == Claim::Scored);
  CHECK(got.cost.t2 == 9);
  // An entry the memo cannot hold, and a retried one, hold no claim.
  CHECK(memo.claim(INVALID, false, got) == Claim::Taken);
  CHECK(memo.claim(INVALID, false, got) == Claim::Taken);
  memo.set_retried(e);
  CHECK(memo.claim(e, true, got) == Claim::Taken);
  CHECK(memo.claim(e, true, got) == Claim::Taken);
}

TEST_CASE("candidate memo: a released claim leaves the score unset and the bound raised") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  REQUIRE(drawn != INVALID);
  PodVector<uint32_t> const none;
  uint32_t const e{ memo.find_score(drawn, none, false).entry };
  REQUIRE(e != INVALID);
  memo.set_route_bound(e, 40);
  MemoScore got;
  REQUIRE(memo.claim(e, false, got) == Claim::Taken);
  memo.release(e, false, 70);
  CHECK(memo.claim(e, false, got) == Claim::Taken);  // claimable again
  memo.release(e, false, 50);                        // a lower bound keeps the higher
  CandidateMemo::Recalled const back{ memo.recall(e, false) };
  CHECK_FALSE(back.found);
  CHECK(back.route_bound == 70);
}

TEST_CASE("candidate memo: a labelled release bounds only labelled requests") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  REQUIRE(drawn != INVALID);
  PodVector<uint32_t> const none;
  uint32_t const e{ memo.find_score(drawn, none, false).entry };
  REQUIRE(e != INVALID);
  MemoScore got;
  REQUIRE(memo.claim(e, true, got) == Claim::Taken);
  memo.release(e, true, 90);
  CHECK(memo.recall(e, false).route_bound == -1);
  CHECK(memo.find_score(drawn, none, false).route_bound == -1);
  CHECK(memo.recall(e, true).route_bound == 90);
  CHECK(memo.find_score(drawn, none, true).route_bound == 90);
  memo.set_route_bound(e, 40);
  CHECK(memo.recall(e, false).route_bound == 40);
  CHECK(memo.recall(e, true).route_bound == 90);
  // An unlabelled release bounds both requests.
  REQUIRE(memo.claim(e, false, got) == Claim::Taken);
  memo.release(e, false, 60);
  CHECK(memo.recall(e, false).route_bound == 60);
  CHECK(memo.recall(e, true).route_bound == 90);
  REQUIRE(memo.claim(e, false, got) == Claim::Taken);
  memo.release(e, false, 120);
  CHECK(memo.recall(e, false).route_bound == 120);
  CHECK(memo.recall(e, true).route_bound == 120);
}

TEST_CASE("candidate memo: a route bound comes back beside a score it does not answer") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  REQUIRE(drawn != INVALID);
  PodVector<uint32_t> const none;
  CandidateMemo::Recalled const fresh{ memo.find_score(drawn, none, true) };
  REQUIRE(fresh.entry != INVALID);
  CHECK(fresh.route_bound == -1);
  int64_t const t2{ (int64_t{ 1 } << 40) + 3 };
  memo.set_route_bound(fresh.entry, t2);
  CandidateMemo::Recalled const again{ memo.find_score(drawn, none, true) };
  CHECK_FALSE(again.found);
  CHECK(again.route_bound == t2);
  CHECK(memo.recall(fresh.entry, false).route_bound == t2);
}

TEST_CASE("candidate memo: a retried entry never answers and takes no score") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  SubmachineOrders const plain{ f.order() };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  REQUIRE(drawn != INVALID);
  PodVector<uint32_t> const none;
  uint32_t const e{ memo.find_score(drawn, none, true).entry };
  REQUIRE(e != INVALID);
  MemoScore const s{ .cost = { .t0_violations = 0, .t1_hints = 0, .t2 = 4 },
                     .viable = true };
  memo.set_score(e, true, s);
  REQUIRE(memo.find_score(drawn, none, true).found);
  memo.set_retried(e);
  CandidateMemo::Recalled const got{ memo.find_score(drawn, none, true) };
  CHECK_FALSE(got.found);
  CHECK(got.entry == INVALID);
  CHECK_FALSE(memo.recall(e, false).found);
  memo.set_score(e, true, s);
  MemoScore read;
  CHECK_FALSE(memo.score(e, true, read));
  CHECK_FALSE(memo.find_score(drawn, none, true).found);
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
  SubmachineOrders const plain{ f.order() };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  REQUIRE(drawn != INVALID);
  PodVector<uint32_t> const none;
  uint32_t const e{ memo.find_score(drawn, none, true).entry };
  REQUIRE(e != INVALID);
  memo.set_score(
      e,
      true,
      { .cost = { .t0_violations = 0, .t1_hints = 0, .t2 = 9 }, .viable = true });
  // Emptied away, the entry reads unset and takes no score.
  MemoScore got;
  CHECK_FALSE(memo.score(e, true, got));
  CHECK_FALSE(memo.recall(e, true).found);
  // The same key comes back under a new number.
  uint32_t const again{ memo.find_score(drawn, none, true).entry };
  REQUIRE(again != INVALID);
  CHECK(again != e);
  CHECK_FALSE(memo.find_score(drawn, none, true).found);
  uint32_t const plain_at{ memo.arrangement(plain) };
  REQUIRE(plain_at != INVALID);
  uint32_t const moved{ memo.arrangement(f.order(f.moved_rank())) };
  REQUIRE(moved != INVALID);
  CHECK(moved != plain_at);
  CHECK(memo.peak_bytes() > 0);
}

TEST_CASE("candidate memo: the tables never hold more than the budget") {
  Fixture const f;
  constexpr uint64_t BUDGET{ uint64_t{ 64 } << 10U };
  CandidateMemo memo{ f.c, f.g, BUDGET };
  uint32_t const row{ memo.row_word(f.row) };
  PodVector<uint32_t> faces;
  uint64_t most{ 0 };
  for (uint32_t i = 0; i < 20000; ++i) {
    faces.assign(1 + (i % 7), i);
    (void)memo.find_score(i, faces, (i % 2) == 0);
    (void)memo.find_ordering(row, i, faces);
    uint64_t const held{ memo.held_bytes() };
    most = (held > most) ? held : most;
    REQUIRE(held <= BUDGET);
  }
  CHECK(most > (BUDGET / 2));
  CHECK(memo.peak_bytes() >= most);
}

TEST_CASE("candidate memo: a link to an entry since emptied away reads nothing") {
  Fixture const f;
  CandidateMemo memo{ f.c, f.g };
  uint32_t const row{ memo.row_word(f.row) };
  SubmachineOrders const plain{ f.order() };
  uint32_t const drawn{ memo.drawing(plain, f.size(plain), memo.profile_word(f.p)) };
  uint32_t const plain_at{ memo.arrangement(plain) };
  REQUIRE(drawn != INVALID);
  REQUIRE(plain_at != INVALID);
  PodVector<uint32_t> const none;
  uint32_t const old{ memo.find_score(drawn, none, true).entry };
  REQUIRE(old != INVALID);
  memo.empty();
  // The same key takes a new entry at the old one's shard and index, and a score.
  uint32_t const now{ memo.find_score(drawn, none, true).entry };
  REQUIRE(now != INVALID);
  CHECK(now != old);
  memo.set_score(
      now,
      true,
      { .cost = { .t0_violations = 0, .t1_hints = 0, .t2 = 9 }, .viable = true });
  REQUIRE(memo.recall(now, true).found);
  uint32_t const key{ memo.find_ordering(row, plain_at, none).key };
  REQUIRE(key != INVALID);
  memo.link(key, old);
  CandidateMemo::Linked const linked{ memo.find_ordering(row, plain_at, none) };
  CHECK(linked.entry == old);
  CHECK_FALSE(memo.recall(linked.entry, true).found);
}
