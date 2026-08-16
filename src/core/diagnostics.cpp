// The code table over every subsystem's diagnostics. A diagnostic carries a
// code and either a span or an entity; position is derived from an offset.

#include "scav/scav_core.h"

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scav {

LineCol diag_line_col(scav_byte const *bytes, size_t len, size_t offset) {
  size_t const stop{ (offset < len) ? offset : len };
  uint32_t line{ 1 };
  uint32_t column{ 1 };
  for (size_t i = 0; i < stop; ++i) {
    if (bytes[i] == '\n') {
      ++line;
      column = 1;
      continue;
    }
    // A UTF-8 continuation byte is the tail of a character already counted.
    if ((bytes[i] & 0xC0U) != 0x80U) { ++column; }
  }
  return { .line = line, .column = column };
}

char const *diag_message(DiagCode code) {
  switch (code) {
    case DiagCode::Ok: return "ok";

    case DiagCode::DocumentTooLarge:
      return "document is larger than 4 GiB, which a span cannot address";
    case DiagCode::Utf8Truncated: return "truncated UTF-8 sequence";
    case DiagCode::Utf8InvalidByte: return "byte cannot start a UTF-8 sequence";
    case DiagCode::Utf8Overlong: return "overlong UTF-8 encoding";
    case DiagCode::Utf8Surrogate: return "UTF-8 encoded a surrogate codepoint";
    case DiagCode::Utf8OutOfRange: return "codepoint above U+10FFFF";

    case DiagCode::BlockCommentUnsupported:
      return "block comments are not supported; use // to the end of the line";
    case DiagCode::UnexpectedCharacter: return "unexpected character";
    case DiagCode::UnterminatedString: return "unterminated string";
    case DiagCode::UnterminatedRawString: return "unterminated raw string";
    case DiagCode::ExpectedArrow: return "expected '->'";

    case DiagCode::UnknownEscape: return "unknown escape sequence";
    case DiagCode::TruncatedEscape: return "escape sequence ends the string";
    case DiagCode::InvalidHexEscape: return "\\u needs four hex digits";
    case DiagCode::EscapedSurrogate:
      return "\\u cannot name a surrogate; write the character directly";
    case DiagCode::NewlineInString:
      return R"(newline in a quoted string; use \n or a """ string)";
    case DiagCode::RawStringUnderIndented:
      return "raw string line is indented less than its closing delimiter";

    case DiagCode::ExpectedChart: return "expected 'chart'";
    case DiagCode::ExpectedIdentifier: return "expected an identifier";
    case DiagCode::ExpectedString: return "expected a string";
    case DiagCode::ExpectedToken: return "expected a different token here";
    case DiagCode::ExpectedItem:
      return "expected 'include', 'state', 'submachine', 'trans' or '@'";
    case DiagCode::ExpectedSeparator: return "expected ',' or '}'";
    case DiagCode::ExpectedBlock: return "expected '{'";
    case DiagCode::ExpectedEndpoint: return "expected '*' or a state path";
    case DiagCode::ExpectedValue: return "expected a string or a '[' list";
    case DiagCode::UnknownStateKind: return "unknown state kind";
    case DiagCode::ReservedWordAsName: return "reserved word used as a name";
    case DiagCode::NumberOutOfRange: return "number does not fit in 32 bits";
    case DiagCode::TrailingContent: return "content after the end of the chart";
    case DiagCode::DepthLimitExceeded: return "nesting is deeper than the limit";

    case DiagCode::IncludePathInvalid: return "path names no document";
    case DiagCode::IncludePathUnresolved:
      return "include path was never supplied to the load session";
    case DiagCode::IncludeCycle: return "include cycle";
    case DiagCode::IncludeExpansionTooLarge:
      return "the include graph expands to more instantiations than the limit";
    case DiagCode::DocumentNotRequested:
      return "document was not on the load session's pending list";
    case DiagCode::DocumentAlreadyLoaded: return "document was already added";
    case DiagCode::LoadSessionEmpty:
      return "load session has no root document, or the chart is not empty";

    case DiagCode::MisplacedStatement: return "statement is not permitted in this block";
    case DiagCode::WildcardBothEndpoints: return "a transition cannot run from '*' to '*'";
    case DiagCode::EndpointUnresolved: return "endpoint path names no state";
    case DiagCode::BadSubmachineQualifier:
      return "submachine qualifier names no submachine, or qualifies nothing";
    case DiagCode::EndpointCrossesInclude:
      return "endpoint path descends into an unresolved include";

    case DiagCode::DanglingRef: return "reference to a row that does not exist";
    case DiagCode::MissingRequiredId: return "required reference is absent";
    case DiagCode::TombstonedTarget: return "reference to a deleted row";
    case DiagCode::DuplicateName: return "duplicate name within a submachine";
    case DiagCode::MultipleInitial: return "more than one initial state in a submachine";
    case DiagCode::NameHasMetacharacter:
      return "name contains a path metacharacter: / : $ @";
    case DiagCode::StatementSpanOutOfRange:
      return "statement span lands outside its document";
    case DiagCode::ColumnCountMismatch:
      return "column row count does not match its entity array";
  }
  return "unknown diagnostic";
}

bool diag_has_errors(std::vector<Diagnostic> const &diags) {
  for (Diagnostic const &d : diags) {
    if (d.code != DiagCode::Ok) { return true; }
  }
  return false;
}

}  // namespace scav
