// Router bench: every registered router over the corpus and the scale targets, scored
// into a golden, timed, and with route ends checked against `straight`'s.

#include "core/tests/corpus.h"
#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/router.h"
#include "layout/size.h"
#include "layout/tests/test_synth.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace scav;

constexpr std::array<char const *, 14> CORPUS{
  "axis.scav",    "bottler.scav", "brew.scav",        "dock.scav", "elevator.scav",
  "estop.scav",   "kiln.scav",    "led.scav",         "mill.scav", "ota.scav",
  "printer.scav", "tcp.scav",     "toolchanger.scav", "vac.scav"
};

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

scav_profile compact() {
  scav_profile p{};
  REQUIRE(profile_named("compact", p));
  return p;
}

// Every chart in test_data/charts/gauntlet by bare file name; test_gauntlet.py checks
// this array and gauntlet_tests.cpp's against the directory.
constexpr std::array<char const *, 35> GAUNTLET{
  "above.scav",   "carried.scav",   "chain.scav",   "corner.scav",    "crossing.scav",
  "crowd.scav",   "enclosing.scav", "entered.scav", "fanin.scav",     "folded.scav",
  "fork.scav",    "header.scav",    "inloop.scav",  "inward.scav",    "lane.scav",
  "level.scav",   "long.scav",      "loop.scav",    "marks.scav",     "mixed.scav",
  "mutual.scav",  "ported.scav",    "pulled.scav",  "regions.scav",   "room.scav",
  "rooms.scav",   "roundtrip.scav", "seated.scav",  "separator.scav", "stretch.scav",
  "through.scav", "tight.scav",     "transit.scav", "under.scav",     "unfolded.scav"
};

std::string router_label(uint32_t index) {
  scav_byte const *bytes{ nullptr };
  uint32_t len{ 0 };
  REQUIRE(router_name(index, bytes, len));
  return std::string{ reinterpret_cast<char const *>(bytes), len };
}

// `rel` is relative to test_data/charts, e.g. "gauntlet/loop.scav".
void load_chart(std::string const &rel, Chart &c) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += rel;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
}

// True when `at` lies on `r`'s border.
bool on_border(scav_point at, scav_rect const &r) {
  bool const on_side{ ((at.x == r.x) || (at.x == (r.x + r.w))) && (at.y >= r.y) &&
                      (at.y <= (r.y + r.h)) };
  bool const on_cap{ ((at.y == r.y) || (at.y == (r.y + r.h))) && (at.x >= r.x) &&
                     (at.x <= (r.x + r.w)) };
  return on_side || on_cap;
}

int64_t timed_run(Chart &c, scav_layout_opts const &o, bool &laid) {
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  auto const t0{ std::chrono::steady_clock::now() };
  laid = layout_run(c, {}, o, placed, diags);
  auto const t1{ std::chrono::steady_clock::now() };
  return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

}  // namespace

TEST_CASE("bench: every registered router scores the corpus, term by term") {
  // Readable, no space requests, phases called directly: every router routes the same
  // candidate.
  scav_profile const p{ readable() };
  scav_router_id reference{ 0 };
  REQUIRE(router_by_name(reinterpret_cast<scav_byte const *>("straight"), 8, reference));

  std::vector<std::string> rows(router_count());
  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Chart c;
    load_chart(name, c);

    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ order_submachines(c, g, {}, p) };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, o, {}, p, z, diags));
    Routes const ref{ route_transitions(c, g, o, z, {}, p, *router_at(reference)) };

    for (uint32_t ri = 0; ri < router_count(); ++ri) {
      std::string const label{ router_label(ri) };
      CAPTURE(label);
      Routes const r{ route_transitions(c, g, o, z, {}, p, *router_at(ri)) };
      CostTerms const t{ cost_terms(c, g, z, r, {}, p) };
      Cost const scored{ cost_of(t, p) };

      std::string &row{ rows[ri] };
      row += label;
      row += ' ';
      row += name;
      for (int64_t const term : { int64_t{ scored.t0_violations },
                                  t.bends,
                                  t.corridor,
                                  t.crossings,
                                  t.excess_len,
                                  t.adjacency,
                                  t.label,
                                  t.label_near,
                                  t.aspect,
                                  t.area,
                                  scored.t2,
                                  int64_t{ r.degraded() },
                                  int64_t{ r.reseated } }) {
        row += ' ';
        row += std::to_string(term);
      }
      row += '\n';

      // Each route end matches `straight`'s or lies on its end state's border.
      REQUIRE(r.route.size() == ref.route.size());
      for (uint32_t edge = 0; edge < r.route.size(); ++edge) {
        CAPTURE(edge);
        scav_span const want{ ref.route[edge] };
        scav_span const got{ r.route[edge] };
        if (want.len == 0) {
          CHECK(got.len == 0);
          continue;
        }
        REQUIRE(got.len >= 2);
        Transition const &tr{ c.transitions[edge] };
        scav_point const head{ r.points[got.off] };
        scav_point const tail{ r.points[got.off + got.len - 1] };
        CHECK((same(head, ref.points[want.off]) || on_border(head, z.state[tr.src.v])));
        CHECK((same(tail, ref.points[want.off + want.len - 1]) ||
               on_border(tail, z.state[tr.dst.v])));
      }
    }
  }

  std::string actual;
  for (std::string const &row : rows) { actual += row; }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/layout/corpus_routers.txt", golden));
  std::string const want{ scav::test::corpus_golden(
      { reinterpret_cast<char const *>(golden.data()), golden.size() }) };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_routers.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_routers.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("bench: every registered router is timed over the corpus and at scale" *
          doctest::test_suite("full")) {
  // One layout per chart: Level 1 off and one portfolio row.
  scav_profile p{ readable() };
  p.portfolio_k = 0;
  p.portfolio_m = 1;
  Chart nested{ nested_2k_chart() };
  Chart flat{ flat_2k_chart() };
  REQUIRE(nested.states.size() >= 2000);
  REQUIRE(nested.transitions.size() >= 3500);
  REQUIRE(flat.transitions.size() >= 2000);

  for (uint32_t ri = 0; ri < router_count(); ++ri) {
    std::string const label{ router_label(ri) };
    scav_layout_opts const o{ .profile = p, .router = ri, .threads = 0 };

    int64_t corpus_us{ 0 };
    for (char const *name : CORPUS) {
      if (scav::test::corpus_skipped(name)) { continue; }
      CAPTURE(name);
      Chart c;
      load_chart(name, c);
      bool laid{ false };
      corpus_us += timed_run(c, o, laid);
      CHECK_MESSAGE(laid, label);
    }
    bool nested_laid{ false };
    bool flat_laid{ false };
    int64_t const nested_us{ timed_run(nested, o, nested_laid) };
    int64_t const flat_us{ timed_run(flat, o, flat_laid) };
    MESSAGE("router ",
            label,
            ": corpus ",
            corpus_us,
            " us, nested 2k ",
            nested_us,
            " us, flat 2k ",
            flat_us,
            " us, laid out: ",
            nested_laid,
            " ",
            flat_laid);
#if SCAV_PERF_ASSERT_FLOOR == 1
    // Upper bounds that catch a quadratic router.
    CHECK_MESSAGE(nested_us < 200000, label, " nested 2k");
    CHECK_MESSAGE(flat_us < 500000, label, " flat 2k");
#endif
  }
}

TEST_CASE("bench: the cells corpus_routers.txt leaves unscored, term by term") {
  // Raw cost terms, unweighted, for the corpus at `compact` and the element suite at
  // both profiles; phases called directly on one fixed candidate.
  std::string actual;
  auto const row = [&actual](char const *profile,
                             scav_profile const &p,
                             uint32_t ri,
                             std::string const &dir,
                             char const *name) {
    CAPTURE(profile);
    CAPTURE(name);
    Chart c;
    load_chart(dir + name, c);
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ order_submachines(c, g, {}, p) };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, o, {}, p, z, diags));
    Routes const r{ route_transitions(c, g, o, z, {}, p, *router_at(ri)) };
    CostTerms const t{ cost_terms(c, g, z, r, {}, p) };

    actual += profile;
    actual += ' ';
    actual += router_label(ri);
    actual += ' ';
    actual += dir;
    actual += name;
    for (int64_t const term : { int64_t{ t.through_box } + t.box_overlap,
                                t.bends,
                                t.corridor,
                                t.crossings,
                                t.excess_len,
                                t.adjacency,
                                t.label,
                                t.label_near,
                                t.aspect,
                                t.area,
                                t.crowding,
                                t.length,
                                t.transit_bends,
                                t.whitespace }) {
      actual += ' ';
      actual += std::to_string(term);
    }
    actual += '\n';
  };

  for (uint32_t ri = 0; ri < router_count(); ++ri) {
    for (char const *name : CORPUS) {
      if (scav::test::corpus_skipped(name)) { continue; }
      row("compact", compact(), ri, "", name);
    }
  }
  for (char const *profile : { "readable", "compact" }) {
    scav_profile p{};
    REQUIRE(profile_named(profile, p));
    for (uint32_t ri = 0; ri < router_count(); ++ri) {
      for (char const *name : GAUNTLET) { row(profile, p, ri, "gauntlet/", name); }
    }
  }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/layout/cost_terms.txt", golden));
  std::string const want{ scav::test::corpus_golden(
      { reinterpret_cast<char const *>(golden.data()), golden.size() }) };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/cost_terms.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/cost_terms.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("bench: the scorer is timed over the corpus and at scale") {
  // Times `cost_terms` alone, with routing outside the clock.
  scav_profile const p{ readable() };
  auto const scored_us = [&p](Chart const &c, uint32_t times) {
    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ order_submachines(c, g, {}, p) };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(size_layout(c, g, o, {}, p, z, diags));
    Routes const r{ route_transitions(c, g, o, z, {}, p, *router_at(0)) };
    auto const t0{ std::chrono::steady_clock::now() };
    for (uint32_t i = 0; i < times; ++i) { (void)cost_terms(c, g, z, r, {}, p); }
    auto const t1{ std::chrono::steady_clock::now() };
    return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  };

  int64_t corpus_us{ 0 };
  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Chart c;
    load_chart(name, c);
    corpus_us += scored_us(c, 10);
  }
  int64_t const nested_us{ scored_us(nested_2k_chart(), 1) };
  int64_t const flat_us{ scored_us(flat_2k_chart(), 1) };
  MESSAGE("cost_terms: corpus x10 ",
          corpus_us,
          " us, nested 2k ",
          nested_us,
          " us, flat 2k ",
          flat_us,
          " us");
#if SCAV_PERF_ASSERT_FLOOR == 1
  // Upper bounds that catch a per-piece sweep over every state.
  CHECK(nested_us < 20000);
  CHECK(flat_us < 20000);
#endif
}
