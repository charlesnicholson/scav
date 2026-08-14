// A deterministic mutation sweep, not libFuzzer: a coverage-guided run only in a
// long-running job is one nobody runs before pushing. The bar is "cannot crash
// and cannot lie" -- every diagnostic still points inside the document.

#include "core/tests/test_support.h"
#include "core/tests/test_synth.h"
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

// splitmix64's finalizer, position-addressed rather than stateful: a failure is
// reproducible from the seed alone.
uint64_t rnd(uint64_t seed, uint64_t index) {
  uint64_t x{ seed + (index * 0x9E37'79B9'7F4A'7C15ULL) };
  x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31U);
}

std::vector<std::string> seed_corpus() {
  return {
    "",
    "chart c {}",
    R"(chart c { state A, state B "label", trans A -> B "ev", })",
    R"(chart eg { include "w.scav" as w, state On { m main { s Idle, t * -> Idle, }, }, })",
    R"(chart c { @flag, @ns:key = "v", @ns { a, b = "c" }, @l = ["x", "y"], })",
    "chart c { trans On:1/Idle -> On:main/Ready, trans * -> A, trans A -> *, }",
    "chart c {\n  // comment\n  state A, // trailing\n\n  // floating\n  state B,\n}",
    "\"\"\"raw\nstring\"\"\"",
    R"(chart c { state A { state B { state C { state D, }, }, }, })",
    "chart\tc\v{\fstate A,}",
  };
}

// The bytes a mutation reaches for: everything structural in the grammar, plus
// the characters most likely to break an assumption.
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
      // Truncation, which is the shape that finds a missing bounds check.
      s.resize(at);
    }
  }
  return s;
}

// The invariants every run has to hold, whatever the input said.
void check_diagnostics(std::vector<Diagnostic> const &diags, uint32_t doc_len) {
  for (Diagnostic const &d : diags) {
    CHECK(d.code != DiagCode::Ok);
    CHECK(d.doc == DocId{ 0 });
    CHECK(static_cast<size_t>(d.src.off) + d.src.len <= doc_len);
  }
}

void check_document(ParsedDocument const &pd) {
  uint32_t const len{ static_cast<uint32_t>(pd.src_bytes.size()) };
  for (uint32_t i = 0; i < pd.stmts.size(); ++i) {
    CHECK(static_cast<size_t>(pd.stmts[i].src.off) + pd.stmts[i].src.len <= len);
    Span const kids{ pd.stmt_children[i] };
    CHECK(static_cast<size_t>(kids.off) + kids.len <= pd.stmt_ids.size());
    for (uint32_t k = 0; k < kids.len; ++k) {
      CHECK(pd.stmt_ids[kids.off + k].v < pd.stmts.size());
    }
    Span const trivia{ pd.stmts[i].comments };
    CHECK(static_cast<size_t>(trivia.off) + trivia.len <= pd.comments.size());
  }
  for (Trivia const &t : pd.comments) {
    CHECK(static_cast<size_t>(t.src.off) + t.src.len <= len);
  }
}

}  // namespace

TEST_CASE("fuzz: the lexer survives arbitrary bytes") {
  constexpr uint64_t SEED{ 0x5CA1'AB1E'0000'0001ULL };
  std::vector<std::string> const seeds{ seed_corpus() };
  for (uint32_t i = 0; i < 4000; ++i) {
    std::string const input{ mutate(seeds[i % seeds.size()], SEED + i) };
    std::vector<scav_byte> normalized;
    std::vector<Diagnostic> diags;
    if (!source_text_normalize(raw(input), size32(input), DocId{ 0 }, normalized, diags)) {
      check_diagnostics(diags, size32(input));
      continue;
    }

    uint32_t const len{ static_cast<uint32_t>(normalized.size()) };
    LexResult lexed;
    lex_source(normalized.data(), len, DocId{ 0 }, lexed, diags);
    check_diagnostics(diags, len);

    // The End sentinel is what lets lookahead skip its bounds check, so it is
    // owed on every path including the failing ones.
    REQUIRE_FALSE(lexed.tokens.empty());
    CHECK(lexed.tokens.back().kind == TokKind::End);
    for (Token const &t : lexed.tokens) {
      CHECK(static_cast<size_t>(t.off) + t.len <= len);
    }
    for (LexComment const &c : lexed.comments) {
      CHECK(static_cast<size_t>(c.src.off) + c.src.len <= len);
    }
  }
}

TEST_CASE("fuzz: the parser survives arbitrary bytes") {
  constexpr uint64_t SEED{ 0x5CA1'AB1E'0000'0002ULL };
  std::vector<std::string> const seeds{ seed_corpus() };
  uint32_t accepted{ 0 };
  for (uint32_t i = 0; i < 4000; ++i) {
    std::string const input{ mutate(seeds[i % seeds.size()], SEED + i) };
    Parsed const r{ parse(input) };
    accepted += r.ok ? 1U : 0U;
    check_diagnostics(r.diags, size32(input) + 8U);
    check_document(r.pd);
    // A failed parse reports why; a successful one has nothing to report.
    CHECK(r.ok == r.diags.empty());
  }
  // Not an assertion about the grammar, only that the corpus is not so mangled
  // that the accepting path went untested.
  CHECK(accepted > 0);
}

TEST_CASE("fuzz: a partially parsed document still has consistent spans") {
  constexpr uint64_t SEED{ 0x5CA1'AB1E'0000'0003ULL };
  // Truncation at every byte of a real chart: each prefix is a document that
  // stops mid-statement, which is where a span left half-written would show.
  std::string_view const full{
    R"(chart c { include "w.scav" as w, state On { @doc = "x", m main { s Idle, t * -> Idle, }, }, })"
  };
  for (uint32_t cut = 0; cut <= full.size(); ++cut) {
    Parsed const r{ parse(full.substr(0, cut)) };
    check_document(r.pd);
    check_diagnostics(r.diags, cut + 8U);
    if (cut < full.size()) { CHECK_FALSE(r.ok); }
  }
  CHECK(parse(full).ok);

  // And a prefix of the *generated* corpus, which nests far deeper.
  SynthSpec spec{ synth_default_spec() };
  spec.depth = 6;
  spec.min_roots = 1;
  SynthStats stats{};
  std::string const text{ synth_document(spec, stats) };
  for (uint32_t cut = 0; cut < text.size(); cut += 37) {
    Parsed const r{ parse(std::string_view{ text }.substr(0, cut)) };
    check_document(r.pd);
  }
  CHECK_MESSAGE(rnd(SEED, 0) != 0, "the generator is position-addressed, not stateful");
}

TEST_CASE("fuzz: every accepted document round-trips its own source spans") {
  constexpr uint64_t SEED{ 0x5CA1'AB1E'0000'0004ULL };
  std::vector<std::string> const seeds{ seed_corpus() };
  for (uint32_t i = 0; i < 2000; ++i) {
    std::string const input{ mutate(seeds[i % seeds.size()], SEED + i) };
    Parsed const r{ parse(input) };
    if (!r.ok) { continue; }
    // Re-parsing the bytes the parser kept must give the same tree: src_bytes
    // is normalized, so normalizing it again is the identity.
    Parsed const again{ parse(
        std::string_view{ reinterpret_cast<char const *>(r.pd.src_bytes.data()),
                          r.pd.src_bytes.size() }) };
    REQUIRE(again.ok);
    CHECK(again.pd.src_bytes == r.pd.src_bytes);
    CHECK(again.pd.strings.bytes == r.pd.strings.bytes);
    CHECK(again.pd.stmts.size() == r.pd.stmts.size());
  }
}

TEST_CASE("fuzz: the mutation corpus is reproducible from its seed") {
  // A fuzz failure nobody can reproduce is a flake, so this is load-bearing.
  CHECK(mutate("chart c {}", 12345) == mutate("chart c {}", 12345));
  CHECK(mutate("chart c {}", 12345) != mutate("chart c {}", 12346));
  CHECK(rnd(1, 1) == rnd(1, 1));
  CHECK(rnd(1, 1) != rnd(1, 2));
  CHECK(rnd(1, 1) != rnd(2, 1));
}
