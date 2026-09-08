// Section 6's thread axis: every geometry column byte for byte and all three
// hashes, at every worker count the matrix names and under the delay injector.

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

// The shim's own hook, declared here rather than in a header: a no-op on the
// null backend, and it must be set before any worker exists.
namespace scav {
void thread_test_delay_seed(uint64_t seed);
// The portfolio's row count, which `layout.cpp` brackets with SCAV_INTERNAL.
uint32_t search_tuple_count(scav_profile const &p, uint32_t entity_count);
}  // namespace scav

namespace {

using namespace scav;

// The matrix's counts less 1, which is what every run below is compared to.
constexpr std::array<uint32_t, 6> THREADS{ 2, 3, 5, 8, 13, 16 };

constexpr std::array<char const *, 11> CORPUS{
  "axis.scav", "bottler.scav", "brew.scav", "dock.scav",        "estop.scav", "led.scav",
  "mill.scav", "ota.scav",     "tcp.scav",  "toolchanger.scav", "vac.scav"
};

// Every geometry column, `scav.geom.gen` included: each run below is on a
// fresh chart, so its run count is one like every other's.
constexpr std::array<char const *, 11> GEOM{
  "scav.geom.state",  "scav.geom.state_before", "scav.geom.state_after",
  "scav.geom.sub",    "scav.geom.route",        "scav.geom.port",
  "scav.geom.point",  "scav.geom.portslot",     "scav.geom.chart",
  "scav.geom.inputs", "scav.geom.gen"
};

// Restores the injector however a case leaves it, failed assertion included.
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

// Reports which column moved rather than only that something did.
void check_same(Snapshot const &got, Snapshot const &want) {
  // The portfolio's own answer, beside the geometry it chose: a worker count
  // that moved the pick would move every column under it, and this says which
  // of the two happened.
  CHECK(got.tuple == want.tuple);
  for (uint32_t i = 0; i < HASHES.size(); ++i) {
    // A `std::string`, because this doctest prints a `char const *` as the
    // pointer and the point of the capture is which one moved.
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

// A fresh load per run, so no thread count reads columns another wrote:
// `layout_run` rewrites them in place.
void check_corpus_chart(std::string const &name, scav_profile const &p) {
  CAPTURE(name);
  Chart first;
  load_corpus(name.c_str(), first);
  Snapshot const want{ lay_out(first, p, 1) };
  for (uint32_t const threads : THREADS) {
    CAPTURE(threads);
    Chart c;
    load_corpus(name.c_str(), c);
    check_same(lay_out(c, p, threads), want);
  }
}

// Shards with a frame to work on: the count comes from every entity, the
// ranges are cut over submachines, so a one-frame chart busies one shard.
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
  // Asserted rather than assumed, so neither case can quietly cover the other's
  // path: the nested chart runs frames concurrently, the flat one has one frame
  // and every other shard empty.
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
  for (uint32_t const threads : THREADS) {
    CAPTURE(threads);
    Chart c{ build() };
    check_same(lay_out(c, p, threads), want);
  }
}

// Three states nested one inside the next, and a self-loop on the innermost.
// Every frame holds exactly one node in exactly one rank, so every packing in
// the chart -- components, fold pieces, sibling submachines -- has one rect to
// place and the box packer cannot produce a different one. A self-loop
// contributes no ordering edge (11.3), so it gives the chart a route to score
// without giving a frame a second node.
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
  scav_layout_opts const o{ .profile = readable(), .router = 0, .threads = threads };
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
  scav_profile const p{ readable() };
  // Four chart-global candidates on every corpus chart, since none of them
  // reaches the 1,024 entities the scaling rule starts halving at: the pick is
  // a real choice here rather than the one row a 2k shape runs.
  REQUIRE(p.portfolio_m == 4);
  std::string shards;
  for (char const *name : CORPUS) {
    Chart sized;
    load_corpus(name, sized);
    REQUIRE(search_tuple_count(p, layout_entity_count(sized)) == 4);
    check_corpus_chart(name, p);

    Chart c;
    load_corpus(name, c);
    shards += name;
    shards += ' ';
    shards += std::to_string(layout_shard_count(c));
    shards += ' ';
  }
  // Most of the corpus is under 64 entities and shards to one, so the corpus
  // alone does not reach the multi-shard path; the scale targets below do.
  MESSAGE("corpus shard counts: ", shards);
}

TEST_CASE("determinism: the scale targets lay out to one answer at every thread count") {
  scav_profile const p{ readable() };
  SUBCASE("nested") { check_scale_chart("nested 2k", nested_2k_chart, p, true); }
  SUBCASE("flat") { check_scale_chart("flat 2k", flat_2k_chart, p, false); }
}

TEST_CASE("determinism: the scheduling-delay injector moves nothing at scale") {
  DelayGuard const guard;
  scav_profile const p{ readable() };
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

TEST_CASE("determinism: the flat target survives the injector too") {
  DelayGuard const guard;
  scav_profile const p{ readable() };
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

TEST_CASE("determinism: an inflating retry re-enters the sharded phases the same way") {
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
  // One component per frame, so both packers place one rect and every row of
  // the table produces the same geometry. `cost_less` is strict, so a tie is
  // the lower index -- and a reduction that folded into a shared best-so-far
  // would be free to answer differently per worker count.
  scav_profile const p{ readable() };
  Chart reference{ tied_chart() };
  Snapshot const want{ lay_out(reference, p, 1) };
  CHECK(want.tuple == 0);

  // What makes it a tie rather than row 0 simply winning: the flipped row on
  // its own writes the same bytes.
  scav_profile flipped{ p };
  flipped.portfolio_m = 1;
  flipped.trybox = (p.trybox != 0) ? 0 : 1;
  Chart alone{ tied_chart() };
  Snapshot const other{ lay_out(alone, flipped, 1) };
  CHECK(other.tuple == 0);
  for (uint32_t i = 0; i < GEOM.size(); ++i) {
    // The inputs digest hears the flipped knob; nothing else does.
    if (std::string{ GEOM[i] } != "scav.geom.inputs") {
      CHECK(other.columns[i] == want.columns[i]);
    }
  }
  CHECK(other.hashes[2] == want.hashes[2]);

  for (uint32_t const threads : THREADS) {
    CAPTURE(threads);
    Chart c{ tied_chart() };
    check_same(lay_out(c, p, threads), want);
  }
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
