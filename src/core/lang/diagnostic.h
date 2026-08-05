#ifndef SCAV_CORE_LANG_DIAGNOSTIC_H_INCLUDED
#define SCAV_CORE_LANG_DIAGNOSTIC_H_INCLUDED

// A front-end diagnostic is a code plus a source span, and nothing else.
//
// PRD 6 says a *model* diagnostic carries only (code, subject_kind,
// subject_index) and derives its position by walking to the statement's src
// span. That works because a statement exists. A syntax error is reported
// before one does, so the span is the payload here -- and it is the same span
// the statement would have carried, into the same normalized bytes, so nothing
// downstream has to special-case it.
//
// Line and column are derived on demand rather than stored: a position that is
// never printed costs nothing, and no layer has to thread one through its call
// stack.

#include "core/model/ids.h"
#include "scav/types.h"

#include <cstdint>
#include <vector>

namespace scav {

// Constants are CamelCase here and in every other scav enum, though PRD 7
// sketches them lower_case: the pinned naming rules are mechanically enforced
// and the document is not. The DSL spelling of a StateKind is a table either
// way, so nothing is lost.
enum class DiagCode : uint32_t {
  Ok,

  // Normalization -- spans index the *raw* input, since the normalized buffer
  // does not exist yet when these fire.
  Utf8Truncated,
  Utf8InvalidByte,
  Utf8Overlong,
  Utf8Surrogate,
  Utf8OutOfRange,

  // Lexing.
  UnexpectedCharacter,
  UnterminatedString,
  UnterminatedRawString,
  ExpectedArrow,

  // String literal decoding.
  UnknownEscape,
  TruncatedEscape,
  InvalidHexEscape,
  EscapedSurrogate,
  NewlineInString,
  RawStringUnderIndented,

  // Parsing.
  ExpectedChart,
  ExpectedIdentifier,
  ExpectedString,
  ExpectedToken,
  ExpectedItem,
  ExpectedSeparator,
  ExpectedBlock,
  ExpectedEndpoint,
  ExpectedValue,
  UnknownStateKind,
  ReservedWordAsName,
  NumberOutOfRange,
  TrailingContent,
  DepthLimitExceeded,
};

struct Diagnostic {
  DiagCode code;
  DocId doc;
  Span src;
};

// One-based, and the column counts codepoints rather than bytes -- a column of
// 4 should mean the fourth character. Offsets index normalized bytes (PRD 7), so
// both numbers are stable across platforms.
struct LineCol {
  uint32_t line, column;
};

LineCol line_col(scav_byte const *bytes, uint32_t len, uint32_t offset);

// A short, stable, locale-free description. Not a formatted message: rendering
// one is the application's, and PRD 4 keeps stream globals out of a library.
char const *diag_text(DiagCode code);

bool has_errors(std::vector<Diagnostic> const &diags);

}  // namespace scav

#endif  // SCAV_CORE_LANG_DIAGNOSTIC_H_INCLUDED
