// Section 6's thread axis: every geometry column byte for byte and all three
// hashes, at every worker count the matrix names and under the delay injector.

#include "layout/shard.h"
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

// The shim's own hook, declared here rather than in a header: a no-op on the
// null backend, and it must be set before any worker exists.
namespace scav { void thread_test_delay_seed(uint64_t seed); }

namespace {

using namespace scav;

// The matrix's counts less 1, which is what every run below is compared to.
constexpr std::array<uint32_t, 6> THREADS{ 2, 3, 5, 8, 13, 16 };

constexpr std::array<char const *, 11> CORPUS{
  "axis.scav", "bottler.scav", "brew.scav", "dock.scav",        "estop.scav", "led.scav",
  "mill.scav", "ota.scav",     "tcp.scav",  "toolchanger.scav", "vac.scav"
};

// Every geometry column but `scav.geom.gen`, which counts runs rather than
// describing one.
constexpr std::array<char const *, 10> GEOM{
  "scav.geom.state", "scav.geom.state_before", "scav.geom.state_after",
  "scav.geom.sub",   "scav.geom.route",        "scav.geom.port",
  "scav.geom.point", "scav.geom.portslot",     "scav.geom.chart",
  "scav.geom.inputs"
};

// Restores the injector however a case leaves it, failed assertion included.
struct DelayGuard {
  DelayGuard() = default;
  DelayGuard(DelayGuard const &) = delete;
  DelayGuard &operator=(DelayGuard const &) = delete;
  ~DelayGuard() { thread_test_delay_seed(0); }
};

constexpr std::array<char const *, 3> HASHES{ "inputs", "structural", "coordinate" };

// One run's whole output: the three hashes, then the raw bytes of every
// geometry column.
struct Snapshot {
  std::array<uint32_t, HASHES.size()> hashes{};
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
  REQUIRE(layout_run(c, {}, o, placed, diags, inflations));
  CHECK(diags.empty());

  Snapshot out;
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

void check_scale_chart(std::string const &name, Chart (*build)(), scav_profile const &p) {
  CAPTURE(name);
  Chart first{ build() };
  // Asserted rather than assumed: without it the case would quietly degrade to
  // the single-shard path the corpus already covers.
  uint32_t const shards{ layout_shard_count(first) };
  REQUIRE(shards > 1);
  MESSAGE(name, " shards: ", shards);
  Snapshot const want{ lay_out(first, p, 1) };
  for (uint32_t const threads : THREADS) {
    CAPTURE(threads);
    Chart c{ build() };
    check_same(lay_out(c, p, threads), want);
  }
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
  std::string shards;
  for (char const *name : CORPUS) {
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
  SUBCASE("nested") { check_scale_chart("nested 2k", nested_2k_chart, p); }
  SUBCASE("flat") { check_scale_chart("flat 2k", flat_2k_chart, p); }
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
