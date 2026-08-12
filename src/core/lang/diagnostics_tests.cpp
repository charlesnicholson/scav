// diagnostics.cpp: the code table, the error predicate, and line/column from an
// offset. Compiled against scavcore_testable.

#include "scav/scav_core.h"

#include "core/test_support.h"

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
  // Exhaustive, so adding a code without a message fails here rather than
  // shipping "unknown diagnostic" to a user.
  for (uint32_t i = 0; i <= static_cast<uint32_t>(DiagCode::DepthLimitExceeded); ++i) {
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
  // An offset inside a character reports the next column, because the lead byte
  // is already behind it. Spans come from the lexer and start on a boundary, so
  // this is pinned rather than relied on.
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
