// Asserts throughput floors and linear scaling to catch an accidental O(n^2);
// scaling is machine-independent. Instrumented builds shrink the input.

#include "core/core_internal.h"
#include "core/tests/perf_support.h"
#include "core/tests/test_support.h"
#include "core/tests/test_synth.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

constexpr uint64_t INPUT_BYTES{ SCAV_PERF_INPUT_BYTES };
constexpr bool ASSERT_FLOOR{ SCAV_PERF_ASSERT_FLOOR != 0 };
constexpr bool ASSERT_SCALING{ SCAV_PERF_ASSERT_SCALING != 0 };

// Throughput floors in MiB/s, an order of magnitude below a laptop's rate.
constexpr uint64_t NORMALIZE_FLOOR_MB_PER_S{ 20 };
constexpr uint64_t LEX_FLOOR_MB_PER_S{ 20 };
constexpr uint64_t PARSE_FLOOR_MB_PER_S{ 10 };

// Allowed growth over linear: 4x input may take under 12x time; quadratic is 16x.
constexpr double SCALING_SLACK{ 3.0 };

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

uint64_t time_normalize(std::string const &text) {
  std::vector<scav_byte> out;
  std::vector<Diagnostic> diags;
  auto const start{ std::chrono::steady_clock::now() };
  bool const ok{ source_text_normalize(raw(text), size32(text), DocId{ 0 }, out, diags) };
  uint64_t const micros{ micros_since(start) };
  REQUIRE(ok);
  return micros;
}

constexpr uint64_t PRINT_FLOOR_MB_PER_S{ 5 };

struct Printed {
  uint64_t micros;
  uint64_t bytes;
};

// Times the print alone, over an already-parsed document.
Printed time_print(ParsedDocument const &pd) {
  std::string out;
  auto const start{ std::chrono::steady_clock::now() };
  bool const ok{ print_document(pd, print_default_options(), out) };
  Printed const p{ .micros = micros_since(start), .bytes = out.size() };
  REQUIRE(ok);
  return p;
}

ParsedDocument parse_for_print(std::string const &text) {
  ParsedDocument pd;
  std::vector<Diagnostic> diags;
  REQUIRE(parse_document(raw(text),
                         size32(text),
                         "perf.scav",
                         parse_default_options(),
                         pd,
                         diags));
  return pd;
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

  uint64_t const norm_micros{ time_normalize(text) };
  uint64_t const norm_rate{ throughput_mb_per_s(bytes, norm_micros) };
  uint64_t const lex_rate{ throughput_mb_per_s(bytes, lexing.micros) };
  uint64_t const parse_rate{ throughput_mb_per_s(bytes, parse_micros) };
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(norm_rate >= NORMALIZE_FLOOR_MB_PER_S,
                  "normalize " << norm_rate << " MiB/s");
    CHECK_MESSAGE(lex_rate >= LEX_FLOOR_MB_PER_S,
                  "lex " << lex_rate << " MiB/s over " << lexing.tokens << " tokens");
    CHECK_MESSAGE(parse_rate >= PARSE_FLOOR_MB_PER_S, "parse " << parse_rate << " MiB/s");

    // On the same machine, normalize runs at least as fast as lex.
    CHECK_MESSAGE(norm_rate >= lex_rate,
                  "normalize " << norm_rate << " MiB/s is slower than lex " << lex_rate);
  }
}

TEST_CASE("perf: peak memory is a bounded multiple of the input") {
  // Bounds the token stream and parsed document as multiples of the input size.
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
  // The document holds its own copy of the source.
  CHECK(parse_bytes > bytes);
}

TEST_CASE("perf: lexing is linear in the input" * doctest::test_suite("full")) {
  SynthStats stats{};
  uint64_t const small_target{ INPUT_BYTES / 8 };
  std::string const small{ generate(small_target, stats) };
  std::string const large{ generate(small_target * 4, stats) };

  std::vector<scav_byte> const small_bytes{ normalized(small) };
  std::vector<scav_byte> const large_bytes{ normalized(large) };
  double const ratio{ static_cast<double>(large_bytes.size()) /
                      static_cast<double>(small_bytes.size()) };

  // Warms both inputs so page faults fall outside the timed runs.
  time_lex(small_bytes);
  time_lex(large_bytes);
  auto const [small_us, large_us]{ best_pair([&] { time_lex(small_bytes); },
                                             [&] { time_lex(large_bytes); }) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_SCALING) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: parsing is linear in the input" * doctest::test_suite("full")) {
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
  auto const [small_us, large_us]{ best_pair(
      [&] { time_parse(small_bytes, small_lexed, footprint); },
      [&] { time_parse(large_bytes, large_lexed, footprint); }) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_SCALING) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: a wide sibling list does not degrade" * doctest::test_suite("full")) {
  // One block holding every statement as siblings.
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

  REQUIRE(parse(small).ok);
  REQUIRE(parse(large).ok);
  auto const [small_us,
              large_us]{ best_pair([&] { parse(small); }, [&] { parse(large); }) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_SCALING) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: a long comment run does not degrade" * doctest::test_suite("full")) {
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

  Parsed const small_parsed{ parse(small) };
  Parsed const large_parsed{ parse(large) };
  auto const [small_us,
              large_us]{ best_pair([&] { parse(small); }, [&] { parse(large); }) };

  REQUIRE(small_parsed.ok);
  REQUIRE(large_parsed.ok);
  CHECK(small_parsed.pd.comments.size() == 4000);
  CHECK(large_parsed.pd.comments.size() == 16000);

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_SCALING) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: deep nesting does not degrade" * doctest::test_suite("full")) {
  // Pushing a frame is amortized constant; each block close copies its children
  // into the shared id array.
  std::string const small{ synth_deep_document(60) };
  std::string const large{ synth_deep_document(240) };
  double const ratio{ static_cast<double>(large.size()) /
                      static_cast<double>(small.size()) };

  REQUIRE(parse_deep(small, 300).ok);
  REQUIRE(parse_deep(large, 300).ok);
  auto const [small_us, large_us]{ best_pair([&] { parse_deep(small, 300); },
                                             [&] { parse_deep(large, 300); }) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_SCALING) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: the generated document is what it claims to be") {
  SynthStats stats{};
  std::string const text{ generate(INPUT_BYTES / 16, stats) };
  Parsed const r{ parse(text) };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));
  CHECK(r.pd.stmts.size() == stats.statements);
  CHECK(r.pd.comments.size() == stats.comments);
  CHECK(stmts_of(r.pd, StmtKind::State).size() == stats.states);
  CHECK(stmts_of(r.pd, StmtKind::Submachine).size() == stats.submachines);
  CHECK(stmts_of(r.pd, StmtKind::Trans).size() == stats.transitions);
  CHECK(stmts_of(r.pd, StmtKind::Attr).size() == stats.attrs);
}

TEST_CASE("perf: print a large document" * doctest::test_suite("full")) {
  SynthStats stats{};
  std::string const text{ generate(INPUT_BYTES, stats) };
  ParsedDocument const pd{ parse_for_print(text) };

  Printed const printed{ time_print(pd) };
  uint64_t const rate{ throughput_mb_per_s(printed.bytes, printed.micros) };
  MESSAGE("print: " << rate << " MB/s over " << printed.bytes << " bytes");
  if (ASSERT_FLOOR) {
    CHECK_MESSAGE(rate >= PRINT_FLOOR_MB_PER_S, "print at " << rate << " MB/s");
  }
  CHECK(printed.bytes > (text.size() / 2));
  CHECK(printed.bytes < (text.size() * 2));
}

TEST_CASE("perf: printing is linear in the input" * doctest::test_suite("full")) {
  SynthStats stats{};
  uint64_t const small_target{ INPUT_BYTES / 8 };
  ParsedDocument const small{ parse_for_print(generate(small_target, stats)) };
  ParsedDocument const large{ parse_for_print(generate(small_target * 4, stats)) };
  double const ratio{ static_cast<double>(large.src_bytes.size()) /
                      static_cast<double>(small.src_bytes.size()) };

  auto const [small_us, large_us]{ best_pair([&] { std::ignore = time_print(small); },
                                             [&] { std::ignore = time_print(large); }) };

  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_SCALING) {
    CHECK_MESSAGE(growth < ratio * SCALING_SLACK,
                  "grew " << growth << "x for " << ratio << "x the bytes");
  }
}

TEST_CASE("perf: a block with many attributes does not degrade" *
          doctest::test_suite("full")) {
  // Times the printer's attribute sort and merge over one block.
  auto const attr_block{ [](uint32_t count) {
    std::string text{ "chart c {\n" };
    for (uint32_t i = 0; i < count; ++i) {
      text += "  @ns:k";
      text += std::to_string(count - i);  // reverse order, so every one moves
      text += " = \"v\",\n";
    }
    text += "}\n";
    return text;
  } };

  ParsedDocument const small{ parse_for_print(attr_block(2000)) };
  ParsedDocument const large{ parse_for_print(attr_block(8000)) };

  auto const [small_us, large_us]{ best_pair([&] { std::ignore = time_print(small); },
                                             [&] { std::ignore = time_print(large); }) };
  double const growth{ static_cast<double>(large_us) / static_cast<double>(small_us) };
  if (ASSERT_SCALING) {
    CHECK_MESSAGE(growth < 4.0 * SCALING_SLACK,
                  "grew " << growth << "x for 4x the attributes");
  }
}
