// The deterministic mutation sweep, aimed past the parser: whatever survives
// parsing must lower and validate without crashing or lying. "Lying" means a
// diagnostic pointing outside the document, a subject naming a row that does
// not exist, or an address that does not round-trip on a clean chart.

#include "core/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

// splitmix64's finalizer, position-addressed: reproducible from the seed alone.
uint64_t rnd(uint64_t seed, uint64_t index) {
  uint64_t x{ seed + (index * 0x9E37'79B9'7F4A'7C15ULL) };
  x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31U);
}

// Model-shaped seeds: every construct lowering has an opinion about.
std::vector<std::string> seed_corpus() {
  return {
    "chart c { state A, state B, trans A -> B \"go\", trans * -> A, }",
    R"(chart c { include "w.scav" as w, trans w/X -> w, })",
    R"(chart c { state On { submachine m { state I, trans * -> I, }, submachine n { state I, }, }, trans On:m/I -> On:1/I, })",
    "chart c { trans * -> *, trans A -> B:9, submachine m { state X, }, }",
    R"(chart c { state A { @k, @ns { a, b = "v" }, }, trans A -> A { @w = "1", }, })",
    "chart c { state S { state A, submachine m { state A, }, }, trans S:0/A -> S:m/A, }",
    "chart c { state A, state A, trans * -> A, trans * -> A, }",
    "chart c { state A, state B, trans A -> B:1, }",
  };
}

constexpr std::string_view INTERESTING{ "{}[],=@:/*->\"'\\\n\t 0aA_\x7f\x80\xc3\xa9\xff" };

std::string mutate(std::string_view seed, uint64_t key) {
  std::string s{ seed };
  uint32_t const rounds{ static_cast<uint32_t>(rnd(key, 0) % 6U) + 1U };
  for (uint32_t round = 0; round < rounds; ++round) {
    uint64_t const r{ rnd(key, round + 1U) };
    uint32_t const op{ static_cast<uint32_t>(r % 4U) };
    uint32_t const at{ s.empty() ? 0U : static_cast<uint32_t>((r >> 8U) % s.size()) };
    char const ch{ INTERESTING[(r >> 32U) % INTERESTING.size()] };
    if ((op == 0) || s.empty()) {
      s.insert(s.begin() + at, ch);
    } else if (op == 1) {
      s[at] = ch;
    } else if (op == 2) {
      s.erase(s.begin() + at);
    } else {
      s.resize(at);
    }
  }
  return s;
}

// Lowering diagnostics are statement-shaped: doc 0, span inside the source.
void check_lower_diags(Chart const &c, std::vector<Diagnostic> const &diags) {
  for (Diagnostic const &d : diags) {
    CHECK(d.code != DiagCode::Ok);
    CHECK(d.doc == DocId{ 0 });
    CHECK(static_cast<size_t>(d.src.off) + d.src.len <= c.src_bytes.size());
  }
}

// Validation subjects must name rows that exist, whatever the model held.
void check_validate_diags(Chart const &c, std::vector<Diagnostic> const &diags) {
  for (Diagnostic const &d : diags) {
    switch (d.subject.kind) {
      case ElemKind::State: CHECK(d.subject.ordinal < c.states.size()); break;
      case ElemKind::Submachine: CHECK(d.subject.ordinal < c.submachines.size()); break;
      case ElemKind::Transition: CHECK(d.subject.ordinal < c.transitions.size()); break;
      case ElemKind::Chart: CHECK(d.subject.ordinal == 0); break;
      case ElemKind::Point:
      case ElemKind::PathBox: FAIL("no validator names these"); break;
      case ElemKind::None: break;  // a column finding, or an include's
    }
  }
}

}  // namespace

TEST_CASE("fuzz: whatever parses also lowers, validates, and addresses") {
  constexpr uint64_t SEED{ 0x5CA1'AB1E'0000'0101ULL };
  std::vector<std::string> const seeds{ seed_corpus() };
  for (uint32_t i = 0; i < 2000; ++i) {
    std::string const input{ mutate(seeds[i % seeds.size()], SEED + i) };
    CAPTURE(i);
    CAPTURE(input);

    Parsed const p{ parse(input, "fuzz.scav") };
    if (!p.ok) { continue; }

    Chart c;
    std::vector<Diagnostic> lower_diags;
    lower_document(c, p.pd, lower_diags);
    check_lower_diags(c, lower_diags);

    std::vector<Diagnostic> validate_diags;
    bool const clean{ validate_chart(c, validate_diags) };
    check_validate_diags(c, validate_diags);

    // Every live state prints an address, and on a clean chart the address
    // resolves back to the row that printed it. Duplicate names make the
    // text ambiguous, which is exactly what validation just rejected.
    for (uint32_t s = 0; s < c.states.size(); ++s) {
      if (c.states[s].live == 0) { continue; }
      std::string const address{ path(c, StateId{ s }) };
      CHECK_FALSE(address.empty());
      if (clean) {
        StateId out{ INVALID };
        CAPTURE(address);
        CHECK(resolve_path(c, c.root_submachine, address, out) == ResolveStatus::Ok);
        CHECK(out == StateId{ s });
      }
    }
  }
}

TEST_CASE("fuzz: the seeds themselves exercise every lowering diagnostic") {
  // Not a mutation: the unmutated seeds cover the diagnostic codes, so the
  // sweep starts from inputs that already reach the interesting paths.
  std::vector<std::string> const seeds{ seed_corpus() };
  std::vector<DiagCode> met;
  for (std::string const &seed : seeds) {
    Parsed const p{ parse(seed, "seed.scav") };
    if (!p.ok) { continue; }
    Chart c;
    std::vector<Diagnostic> diags;
    lower_document(c, p.pd, diags);
    for (Diagnostic const &d : diags) { met.push_back(d.code); }
  }
  auto const saw = [&](DiagCode code) {
    for (DiagCode const m : met) {
      if (m == code) { return true; }
    }
    return false;
  };
  CHECK(saw(DiagCode::WildcardBothEndpoints));
  CHECK(saw(DiagCode::EndpointUnresolved));
  CHECK(saw(DiagCode::BadSubmachineQualifier));
  CHECK(saw(DiagCode::EndpointCrossesInclude));
  CHECK(saw(DiagCode::MisplacedStatement));
}
