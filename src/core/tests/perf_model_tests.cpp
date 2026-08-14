// Lowering and validation floors, never times: the job is catching an
// accidental O(n^2) -- a span rebuild per wildcard, a per-append fix-up walk
// -- so the load-bearing assertion is the machine-independent scaling one.

#include "core/tests/test_support.h"
#include "core/tests/test_synth.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

constexpr uint64_t INPUT_BYTES{ SCAV_PERF_INPUT_BYTES };
constexpr bool ASSERT_FLOOR{ SCAV_PERF_ASSERT_FLOOR != 0 };

// An order of magnitude under a 2020-era laptop, like the front end's floors.
constexpr uint64_t LOWER_FLOOR_MB_PER_S{ 10 };
constexpr uint64_t VALIDATE_FLOOR_MB_PER_S{ 20 };

constexpr double SCALING_SLACK{ 3.0 };
constexpr uint32_t SCALING_RUNS{ 5 };

uint64_t micros_since(std::chrono::steady_clock::time_point start) {
  auto const elapsed{ std::chrono::steady_clock::now() - start };
  uint64_t const us{ static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) };
  return (us == 0) ? 1U : us;
}

template <typename Once>
uint64_t fastest_micros(Once &&once) {
  uint64_t best{ UINT64_MAX };
  for (uint32_t i = 0; i < SCALING_RUNS; ++i) {
    auto const start{ std::chrono::steady_clock::now() };
    once();
    uint64_t const us{ micros_since(start) };
    best = (us < best) ? us : best;
  }
  return best;
}

uint64_t throughput_mb_per_s(uint64_t bytes, uint64_t micros) {
  return (bytes * 1'000'000ULL) / (micros * 1024ULL * 1024ULL);
}

std::string generate(uint64_t target_bytes, SynthStats &stats) {
  SynthSpec spec{ synth_default_spec() };
  spec.depth = 16;
  spec.min_bytes = target_bytes;
  return synth_document(spec, stats);
}

ParsedDocument parsed(std::string const &text) {
  ParsedDocument pd;
  std::vector<Diagnostic> diags;
  REQUIRE(parse_document(raw(text),
                         text.size(),
                         "perf.scav",
                         parse_default_options(),
                         pd,
                         diags));
  return pd;
}

uint64_t time_lower(ParsedDocument const &pd, Chart &c) {
  std::vector<Diagnostic> diags;
  auto const start{ std::chrono::steady_clock::now() };
  bool const clean{ lower_document(c, pd, diags) };
  uint64_t const micros{ micros_since(start) };
  REQUIRE(clean);
  return micros;
}

uint64_t time_validate(Chart const &c) {
  std::vector<Diagnostic> diags;
  auto const start{ std::chrono::steady_clock::now() };
  bool const clean{ validate_chart(c, diags) };
  uint64_t const micros{ micros_since(start) };
  REQUIRE(clean);
  return micros;
}

}  // namespace

TEST_CASE(
    "perf: lowering and validation meet their floors, and the chart's"
    " memory is bounded") {
  SynthStats stats{};
  std::string const text{ generate(INPUT_BYTES, stats) };
  uint64_t const bytes{ text.size() };
  ParsedDocument const pd{ parsed(text) };

  Chart c;
  uint64_t const lower_us{ time_lower(pd, c) };
  uint64_t const validate_us{ time_validate(c) };

  // The generator's stats are what the model must contain: nothing dropped,
  // nothing invented but the per-`*` pseudostates.
  uint32_t authored_states{ 0 };
  uint32_t synthesized{ 0 };
  for (State const &s : c.states) {
    if (s.name.len != 0) {
      ++authored_states;
    } else {
      ++synthesized;
    }
  }
  // + the alias host the generator's one include synthesizes (§9).
  CHECK(authored_states == stats.states + c.includes.size());
  CHECK(c.includes.size() == 1);
  CHECK(c.transitions.size() == stats.transitions);
  CHECK(c.attrs.size() >= stats.attrs);  // a block entry lowers to >= 1 row
  CHECK(synthesized > 0);

  // Entity rows cost more than the text that declared them -- a 12-byte
  // `state A123,` becomes a 52-byte row plus ids, statement, and name -- so
  // the honest bound is a multiple, asserted so it cannot quietly grow.
  uint64_t const footprint{ chart_footprint(c) };
  CHECK_MESSAGE(footprint < bytes * 8,
                "chart " << ((footprint * 100) / bytes) << "% of input");

  if (ASSERT_FLOOR) {
    uint64_t const lower_rate{ throughput_mb_per_s(bytes, lower_us) };
    uint64_t const validate_rate{ throughput_mb_per_s(bytes, validate_us) };
    CHECK_MESSAGE(lower_rate >= LOWER_FLOOR_MB_PER_S, "lower " << lower_rate << " MiB/s");
    CHECK_MESSAGE(validate_rate >= VALIDATE_FLOOR_MB_PER_S,
                  "validate " << validate_rate << " MiB/s");
  }
}

TEST_CASE("perf: lowering is linear in the input") {
  SynthStats stats{};
  uint64_t const small_target{ INPUT_BYTES / 8 };
  std::string const small{ generate(small_target, stats) };
  std::string const large{ generate(small_target * 4, stats) };
  ParsedDocument const small_pd{ parsed(small) };
  ParsedDocument const large_pd{ parsed(large) };
  double const ratio{ static_cast<double>(large.size()) /
                      static_cast<double>(small.size()) };

  auto const lower_once = [](ParsedDocument const &pd) {
    Chart c;
    std::vector<Diagnostic> diags;
    REQUIRE(lower_document(c, pd, diags));
  };
  lower_once(small_pd);  // warm both paths before timing either
  lower_once(large_pd);
  uint64_t const small_us{ fastest_micros([&] { lower_once(small_pd); }) };
  uint64_t const large_us{ fastest_micros([&] { lower_once(large_pd); }) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: validation is linear in the model") {
  SynthStats stats{};
  uint64_t const small_target{ INPUT_BYTES / 8 };
  std::string const small{ generate(small_target, stats) };
  std::string const large{ generate(small_target * 4, stats) };
  ParsedDocument const small_pd{ parsed(small) };
  ParsedDocument const large_pd{ parsed(large) };
  double const ratio{ static_cast<double>(large.size()) /
                      static_cast<double>(small.size()) };

  Chart small_chart;
  Chart large_chart;
  std::vector<Diagnostic> diags;
  REQUIRE(lower_document(small_chart, small_pd, diags));
  REQUIRE(lower_document(large_chart, large_pd, diags));

  time_validate(small_chart);  // warm
  time_validate(large_chart);
  uint64_t const small_us{ fastest_micros([&] { time_validate(small_chart); }) };
  uint64_t const large_us{ fastest_micros([&] { time_validate(large_chart); }) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: a wide sibling list lowers without degrading") {
  // Every statement in one block: the shape that turns quadratic when an
  // append pays a fix-up walk over every span it did not touch.
  auto const wide = [](uint32_t count) {
    std::string out{ "chart wide {" };
    for (uint32_t i = 0; i < count; ++i) { out += "state W" + std::to_string(i) + ","; }
    out += "trans * -> W0,}";
    return out;
  };
  std::string const small{ wide(4'000) };
  std::string const large{ wide(16'000) };
  ParsedDocument const small_pd{ parsed(small) };
  ParsedDocument const large_pd{ parsed(large) };

  auto const lower_once = [](ParsedDocument const &pd) {
    Chart c;
    std::vector<Diagnostic> diags;
    REQUIRE(lower_document(c, pd, diags));
  };
  lower_once(small_pd);
  lower_once(large_pd);
  uint64_t const small_us{ fastest_micros([&] { lower_once(small_pd); }) };
  uint64_t const large_us{ fastest_micros([&] { lower_once(large_pd); }) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < 4.0 * SCALING_SLACK,
                  "grew " << growth << "x for 4x the states");
  }
}
