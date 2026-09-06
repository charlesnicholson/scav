#ifndef SCAV_CORE_TESTS_TEST_SUPPORT_H_INCLUDED
#define SCAV_CORE_TESTS_TEST_SUPPORT_H_INCLUDED

// Shared by the core unit tests, header-only and test-only. Charts are inline
// literals, so nothing here reaches a filesystem.

#include "core/core_internal.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace scav::test {

inline scav_byte const *raw(std::string_view text) {
  return reinterpret_cast<scav_byte const *>(text.data());
}

inline uint32_t size32(std::string_view text) {
  return static_cast<uint32_t>(text.size());
}

struct Parsed {
  ParsedDocument pd;
  std::vector<Diagnostic> diags;
  bool ok;
};

inline Parsed parse(std::string_view text, std::string_view name = "test.scav") {
  Parsed r;
  r.ok = parse_document(raw(text),
                        size32(text),
                        name,
                        parse_default_options(),
                        r.pd,
                        r.diags);
  return r;
}

inline Parsed parse_deep(std::string_view text, uint32_t max_depth) {
  Parsed r;
  ParseOptions const opts{ .max_depth = max_depth };
  r.ok = parse_document(raw(text), size32(text), "test.scav", opts, r.pd, r.diags);
  return r;
}

struct Lexed {
  LexResult result;
  std::vector<Diagnostic> diags;
  bool ok;
  std::vector<scav_byte> bytes;
};

// Normalizes first, so a test's offsets mean the same thing they do in the
// parser.
inline Lexed lex_text(std::string_view text) {
  Lexed r;
  std::vector<Diagnostic> norm;
  DocId const doc{ 0 };
  r.ok = source_text_normalize(raw(text), size32(text), doc, r.bytes, norm);
  r.diags = norm;
  if (!r.ok) { return r; }
  r.ok = lex_source(r.bytes.data(),
                    static_cast<uint32_t>(r.bytes.size()),
                    doc,
                    r.result,
                    r.diags);
  return r;
}

// Canonical text for source. The parse is not asserted: some callers feed input
// the parser rejects, to see the printer walk what survived.
inline std::string print(std::string_view text, uint32_t columns = DEFAULT_PRINT_COLUMNS) {
  Parsed const r{ parse(text) };
  std::string out;
  PrintOptions const opts{ .columns = columns };
  print_document(r.pd, opts, out);
  return out;
}

// Canonical text prints as itself -- not merely that a second pass agrees with
// the first, which every input satisfies.
inline bool is_canonical(std::string_view text, uint32_t columns = DEFAULT_PRINT_COLUMNS) {
  return std::string{ text } == print(text, columns);
}

inline std::string_view str(ParsedDocument const &pd, StrRef ref) {
  return string_pool_view(pd.strings, ref);
}

inline std::string_view src(ParsedDocument const &pd, Span span) {
  if (span.len == 0) { return {}; }
  return { reinterpret_cast<char const *>(pd.src_bytes.data() + span.off), span.len };
}

// The first diagnostic's code, or Ok when there were none. Every error test
// asserts the code rather than the message, because the message is prose.
inline DiagCode first_code(std::vector<Diagnostic> const &diags) {
  return diags.empty() ? DiagCode::Ok : diags[0].code;
}

inline bool has_code(std::vector<Diagnostic> const &diags, DiagCode code) {
  for (Diagnostic const &d : diags) {
    if (d.code == code) { return true; }
  }
  return false;
}

// Statement rows of one kind, in document order.
inline std::vector<uint32_t> stmts_of(ParsedDocument const &pd, StmtKind kind) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < pd.stmts.size(); ++i) {
    if (pd.stmts[i].kind == kind) { out.push_back(i); }
  }
  return out;
}

inline StateStmt const &state_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.states[pd.stmt_payload[stmt]];
}

inline TransStmt const &trans_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.transitions[pd.stmt_payload[stmt]];
}

inline SubmachineStmt const &submachine_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.submachines[pd.stmt_payload[stmt]];
}

inline AttrStmt const &attr_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.attrs[pd.stmt_payload[stmt]];
}

inline IncludeStmt const &include_at(ParsedDocument const &pd, uint32_t stmt) {
  return pd.includes[pd.stmt_payload[stmt]];
}

// Model refs, spelled short so an assertion reads like its claim.
inline ElemRef ref(StateId id) { return { .kind = ElemKind::State, .ordinal = id.v }; }
inline ElemRef ref(SubmachineId id) {
  return { .kind = ElemKind::Submachine, .ordinal = id.v };
}
inline ElemRef ref(TransId id) {
  return { .kind = ElemKind::Transition, .ordinal = id.v };
}
inline ElemRef chart_ref() { return { .kind = ElemKind::Chart, .ordinal = 0 }; }

inline std::string path(Chart const &c, StateId id) {
  std::string out;
  chart_path_of(c, id, out);
  return out;
}

// A path spelled back out, so an endpoint assertion reads like the source did.
inline std::string path_text(ParsedDocument const &pd, Endpoint const &e) {
  if (e.wildcard != 0) { return "*"; }
  std::string out;
  for (uint32_t i = 0; i < e.segs.len; ++i) {
    PathSeg const &seg{ pd.path_segs[e.segs.off + i] };
    if (i != 0) { out.push_back('/'); }
    out += str(pd, seg.name);
    if (seg.qualifier.len != 0) {
      out.push_back(':');
      out += str(pd, seg.qualifier);
    } else if (seg.ordinal != INVALID) {
      out.push_back(':');
      out += std::to_string(seg.ordinal);
    }
  }
  return out;
}

// --- timing -----------------------------------------------------------------
//
// Shared by the three perf suites, which measured the same way in three copies.

inline uint64_t micros_since(std::chrono::steady_clock::time_point start) {
  auto const elapsed{ std::chrono::steady_clock::now() - start };
  uint64_t const us{ static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) };
  return (us == 0) ? 1U : us;  // a zero denominator says nothing useful
}

inline uint64_t nanos_since(std::chrono::steady_clock::time_point start) {
  auto const elapsed{ std::chrono::steady_clock::now() - start };
  uint64_t const ns{ static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) };
  return (ns == 0) ? 1U : ns;
}

inline constexpr uint32_t SCALING_RUNS{ 5 };
inline constexpr uint32_t SCALING_ATTEMPTS{ 3 };

// A throughput floor's estimator. Noise only adds time, so the fastest run is
// the one least of the machine and most of the code.
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

// A *ratio's* estimator, which no choice of sample makes safe at this duration.
// The scaling inputs run in hundreds of microseconds and a scheduler quantum is
// milliseconds, so one descheduled slice inflates a run by more than the slack a
// ratio allows -- a parse that is linear on an idle machine read 21x for 4x the
// bytes on a hosted runner. The median carries the steal it was measured with;
// min-of-N biases a ratio upward, because the shorter side finds an uncontended
// window more often than the longer one does.
//
// So the window is lengthened rather than the sample made clever: the work
// repeats inside one timed span until the span is tens of milliseconds. The
// ratio is untouched -- both sides are still one run's cost -- and a stolen
// millisecond goes from swamping the measurement to rounding it. Nanoseconds
// per run, so dividing by the repeat count keeps the resolution the division
// would otherwise spend.
inline constexpr uint64_t SCALING_WINDOW_NANOS{ 20'000'000 };

template <typename Once>
uint64_t nanos_per_run(Once &&once) {
  // The probe is also the warm-up, so a cold allocator is never a sample.
  auto const probe_start{ std::chrono::steady_clock::now() };
  once();
  uint64_t const probe{ nanos_since(probe_start) };
  uint64_t const reps{ (probe >= SCALING_WINDOW_NANOS) ? 1ULL
                                                       : (SCALING_WINDOW_NANOS / probe) };
  auto const start{ std::chrono::steady_clock::now() };
  for (uint64_t i = 0; i < reps; ++i) { once(); }
  uint64_t const total{ nanos_since(start) };
  uint64_t const each{ total / reps };
  return (each == 0) ? 1U : each;
}

// Both sides of a ratio, measured together and retried.
//
// A scheduling hiccup inflates one attempt; a quadratic inflates every one. So
// the pair kept is the one with the lowest growth, which noise can only make
// look better and a real quadratic cannot fake.
struct Pair {
  uint64_t small, large;
};

template <typename Small, typename Large>
Pair best_pair(Small &&small, Large &&large) {
  Pair best{ .small = 1, .large = UINT64_MAX };
  for (uint32_t attempt = 0; attempt < SCALING_ATTEMPTS; ++attempt) {
    Pair const got{ .small = nanos_per_run(small), .large = nanos_per_run(large) };
    if ((got.large * best.small) < (best.large * got.small)) { best = got; }
  }
  return best;
}

}  // namespace scav::test

#endif  // SCAV_CORE_TESTS_TEST_SUPPORT_H_INCLUDED
