// Loader floors, never times, over documents generated in RAM. Catches a
// per-document scan going quadratic in discovery, resolution or the rebuild.

#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

constexpr bool ASSERT_FLOOR{ SCAV_PERF_ASSERT_FLOOR != 0 };

// An order of magnitude under a 2020-era laptop, like every other floor here.
constexpr uint64_t LOAD_FLOOR_MB_PER_S{ 5 };

// Doubling the network must not more than treble the work. Generous, because
// the point is catching a quadratic and not tracking a constant factor.
constexpr double SCALING_SLACK{ 3.0 };
constexpr uint32_t SCALING_RUNS{ 3 };

// Instrumentation makes the absolute numbers describe the instrumentation, so
// those rows run a smaller network; the ratio is what carries the assertion.
constexpr uint32_t WIDE{ (SCAV_PERF_INPUT_BYTES <= 4U * 1024U * 1024U) ? 60U : 200U };
constexpr uint32_t NARROW{ WIDE / 2U };

struct Doc {
  std::string name;
  std::string text;
};

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

// Document i includes document i+1, so the network is `count` deep with one
// instantiation each. Depth stresses the resolver's walk and the rebuild.
std::vector<Doc> chain(uint32_t count, uint32_t states_each) {
  std::vector<Doc> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    std::string text{ "chart d" };
    text += std::to_string(i);
    text += " {\n";
    if (i + 1 < count) {
      text += "include \"d" + std::to_string(i + 1) + ".scav\" as nxt,\n";
    }
    for (uint32_t s = 0; s < states_each; ++s) {
      text += "state S" + std::to_string(s) + ",\n";
    }
    text += "trans * -> S0,\n";
    for (uint32_t s = 0; s + 1 < states_each; ++s) {
      text += "trans S" + std::to_string(s) + " -> S" + std::to_string(s + 1) + ",\n";
    }
    // A cross-document endpoint per document, which is the path that could not
    // resolve at all before the whole network was attached.
    if (i + 1 < count) { text += "trans S0 -> nxt/S0,\n"; }
    text += "}\n";
    out.push_back({ .name = "d" + std::to_string(i) + ".scav", .text = text });
  }
  return out;
}

// One root including `count` leaves, all the same document, so the network is
// one file parsed once against `count` instantiations.
std::vector<Doc> star(uint32_t count, uint32_t states_each) {
  std::string root{ "chart root {\n" };
  for (uint32_t i = 0; i < count; ++i) {
    root += "include \"leaf.scav\" as l" + std::to_string(i) + ",\n";
  }
  root += "state R,\ntrans * -> R,\n}\n";

  std::string leaf{ "chart leaf {\n" };
  for (uint32_t s = 0; s < states_each; ++s) {
    leaf += "state S" + std::to_string(s) + ",\n";
  }
  leaf += "trans * -> S0,\n";
  for (uint32_t s = 0; s + 1 < states_each; ++s) {
    leaf += "trans S" + std::to_string(s) + " -> S" + std::to_string(s + 1) + ",\n";
  }
  leaf += "}\n";
  return { { .name = "root.scav", .text = root }, { .name = "leaf.scav", .text = leaf } };
}

uint64_t corpus_bytes(std::vector<Doc> const &corpus) {
  uint64_t n{ 0 };
  for (Doc const &d : corpus) { n += d.text.size(); }
  return n;
}

struct Outcome {
  uint32_t states;
  uint32_t documents;
  uint32_t includes;
  bool ok;
};

Outcome run_once(std::vector<Doc> const &corpus) {
  Loader loader;
  Chart chart;
  std::vector<Diagnostic> diags;

  bool alive{
    load_add(loader, raw(corpus[0].text), corpus[0].text.size(), corpus[0].name)
  };
  std::vector<std::string> wanted;
  while (alive) {
    wanted.clear();
    for (Pending const &p : load_pending(loader)) {
      wanted.emplace_back(load_pending_path(loader, p));
    }
    if (wanted.empty()) { break; }
    for (std::string const &want : wanted) {
      std::string_view body;
      for (Doc const &d : corpus) {
        if (d.name == want) { body = d.text; }
      }
      REQUIRE_FALSE(body.empty());
      alive = load_add(loader, raw(body), body.size(), want);
      if (!alive) { break; }
    }
  }

  bool const ok{ load_finish(loader, chart, diags) };
  return { .states = static_cast<uint32_t>(chart.states.size()),
           .documents = static_cast<uint32_t>(chart.documents.size()),
           .includes = static_cast<uint32_t>(chart.includes.size()),
           .ok = ok };
}

}  // namespace

TEST_CASE("perf: a deep chain loads at the throughput floor") {
  std::vector<Doc> const corpus{ chain(WIDE, 20) };
  uint64_t const bytes{ corpus_bytes(corpus) };

  Outcome const outcome{ run_once(corpus) };
  REQUIRE(outcome.ok);
  CHECK(outcome.documents == WIDE);
  CHECK(outcome.includes == WIDE - 1);

  uint64_t const us{ fastest_micros([&] { std::ignore = run_once(corpus); }) };
  uint64_t const rate{ (bytes * 1'000'000ULL) / (us * 1024ULL * 1024ULL) };
  MESSAGE("load: " << bytes << " bytes over " << outcome.documents << " documents in "
                   << us << " us (" << rate << " MB/s)");
  if (ASSERT_FLOOR) { CHECK(rate >= LOAD_FLOOR_MB_PER_S); }
}

TEST_CASE("perf: chain load is linear in the number of documents") {
  // The machine-independent assertion. Discovery's find-by-key, the resolver's
  // include scan and the containment rebuild are all per-document scans.
  std::vector<Doc> const small{ chain(NARROW, 20) };
  std::vector<Doc> const large{ chain(WIDE, 20) };

  uint64_t const small_us{ fastest_micros([&] { std::ignore = run_once(small); }) };
  uint64_t const large_us{ fastest_micros([&] { std::ignore = run_once(large); }) };

  double const ratio{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  MESSAGE("chain " << NARROW << " -> " << WIDE << " documents: " << small_us << " us -> "
                   << large_us << " us (" << ratio << "x)");
  CHECK(ratio < (2.0 * SCALING_SLACK));
}

TEST_CASE("perf: instantiation is linear in the number of instantiations") {
  // One document, many instances: parse-once must not be paid per instance,
  // and the span rebuilds must stay one-per-network.
  std::vector<Doc> const small{ star(NARROW, 20) };
  std::vector<Doc> const large{ star(WIDE, 20) };

  Outcome const outcome{ run_once(large) };
  REQUIRE(outcome.ok);
  CHECK(outcome.documents == 2);    // parsed once, whatever the instance count
  CHECK(outcome.includes == WIDE);  // and instantiated once per include

  uint64_t const small_us{ fastest_micros([&] { std::ignore = run_once(small); }) };
  uint64_t const large_us{ fastest_micros([&] { std::ignore = run_once(large); }) };

  double const ratio{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  MESSAGE("star " << NARROW << " -> " << WIDE << " instantiations: " << small_us
                  << " us -> " << large_us << " us (" << ratio << "x)");
  CHECK(ratio < (2.0 * SCALING_SLACK));
}

TEST_CASE("perf: the digest is linear in the model") {
  std::vector<Doc> const small{ chain(NARROW, 20) };
  std::vector<Doc> const large{ chain(WIDE, 20) };

  auto const hash_of = [](std::vector<Doc> const &corpus) {
    Loader loader;
    Chart chart;
    std::vector<Diagnostic> diags;
    std::ignore =
        load_add(loader, raw(corpus[0].text), corpus[0].text.size(), corpus[0].name);
    std::vector<std::string> wanted;
    for (;;) {
      wanted.clear();
      for (Pending const &p : load_pending(loader)) {
        wanted.emplace_back(load_pending_path(loader, p));
      }
      if (wanted.empty()) { break; }
      for (std::string const &want : wanted) {
        for (Doc const &d : corpus) {
          if (d.name == want) {
            std::ignore = load_add(loader, raw(d.text), d.text.size(), want);
          }
        }
      }
    }
    REQUIRE(load_finish(loader, chart, diags));
    return chart;
  };

  Chart const a{ hash_of(small) };
  Chart const b{ hash_of(large) };
  uint64_t const a_us{ fastest_micros([&] { std::ignore = chart_structural_hash(a); }) };
  uint64_t const b_us{ fastest_micros([&] { std::ignore = chart_structural_hash(b); }) };

  double const ratio{ static_cast<double>(b_us) / static_cast<double>(a_us) };
  MESSAGE("digest " << NARROW << " -> " << WIDE << " documents: " << a_us << " us -> "
                    << b_us << " us (" << ratio << "x)");
  CHECK(ratio < (2.0 * SCALING_SLACK));
}
