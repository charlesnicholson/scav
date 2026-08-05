#include "core/lang/diagnostic.h"

#include "scav/types.h"

#include <cstdint>
#include <vector>

namespace scav {

LineCol line_col(scav_byte const *bytes, uint32_t len, uint32_t offset) {
  uint32_t const stop{ (offset < len) ? offset : len };
  uint32_t line{ 1 };
  uint32_t column{ 1 };
  for (uint32_t i = 0; i < stop; ++i) {
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

char const *diag_text(DiagCode code) {
  switch (code) {
    case DiagCode::Ok: return "ok";

    case DiagCode::Utf8Truncated: return "truncated UTF-8 sequence";
    case DiagCode::Utf8InvalidByte: return "byte cannot start a UTF-8 sequence";
    case DiagCode::Utf8Overlong: return "overlong UTF-8 encoding";
    case DiagCode::Utf8Surrogate: return "UTF-8 encoded a surrogate codepoint";
    case DiagCode::Utf8OutOfRange: return "codepoint above U+10FFFF";

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
  }
  return "unknown diagnostic";
}

bool has_errors(std::vector<Diagnostic> const &diags) {
  for (Diagnostic const &d : diags) {
    if (d.code != DiagCode::Ok) { return true; }
  }
  return false;
}

}  // namespace scav
