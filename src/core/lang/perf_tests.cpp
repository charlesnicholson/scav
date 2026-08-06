// Floors, never times: the job is catching an accidental O(n^2), so the
// load-bearing assertion is the machine-independent *scaling* one. Inputs are
// built in RAM, and instrumented builds shrink them and skip the floor.

#include "core/lang/synth_document.h"
#include "core/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

constexpr uint64_t INPUT_BYTES{ SCAV_PERF_INPUT_BYTES };
constexpr bool ASSERT_FLOOR{ SCAV_PERF_ASSERT_FLOOR != 0 };

// An order of magnitude under a 2020-era laptop: a halved throughput is not what
// this catches, and a quadratic one blows through it regardless.
constexpr uint64_t LEX_FLOOR_MB_PER_S{ 20 };
constexpr uint64_t PARSE_FLOOR_MB_PER_S{ 10 };

// 4x input for 4x work is linear; 3x slack covers cache effects and a noisy box,
// and is still nowhere near the 16x a quadratic term costs.
constexpr double SCALING_SLACK{ 3.0 };

uint64_t micros_since(std::chrono::steady_clock::time_point start) {
  auto const elapsed{ std::chrono::steady_clock::now() - start };
  uint64_t const us{ static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) };
  return (us == 0) ? 1U : us;  // a zero denominator says nothing useful
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

struct LexTiming {
  uint64_t micros;
  uint64_t tokens;
  uint64_t footprint;
};

LexTiming time_lex(std::vector<scav_byte> const &bytes) {
  LexResult lexed;
  std::vector<Diagnostic> diags;
  auto const start{ std::chrono::steady_clock::now() };
  bool const ok{
    lex_source(bytes.data(), static_cast<uint32_t>(bytes.size()), DocId{ 0 }, lexed, diags)
  };
  LexTiming const t{ .micros = micros_since(start),
                     .tokens = lexed.tokens.size(),
                     .footprint = lex_footprint(lexed) };
  REQUIRE(ok);
  return t;
}

uint64_t time_parse(std::vector<scav_byte> const &bytes,
                    LexResult const &lexed,
                    uint64_t &footprint) {
  ParsedDocument pd;
  std::vector<Diagnostic> diags;
  auto const start{ std::chrono::steady_clock::now() };
  bool const ok{ parse_tokens(bytes.data(),
                              static_cast<uint32_t>(bytes.size()),
                              lexed,
                              DocId{ 0 },
                              "perf.scav",
                              parse_default_options(),
                              pd,
                              diags) };
  uint64_t const micros{ micros_since(start) };
  REQUIRE(ok);
  footprint = parse_footprint(pd);
  return micros;
}

std::vector<scav_byte> normalized(std::string const &text) {
  std::vector<scav_byte> out;
  std::vector<Diagnostic> diags;
  REQUIRE(source_text_normalize(raw(text), size32(text), DocId{ 0 }, out, diags));
  return out;
}

}  // namespace

TEST_CASE("perf: lex and parse a large document in RAM") {
  SynthStats stats{};
  std::string const text{ generate(INPUT_BYTES, stats) };
  uint64_t const bytes{ text.size() };
  std::vector<scav_byte> const source{ normalized(text) };
  LexTiming const lexing{ time_lex(source) };

  LexResult lexed;
  std::vector<Diagnostic> diags;
  REQUIRE(lex_source(source.data(),
                     static_cast<uint32_t>(source.size()),
                     DocId{ 0 },
                     lexed,
                     diags));

  uint64_t parse_bytes{ 0 };
  uint64_t const parse_micros{ time_parse(source, lexed, parse_bytes) };

  uint64_t const lex_rate{ throughput_mb_per_s(bytes, lexing.micros) };
  uint64_t const parse_rate{ throughput_mb_per_s(bytes, parse_micros) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(lex_rate >= LEX_FLOOR_MB_PER_S,
                  "lex " << lex_rate << " MiB/s over " << lexing.tokens << " tokens");
    CHECK_MESSAGE(parse_rate >= PARSE_FLOOR_MB_PER_S, "parse " << parse_rate << " MiB/s");
  }
}

TEST_CASE("perf: peak memory is a bounded multiple of the input") {
  // The cost of materializing the token stream rather than pulling it, stated
  // as a number instead of left as a claim.
  SynthStats stats{};
  std::string const text{ generate(INPUT_BYTES, stats) };
  uint64_t const bytes{ text.size() };
  std::vector<scav_byte> const source{ normalized(text) };

  LexResult lexed;
  std::vector<Diagnostic> diags;
  REQUIRE(lex_source(source.data(),
                     static_cast<uint32_t>(source.size()),
                     DocId{ 0 },
                     lexed,
                     diags));
  uint64_t const lex_bytes{ lex_footprint(lexed) };

  ParsedDocument pd;
  REQUIRE(parse_tokens(source.data(),
                       static_cast<uint32_t>(source.size()),
                       lexed,
                       DocId{ 0 },
                       "perf.scav",
                       parse_default_options(),
                       pd,
                       diags));
  uint64_t const parse_bytes{ parse_footprint(pd) };

  // The token vector is 12 bytes per token against roughly six bytes of source
  // each, so about 2x. Four is the ceiling this asserts.
  CHECK_MESSAGE(lex_bytes < bytes * 4,
                "token stream " << ((lex_bytes * 100) / bytes) << "% of input");
  // The document holds src_bytes at 1x plus one row per statement, so under 3x.
  CHECK_MESSAGE(parse_bytes < bytes * 4,
                "document " << ((parse_bytes * 100) / bytes) << "% of input");
  // Both are freed independently: the parse does not need the tokens afterwards.
  CHECK(parse_bytes > bytes);
}

TEST_CASE("perf: lexing is linear in the input") {
  // The machine-independent half, and the one that actually catches an
  // accidental quadratic.
  SynthStats stats{};
  uint64_t const small_target{ INPUT_BYTES / 8 };
  std::string const small{ generate(small_target, stats) };
  std::string const large{ generate(small_target * 4, stats) };

  std::vector<scav_byte> const small_bytes{ normalized(small) };
  std::vector<scav_byte> const large_bytes{ normalized(large) };
  double const ratio{ static_cast<double>(large_bytes.size()) /
                      static_cast<double>(small_bytes.size()) };

  // Warm both paths first, so the first measurement is not paying for page
  // faults the second one does not.
  time_lex(small_bytes);
  time_lex(large_bytes);
  uint64_t const small_us{ time_lex(small_bytes).micros };
  uint64_t const large_us{ time_lex(large_bytes).micros };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: parsing is linear in the input") {
  SynthStats stats{};
  uint64_t const small_target{ INPUT_BYTES / 8 };
  std::string const small{ generate(small_target, stats) };
  std::string const large{ generate(small_target * 4, stats) };

  std::vector<scav_byte> const small_bytes{ normalized(small) };
  std::vector<scav_byte> const large_bytes{ normalized(large) };
  double const ratio{ static_cast<double>(large_bytes.size()) /
                      static_cast<double>(small_bytes.size()) };

  LexResult small_lexed;
  LexResult large_lexed;
  std::vector<Diagnostic> diags;
  REQUIRE(lex_source(small_bytes.data(),
                     static_cast<uint32_t>(small_bytes.size()),
                     DocId{ 0 },
                     small_lexed,
                     diags));
  REQUIRE(lex_source(large_bytes.data(),
                     static_cast<uint32_t>(large_bytes.size()),
                     DocId{ 0 },
                     large_lexed,
                     diags));

  uint64_t footprint{ 0 };
  time_parse(small_bytes, small_lexed, footprint);
  time_parse(large_bytes, large_lexed, footprint);
  uint64_t const small_us{ time_parse(small_bytes, small_lexed, footprint) };
  uint64_t const large_us{ time_parse(large_bytes, large_lexed, footprint) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: a wide sibling list does not degrade") {
  // One block with every statement in it, which is the shape a naive
  // children-span implementation turns quadratic.
  auto const wide = [](uint32_t count) {
    std::string out{ "chart wide {" };
    for (uint32_t i = 0; i < count; ++i) {
      out += "state S";
      out += std::to_string(i);
      out += ",";
    }
    out += "}";
    return out;
  };

  std::string const small{ wide(4000) };
  std::string const large{ wide(16000) };
  double const ratio{ static_cast<double>(large.size()) /
                      static_cast<double>(small.size()) };

  parse(small);
  parse(large);
  auto start{ std::chrono::steady_clock::now() };
  REQUIRE(parse(small).ok);
  uint64_t const small_us{ micros_since(start) };
  start = std::chrono::steady_clock::now();
  REQUIRE(parse(large).ok);
  uint64_t const large_us{ micros_since(start) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: a long comment run does not degrade") {
  // Trivia attachment walks the statement tree once and counting-sorts, so a
  // document that is almost entirely comments must stay linear.
  auto const commented = [](uint32_t count) {
    std::string out{ "chart c {\n" };
    for (uint32_t i = 0; i < count; ++i) {
      out += "// comment number ";
      out += std::to_string(i);
      out += "\nstate S";
      out += std::to_string(i);
      out += ", // trailing\n";
    }
    out += "}\n";
    return out;
  };

  std::string const small{ commented(2000) };
  std::string const large{ commented(8000) };
  double const ratio{ static_cast<double>(large.size()) /
                      static_cast<double>(small.size()) };

  parse(small);
  parse(large);
  auto start{ std::chrono::steady_clock::now() };
  Parsed const small_parsed{ parse(small) };
  uint64_t const small_us{ micros_since(start) };
  start = std::chrono::steady_clock::now();
  Parsed const large_parsed{ parse(large) };
  uint64_t const large_us{ micros_since(start) };

  REQUIRE(small_parsed.ok);
  REQUIRE(large_parsed.ok);
  CHECK(small_parsed.pd.comments.size() == 4000);
  CHECK(large_parsed.pd.comments.size() == 16000);

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: string-pool growth does not degrade with distinct names") {
  // Every name unique, so interning never hits its fast path and the finalizing
  // sort runs on the full set.
  auto const distinct = [](uint32_t count) {
    std::string out{ "chart c {" };
    for (uint32_t i = 0; i < count; ++i) {
      // Prefixed so byte order is nothing like encounter order, which is what
      // the sort has to cope with.
      out += "state N";
      out += std::to_string((i * 2654435761U) % 1000000007U);
      out += ",";
    }
    out += "}";
    return out;
  };

  std::string const small{ distinct(4000) };
  std::string const large{ distinct(16000) };
  double const ratio{ static_cast<double>(large.size()) /
                      static_cast<double>(small.size()) };

  parse(small);
  parse(large);
  auto start{ std::chrono::steady_clock::now() };
  REQUIRE(parse(small).ok);
  uint64_t const small_us{ micros_since(start) };
  start = std::chrono::steady_clock::now();
  REQUIRE(parse(large).ok);
  uint64_t const large_us{ micros_since(start) };

  // n log n, not n, so the slack has to cover the log term as well.
  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK * 2.0,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: deep nesting does not degrade") {
  // Pushing a frame is amortized constant; materializing each block's children
  // into the shared id array is where a per-close copy would show up.
  std::string const small{ synth_deep_document(60) };
  std::string const large{ synth_deep_document(240) };
  double const ratio{ static_cast<double>(large.size()) /
                      static_cast<double>(small.size()) };

  CHECK(parse_deep(small, 300).ok);
  CHECK(parse_deep(large, 300).ok);
  auto start{ std::chrono::steady_clock::now() };
  REQUIRE(parse_deep(small, 300).ok);
  uint64_t const small_us{ micros_since(start) };
  start = std::chrono::steady_clock::now();
  REQUIRE(parse_deep(large, 300).ok);
  uint64_t const large_us{ micros_since(start) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: the generated document is what it claims to be") {
  // A performance test over input nobody checked measures the generator.
  SynthStats stats{};
  std::string const text{ generate(INPUT_BYTES / 16, stats) };
  Parsed const r{ parse(text) };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));
  CHECK(r.pd.stmts.size() == stats.statements);
  CHECK(r.pd.comments.size() == stats.comments);
  CHECK(stmts_of(r.pd, ElemKind::State).size() == stats.states);
  CHECK(stmts_of(r.pd, ElemKind::Submachine).size() == stats.submachines);
  CHECK(stmts_of(r.pd, ElemKind::Trans).size() == stats.transitions);
  CHECK(stmts_of(r.pd, ElemKind::Attr).size() == stats.attrs);
}
