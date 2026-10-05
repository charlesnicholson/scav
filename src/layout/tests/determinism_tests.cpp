// Thread determinism: every geometry column byte for byte and all three hashes, serial
// against the pool and under the delay injector.

#include "core/tests/corpus.h"
#include "layout/shard.h"
#include "layout/tests/test_synth.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_shard.h"

#include "doctest.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace scav {
// Seeds the thread shim's delay injector, 0 to disable; a no-op on the null backend.
// Set it before any worker exists.
void thread_test_delay_seed(uint64_t seed);
// The portfolio's row count; SCAV_INTERNAL in `layout.cpp`.
uint32_t search_tuple_count(scav_profile const &p);
}  // namespace scav

namespace {

using namespace scav;

constexpr std::array<char const *, 4> CHARTS{ "brew.scav",
                                              "dock.scav",
                                              "tcp.scav",
                                              "vac.scav" };

// Every geometry column, `scav.geom.gen` included; each run below uses a fresh chart.
constexpr std::array<char const *, 15> GEOM{ "scav.geom.state",
                                             "scav.geom.state_before",
                                             "scav.geom.state_after",
                                             "scav.geom.state_lead",
                                             "scav.geom.state_trail",
                                             "scav.geom.state_loop",
                                             "scav.geom.state_loop_place",
                                             "scav.geom.sub",
                                             "scav.geom.route",
                                             "scav.geom.port",
                                             "scav.geom.point",
                                             "scav.geom.portslot",
                                             "scav.geom.chart",
                                             "scav.geom.inputs",
                                             "scav.geom.gen" };

// Disables the delay injector on scope exit.
struct DelayGuard {
  DelayGuard() = default;
  DelayGuard(DelayGuard const &) = delete;
  DelayGuard &operator=(DelayGuard const &) = delete;
  ~DelayGuard() { thread_test_delay_seed(0); }
};

constexpr std::array<char const *, 3> HASHES{ "inputs", "structural", "coordinate" };

// One run's whole output: the three hashes, the portfolio row that produced
// them, then the raw bytes of every geometry column.
struct Snapshot {
  std::array<uint32_t, HASHES.size()> hashes{};
  uint32_t tuple{ INVALID };
  std::array<std::vector<scav_byte>, GEOM.size()> columns;
};

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

// Readable with Level 1 off and one portfolio row, for the 2k shapes.
scav_profile scale_readable() {
  scav_profile p{ readable() };
  p.portfolio_k = 0;
  p.portfolio_m = 1;
  return p;
}

void load_corpus(char const *name, Chart &c) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
}

Snapshot lay_out(Chart &c,
                 scav_profile const &p,
                 uint32_t threads,
                 uint32_t *inflations = nullptr) {
  scav_layout_opts const o{ .profile = p, .router = 0, .threads = threads };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  Snapshot out;
  REQUIRE(layout_run(c, {}, o, placed, diags, inflations, &out.tuple));
  CHECK(diags.empty());

  out.hashes = { layout_inputs_digest(c),
                 layout_structural_hash(c),
                 layout_coordinate_hash(c) };
  for (uint32_t i = 0; i < GEOM.size(); ++i) {
    ColumnId const id{ column_find(c, GEOM[i]) };
    REQUIRE(id.v != INVALID);
    size_t const bytes{ static_cast<size_t>(column_count(c, id)) *
                        c.columns[id.v].desc.elem_size };
    scav_byte const *const at{ column_data(c, id) };
    if (bytes != 0) { out.columns[i].assign(at, at + bytes); }
  }
  return out;
}

// Compares the tuple, each hash and each column; a failure names the one that differs.
void check_same(Snapshot const &got, Snapshot const &want) {
  CHECK(got.tuple == want.tuple);
  for (uint32_t i = 0; i < HASHES.size(); ++i) {
    // CAPTURE prints a `char const *` as a pointer.
    std::string const hash{ HASHES[i] };
    CAPTURE(hash);
    CHECK(got.hashes[i] == want.hashes[i]);
  }
  for (uint32_t i = 0; i < GEOM.size(); ++i) {
    std::string const column{ GEOM[i] };
    CAPTURE(column);
    CHECK(got.columns[i] == want.columns[i]);
  }
}

// Lays out one fresh load serially and another on the pool, and compares them.
void check_corpus_chart(std::string const &name, scav_profile const &p) {
  CAPTURE(name);
  Chart first;
  load_corpus(name.c_str(), first);
  Snapshot const want{ lay_out(first, p, 1) };
  Chart c;
  load_corpus(name.c_str(), c);
  check_same(lay_out(c, p, 0), want);
}

// Shards whose submachine range is nonempty; a one-frame chart busies one shard.
uint32_t busy_shards(Chart const &c) {
  uint32_t const shards{ layout_shard_count(c) };
  uint32_t const subs{ static_cast<uint32_t>(c.submachines.size()) };
  uint32_t busy{ 0 };
  for (uint32_t s = 0; s < shards; ++s) {
    busy += (shard_range(s, shards, subs).len > 0) ? 1U : 0U;
  }
  return busy;
}

void check_scale_chart(std::string const &name,
                       Chart (*build)(),
                       scav_profile const &p,
                       bool concurrent_frames) {
  CAPTURE(name);
  Chart first{ build() };
  // The nested chart busies more than one shard; the flat chart busies exactly one.
  uint32_t const shards{ layout_shard_count(first) };
  uint32_t const busy{ busy_shards(first) };
  REQUIRE(shards > 1);
  if (concurrent_frames) {
    REQUIRE(busy > 1);
  } else {
    REQUIRE(busy == 1);
  }
  MESSAGE(name, " shards: ", shards, ", busy: ", busy);
  Snapshot const want{ lay_out(first, p, 1) };
  Chart c{ build() };
  check_same(lay_out(c, p, 0), want);
}

// Three states nested one inside the next, with a self-loop on the innermost: every
// packing places one rect, and the self-loop adds a route but no ordering edge.
Chart tied_chart() {
  Chart c;
  SubmachineId parent{ build_chart(c, "tied", {}) };
  StateId at{ INVALID };
  for (uint32_t d = 0; d < 3; ++d) {
    at = build_state(c, parent, "S", StateKind::Normal, {});
    parent = build_submachine(c, at, {}, {});
  }
  build_trans(c, at, at, TransKind::External, {});
  return c;
}

int64_t timed(Chart &c, uint32_t threads) {
  scav_layout_opts const o{ .profile = scale_readable(), .router = 0, .threads = threads };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  auto const t0{ std::chrono::steady_clock::now() };
  bool const laid{ layout_run(c, {}, o, placed, diags) };
  auto const t1{ std::chrono::steady_clock::now() };
  CHECK(laid);
  return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

}  // namespace

TEST_CASE("determinism: the corpus lays out to one answer at every thread count") {
  // Four Level 2 rows: both box packers, each with and without compaction.
  scav_profile p{ readable() };
  p.portfolio_m = 4;
  // A move budget of 24, below the shipped 1024; the next case runs the shipped depth.
  p.portfolio_k = 24;
  REQUIRE(profile_validate(p));
  REQUIRE(search_tuple_count(p) == 4);
  std::string shards;
  for (char const *name : CHARTS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    check_corpus_chart(name, p);

    Chart c;
    load_corpus(name, c);
    shards += name;
    shards += ' ';
    shards += std::to_string(layout_shard_count(c));
    shards += ' ';
  }
  // Reports each chart's shard count; the scale targets below cover multiple shards.
  MESSAGE("corpus shard counts: ", shards);
}

TEST_CASE("determinism: a searched drawing is the same drawing at every thread count" *
          doctest::test_suite("full")) {
  // The shipped search depth on small charts: parallel candidates, rows, finishes and
  // kicks reduce in enumeration order at every thread count.
  scav_profile const p{ readable() };
  for (char const *name :
       { "estop.scav", "kiln.scav", "led.scav", "dock.scav", "brew.scav" }) {
    CAPTURE(name);
    Chart first;
    load_corpus(name, first);
    Snapshot const want{ lay_out(first, p, 1) };
    for (uint32_t const threads : { 2U, 8U, 16U }) {
      CAPTURE(threads);
      Chart c;
      load_corpus(name, c);
      check_same(lay_out(c, p, threads), want);
    }
  }
}

TEST_CASE("determinism: the scale targets lay out to one answer at every thread count" *
          doctest::test_suite("full")) {
  scav_profile const p{ scale_readable() };
  SUBCASE("nested") { check_scale_chart("nested 2k", nested_2k_chart, p, true); }
  SUBCASE("flat") { check_scale_chart("flat 2k", flat_2k_chart, p, false); }
}

TEST_CASE("determinism: the scheduling-delay injector moves nothing at scale" *
          doctest::test_suite("full")) {
  DelayGuard const guard;
  scav_profile const p{ scale_readable() };
  Chart reference{ nested_2k_chart() };
  Snapshot const want{ lay_out(reference, p, 1) };

  for (uint64_t const seed : { UINT64_C(1), UINT64_C(2), UINT64_C(0xDEAD'BEEF) }) {
    for (uint32_t const threads : { 5U, 13U }) {
      CAPTURE(seed);
      CAPTURE(threads);
      thread_test_delay_seed(seed);
      Chart c{ nested_2k_chart() };
      check_same(lay_out(c, p, threads), want);
    }
  }
}

TEST_CASE("determinism: the flat target survives the injector too" *
          doctest::test_suite("full")) {
  DelayGuard const guard;
  scav_profile const p{ scale_readable() };
  Chart reference{ flat_2k_chart() };
  Snapshot const want{ lay_out(reference, p, 1) };

  for (uint64_t const seed : { UINT64_C(3), UINT64_C(7), UINT64_C(0xC0FF'EE01) }) {
    for (uint32_t const threads : { 5U, 13U }) {
      CAPTURE(seed);
      CAPTURE(threads);
      thread_test_delay_seed(seed);
      Chart c{ flat_2k_chart() };
      check_same(lay_out(c, p, threads), want);
    }
  }
}

// Skipped: `sealed_chart` routes at its drawn size, and no known fixture makes the
// spacing retry run.
TEST_CASE("determinism: an inflating retry re-enters the sharded phases the same way" *
          doctest::skip()) {
  scav_profile const p{ sealed_profile(readable()) };
  Chart one{ sealed_chart() };
  uint32_t serial{ 0 };
  Snapshot const want{ lay_out(one, p, 1, &serial) };
  CHECK(serial == 3);

  Chart eight{ sealed_chart() };
  uint32_t threaded{ 0 };
  check_same(lay_out(eight, p, 8, &threaded), want);
  CHECK(threaded == serial);
}

TEST_CASE("determinism: two tuples that tie keep the lower row, at every count") {
  // One component per frame: every table row yields the same geometry, and the strict
  // `cost_less` keeps the lower row at every worker count.
  scav_profile const p{ readable() };
  Chart reference{ tied_chart() };
  Snapshot const want{ lay_out(reference, p, 1) };
  CHECK(want.tuple == 0);

  // The flipped row alone writes the same geometry: the rows tie.
  scav_profile flipped{ p };
  flipped.portfolio_m = 1;
  flipped.trybox = (p.trybox != 0) ? 0 : 1;
  Chart alone{ tied_chart() };
  Snapshot const other{ lay_out(alone, flipped, 1) };
  CHECK(other.tuple == 0);
  for (uint32_t i = 0; i < GEOM.size(); ++i) {
    // Only the inputs column records the flipped knob.
    if (std::string{ GEOM[i] } != "scav.geom.inputs") {
      CHECK(other.columns[i] == want.columns[i]);
    }
  }
  CHECK(other.hashes[2] == want.hashes[2]);

  Chart c{ tied_chart() };
  check_same(lay_out(c, p, 0), want);
}

TEST_CASE("determinism: the scale target is timed at one thread and at eight") {
  // Informational only.
  Chart one{ nested_2k_chart() };
  Chart eight{ nested_2k_chart() };
  MESSAGE("nested 2k layout_run: threads=1 ",
          timed(one, 1),
          " us, threads=8 ",
          timed(eight, 8),
          " us");
}
