// diagnostics.cpp: the code table, the error predicate, and line/column from an
// offset. Compiled against scavcore_testable.

#include "scav/scav_core.h"

#include "core/tests/test_support.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace {

using namespace scav;
using namespace scav::test;

LineCol at(std::string_view text, size_t offset) {
  return diag_line_col(raw(text), text.size(), offset);
}

}  // namespace

TEST_CASE("diag: every code has its own message") {
  // Exhaustive up to the last enumerator, so adding a code without a message
  // fails here rather than shipping "unknown diagnostic" to a user. A new code
  // is appended past the last one, and this bound moves with it.
  for (uint32_t i = 0; i <= static_cast<uint32_t>(DiagCode::RouteDegraded); ++i) {
    std::string const message{ diag_message(static_cast<DiagCode>(i)) };
    CHECK(message != "unknown diagnostic");
    CHECK_FALSE(message.empty());
  }
  CHECK(std::string{ diag_message(static_cast<DiagCode>(9999)) } == "unknown diagnostic");
}

TEST_CASE("diag: has_errors is false only when nothing but Ok is present") {
  CHECK_FALSE(diag_has_errors({}));
  CHECK_FALSE(diag_has_errors({ { .code = DiagCode::Ok, .doc = {}, .src = {} } }));
  CHECK(diag_has_errors({ { .code = DiagCode::ExpectedChart, .doc = {}, .src = {} } }));
  // Any non-Ok anywhere, not just the first row.
  CHECK(diag_has_errors({ { .code = DiagCode::Ok, .doc = {}, .src = {} },
                          { .code = DiagCode::ExpectedBlock, .doc = {}, .src = {} } }));
}

TEST_CASE("diag: line and column are one-based") {
  LineCol const first{ at("abc", 0) };
  CHECK(first.line == 1);
  CHECK(first.column == 1);
  CHECK(at("abc", 2).column == 3);
}

TEST_CASE("diag: a newline advances the line and resets the column") {
  std::string_view const text{ "ab\ncd\n\nx" };
  CHECK(at(text, 3).line == 2);
  CHECK(at(text, 3).column == 1);
  CHECK(at(text, 4).column == 2);
  // The blank line is its own line, so the next character is line 4.
  CHECK(at(text, 7).line == 4);
  CHECK(at(text, 7).column == 1);
}

TEST_CASE("diag: the column counts characters, not bytes") {
  // Continuation bytes are the tail of a character already counted, or every
  // non-ASCII label would report a column past where it visibly sits.
  std::string_view const text{ "\xc3\xa9\xc3\xa9x" };  // e-acute, e-acute, x
  CHECK(at(text, 4).column == 3);
  CHECK(at(text, 2).column == 2);
  // An offset inside a character reports the next column. Lexer spans start on
  // a boundary, so this is pinned rather than relied on.
  CHECK(at(text, 1).column == 2);
}

TEST_CASE("diag: an offset past the end clamps to the end") {
  // A diagnostic at EOF is ordinary -- "unterminated string" points there -- so
  // this reports the last position rather than walking off the buffer.
  std::string_view const text{ "ab\ncd" };
  CHECK(at(text, 99).line == 2);
  CHECK(at(text, 99).column == 3);
  CHECK(at("", 0).line == 1);
  CHECK(at("", 5).column == 1);
}

// Number formatting =========================================================

TEST_CASE("diag: an integer formats without a locale reaching it") {
  std::string out;
  string_append_u32(out, 0);
  CHECK(out == "0");
  out.clear();
  string_append_u32(out, 7);
  CHECK(out == "7");
  out.clear();
  string_append_u32(out, 1'000'000);
  CHECK(out == "1000000");  // never a thousands separator
  out.clear();
  string_append_u32(out, 4'294'967'295U);
  CHECK(out == "4294967295");
  out += '!';
  string_append_u32(out, 12);
  CHECK(out == "4294967295!12");  // appends, never assigns
}

TEST_CASE("diag: a hash is always eight lowercase hex digits") {
  std::string out;
  string_append_hex32(out, 0);
  CHECK(out == "00000000");
  out.clear();
  string_append_hex32(out, 0xDEAD'BEEFU);
  CHECK(out == "deadbeef");
  out.clear();
  string_append_hex32(out, 1);
  CHECK(out == "00000001");
}

// Rendering =================================================================

namespace {

// One document loaded from memory, which is all a diagnostic needs to be
// positioned against.
struct Rendered {
  Loader loader;
  Chart chart;
  std::vector<Diagnostic> diags;
};

void load_one(Rendered &r, std::string_view text, std::string_view name) {
  load_add(r.loader, raw(text), text.size(), name);
  load_finish(r.loader, r.chart, r.diags);
}

}  // namespace

TEST_CASE("diag: a model finding names the document holding its statement") {
  Rendered r;
  load_one(r, "chart c {\n  state A,\n  state A,\n}\n", "dup.scav");
  REQUIRE_FALSE(r.chart.documents.empty());
  std::vector<Diagnostic> found;
  CHECK_FALSE(validate_chart(r.chart, found));
  REQUIRE_FALSE(found.empty());

  std::string out;
  diag_append(out, r.chart, found[0], "cmdline.scav");
  CHECK(out == "dup.scav:3:3: duplicate name within a submachine\n");
}

TEST_CASE("diag: a finding with no position at all still names something") {
  Rendered r;
  load_one(r, "chart c {\n  state A,\n}\n", "solo.scav");
  Diagnostic const bare{ .code = DiagCode::LoaderEmpty,
                         .subject = { .kind = ElemKind::None, .ordinal = INVALID },
                         .doc = { 0 },
                         .src = {} };
  std::string out;
  diag_append(out, r.chart, bare, "cmdline.scav");
  CHECK(out ==
        std::string{ "cmdline.scav: " } + diag_message(DiagCode::LoaderEmpty) + "\n");
}

TEST_CASE("diag: a loader finding is positioned against the bytes it still holds") {
  Loader loader;
  std::string_view const text{ "chart c { state , }\n" };
  CHECK_FALSE(load_add(loader, raw(text), text.size(), "broken.scav"));
  REQUIRE_FALSE(loader.diags.empty());

  std::string out;
  diag_append(out, loader, loader.diags[0], "cmdline.scav");
  CHECK(out.starts_with("broken.scav:1:"));
  CHECK(out.ends_with("\n"));
}

TEST_CASE("diag: an unnamed document falls back to the name the caller supplied") {
  Loader const loader;
  Diagnostic const d{ .code = DiagCode::LoaderEmpty,
                      .subject = { .kind = ElemKind::None, .ordinal = INVALID },
                      .doc = { INVALID },
                      .src = {} };
  std::string out;
  diag_append(out, loader, d, "cmdline.scav");
  CHECK(out ==
        std::string{ "cmdline.scav: " } + diag_message(DiagCode::LoaderEmpty) + "\n");
}

TEST_CASE("diag: rendering appends, so a run of findings is one string") {
  Rendered r;
  load_one(r, "chart c {\n  state A,\n  state A,\n  state A,\n}\n", "dups.scav");
  std::vector<Diagnostic> found;
  CHECK_FALSE(validate_chart(r.chart, found));
  std::string out;
  for (Diagnostic const &d : found) { diag_append(out, r.chart, d, "cmdline.scav"); }
  uint32_t lines{ 0 };
  for (char const ch : out) { lines += (ch == '\n') ? 1U : 0U; }
  CHECK(lines == found.size());
}

TEST_CASE("diag: a finding on any subject kind walks to its own statement") {
  Rendered r;
  load_one(r,
           "chart c {\n"
           "  state On {\n"
           "    submachine m {\n"
           "      state A,\n"
           "    },\n"
           "  },\n"
           "  trans On:m/A -> On:m/A,\n"
           "}\n",
           "kinds.scav");
  REQUIRE(r.diags.empty());
  StateId a{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "On:m/A", a) ==
          ResolveStatus::Ok);
  SubmachineId const m{ r.chart.states[a.v].parent };
  REQUIRE(r.chart.transitions.size() == 1);

  auto const rendered = [&](ElemRef subject) {
    std::string out;
    diag_append(out,
                r.chart,
                { .code = DiagCode::DanglingRef,
                  .subject = subject,
                  .doc = { INVALID },
                  .src = {} },
                "cmdline.scav");
    return out;
  };
  std::string const tail{ std::string{ ": " } + diag_message(DiagCode::DanglingRef) +
                          "\n" };
  CHECK(rendered(ref(a)) == "kinds.scav:4:7" + tail);
  CHECK(rendered(ref(m)) == "kinds.scav:3:5" + tail);
  CHECK(rendered(ref(TransId{ 0 })) == "kinds.scav:7:3" + tail);
  // The chart entity's statement is the root submachine's, which is the chart
  // line itself.
  CHECK(rendered(chart_ref()) == "kinds.scav:1:1" + tail);
}

TEST_CASE("diag: a chart finding with no root submachine falls back to the name") {
  Rendered r;
  load_one(r, "chart c {\n  state A,\n}\n", "solo.scav");
  r.chart.root_submachine = SubmachineId{ 99 };
  std::string out;
  diag_append(out,
              r.chart,
              { .code = DiagCode::DanglingRef,
                .subject = chart_ref(),
                .doc = { INVALID },
                .src = {} },
              "cmdline.scav");
  CHECK(out ==
        std::string{ "cmdline.scav: " } + diag_message(DiagCode::DanglingRef) + "\n");
}

TEST_CASE("diag: a finding that already carries a span is positioned by that span") {
  // A producer running before the entity existed fills `src`, so the reader must
  // not walk to the subject and report the statement's position instead.
  Rendered r;
  load_one(r, "chart c {\n  state A,\n}\n", "solo.scav");
  StateId a{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "A", a) == ResolveStatus::Ok);
  std::string out;
  // The state's own statement begins at 2:3; the span names 2:6.
  diag_append(out,
              r.chart,
              { .code = DiagCode::DanglingRef,
                .subject = ref(a),
                .doc = { 0 },
                .src = make_span(15, 1) },
              "cmdline.scav");
  CHECK(out ==
        std::string{ "solo.scav:2:6: " } + diag_message(DiagCode::DanglingRef) + "\n");
}

TEST_CASE("diag: a statement span cleared by mutation degrades to the triple") {
  Rendered r;
  load_one(r, "chart c {\n  state A,\n}\n", "solo.scav");
  StateId a{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "A", a) == ResolveStatus::Ok);
  StmtId const stmt{ r.chart.states[a.v].stmt };
  REQUIRE(stmt.v < r.chart.stmts.size());

  Diagnostic const d{ .code = DiagCode::DanglingRef,
                      .subject = ref(a),
                      .doc = { INVALID },
                      .src = {} };
  std::string located;
  diag_append(located, r.chart, d, "cmdline.scav");
  CHECK(located ==
        std::string{ "solo.scav:2:3: " } + diag_message(DiagCode::DanglingRef) + "\n");

  r.chart.stmts[stmt.v].src = {};  // what a mutation leaves behind
  std::string bare;
  diag_append(bare, r.chart, d, "cmdline.scav");
  CHECK(bare ==
        std::string{ "cmdline.scav: " } + diag_message(DiagCode::DanglingRef) + "\n");
}

TEST_CASE("diag: an unnamed document still positions, under the caller's name") {
  // A chart built from a buffer nobody named keeps its offsets, so the line and
  // column survive even though the document has no path to quote.
  Rendered r;
  load_one(r, "chart c {\n  state A,\n}\n", "solo.scav");
  REQUIRE(r.chart.documents.size() == 1);
  r.chart.documents[0].path = {};
  StateId a{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "A", a) == ResolveStatus::Ok);
  std::string out;
  diag_append(
      out,
      r.chart,
      { .code = DiagCode::DanglingRef, .subject = ref(a), .doc = { INVALID }, .src = {} },
      "<buffer>");
  CHECK(out ==
        std::string{ "<buffer>:2:3: " } + diag_message(DiagCode::DanglingRef) + "\n");
}
