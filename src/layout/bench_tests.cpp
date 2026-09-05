// The router bench: every registered router over the corpus and the scale
// targets, scored into a golden, timed, and joined against `straight`.

#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/router.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace scav;

char const *const CORPUS[]{ "axis.scav",  "bottler.scav",     "brew.scav", "dock.scav",
                            "estop.scav", "led.scav",         "mill.scav", "ota.scav",
                            "tcp.scav",   "toolchanger.scav", "vac.scav" };

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

std::string router_label(uint32_t index) {
  scav_byte const *bytes{ nullptr };
  uint32_t len{ 0 };
  REQUIRE(router_name(index, bytes, len));
  return std::string{ reinterpret_cast<char const *>(bytes), len };
}

void load_corpus(char const *name, Chart &c) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
}

// Where a router may move an end that names a box: on a side within the cap
// span, or on a cap within the side span.
bool on_border(scav_point at, scav_rect const &r) {
  bool const on_side{ ((at.x == r.x) || (at.x == (r.x + r.w))) && (at.y >= r.y) &&
                      (at.y <= (r.y + r.h)) };
  bool const on_cap{ ((at.y == r.y) || (at.y == (r.y + r.h))) && (at.x >= r.x) &&
                     (at.x <= (r.x + r.w)) };
  return on_side || on_cap;
}

// The scale target: depth 16, ~2k states, ~3.7k transitions including one long
// hierarchical edge per level.
Chart nested_2k() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  for (uint32_t r = 0; r < 8; ++r) {
    SubmachineId parent{ root };
    StateId last{ INVALID };
    for (uint32_t d = 0; d < 16; ++d) {
      std::vector<StateId> level;
      level.reserve(15);
      for (uint32_t k = 0; k < 15; ++k) {
        level.push_back(build_state(c, parent, {}, StateKind::Normal, {}));
      }
      StateId const comp{ build_state(c, parent, {}, StateKind::Normal, {}) };
      for (uint32_t k = 1; k < level.size(); ++k) {
        build_trans(c, level[k - 1], level[k], TransKind::External, {});
        build_trans(c, level[k], comp, TransKind::External, {});
      }
      if (last.v != INVALID) { build_trans(c, comp, last, TransKind::External, {}); }
      last = comp;
      parent = build_submachine(c, comp, {}, {});
    }
  }
  return c;
}

// The same count in one frame, which is the largest single routing graph any
// chart produces.
Chart flat_2k() {
  Chart c;
  SubmachineId const root{ build_chart(c, "flat", {}) };
  std::vector<StateId> all;
  all.reserve(2048);
  for (uint32_t i = 0; i < 2048; ++i) {
    all.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < all.size(); ++i) {
    build_trans(c, all[i - 1], all[i], TransKind::External, {});
    if ((i % 16) == 0) { build_trans(c, all[i], all[i - 16], TransKind::External, {}); }
  }
  return c;
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
  // No space requests and the readable profile, so a row is comparable with the
  // single-router cost golden and with whatever registers next.
  scav_profile const p{ readable() };
  scav_router_id reference{ 0 };
  REQUIRE(router_by_name(reinterpret_cast<scav_byte const *>("straight"), 8, reference));

  std::vector<std::string> rows(router_count());
  for (char const *name : CORPUS) {
    CAPTURE(name);
    Chart c;
    load_corpus(name, c);

    SplitGraph const g{ decompose(c) };
    SubmachineOrders const o{ phase1_order(c, g, {}, p) };
    SizedLayout z;
    std::vector<Diagnostic> diags;
    REQUIRE(phase2_size(c, g, o, {}, p, z, diags));
    Routes const ref{ phase3_route(c, g, o, z, {}, p, *router_at(reference)) };

    for (uint32_t ri = 0; ri < router_count(); ++ri) {
      std::string const label{ router_label(ri) };
      CAPTURE(label);
      Routes const r{ phase3_route(c, g, o, z, {}, p, *router_at(ri)) };
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
                                  t.aspect,
                                  t.area,
                                  scored.t2,
                                  int64_t{ r.degraded() },
                                  int64_t{ r.reseated } }) {
        row += ' ';
        row += std::to_string(term);
      }
      row += '\n';

      // `straight` takes both ends where phase 3 put them, so its polyline is
      // where every other router's has to begin and end.
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
  std::string const want{ reinterpret_cast<char const *>(golden.data()), golden.size() };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_routers.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_routers.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("bench: every registered router is timed over the corpus and at scale") {
  scav_profile const p{ readable() };
  Chart nested{ nested_2k() };
  Chart flat{ flat_2k() };
  REQUIRE(nested.states.size() >= 2000);
  REQUIRE(nested.transitions.size() >= 3500);
  REQUIRE(flat.transitions.size() >= 2000);

  for (uint32_t ri = 0; ri < router_count(); ++ri) {
    std::string const label{ router_label(ri) };
    scav_layout_opts const o{ .profile = p, .router = ri, .threads = 0 };

    int64_t corpus_us{ 0 };
    for (char const *name : CORPUS) {
      CAPTURE(name);
      Chart c;
      load_corpus(name, c);
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
    // Floors, not times, and the same two the single-router cases assert: a
    // router that arrives quadratic is caught rather than measured.
    CHECK_MESSAGE(nested_us < 200000, label, " nested 2k");
    CHECK_MESSAGE(flat_us < 500000, label, " flat 2k");
#endif
  }
}
