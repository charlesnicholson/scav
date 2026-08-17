#ifndef SCAV_CORE_CORE_INTERNAL_H_INCLUDED
#define SCAV_CORE_CORE_INTERNAL_H_INCLUDED

// The front end behind parse_document, and the helpers core's layers share.
// Private by location: nothing outside src/ can reach this path.

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scav {

// Narrowing =================================================================

// size_t stops at the API boundary: an entry point takes one, narrows it, and
// hands uint32_t inward.

// True when `value` round-trips into T, leaving `out` written only then.
template <typename T, typename U>
bool narrow(U value, T &out) {
  static_assert(std::is_unsigned_v<T> && std::is_unsigned_v<U>,
                "scav narrows unsigned to unsigned; a signed length is a bug upstream");
  if (value > static_cast<U>(std::numeric_limits<T>::max())) { return false; }
  out = static_cast<T>(value);
  return true;
}

// For a bound already known to hold. Clamps rather than wrapping, so a mistake
// is a short read and not a 4-gigabyte one.
template <typename T, typename U>
T narrow_clamp(U value) {
  static_assert(std::is_unsigned_v<T> && std::is_unsigned_v<U>, "unsigned only");
  constexpr U LIMIT{ static_cast<U>(std::numeric_limits<T>::max()) };
  return static_cast<T>((value > LIMIT) ? LIMIT : value);
}

// String pool ===============================================================

// Appends as met, never deduplicates. The empty string returns the empty ref
// and grows the pool by nothing.
StrRef string_pool_add(StringPool &pool, std::string_view text);

// Source text ===============================================================

// BOM stripped, line endings folded to LF, UTF-8 validated, NFC applied.
// Offsets downstream index these bytes; a UTF-8 error indexes the raw input.

// Returns false and leaves `out` empty when the input is not valid UTF-8.
// `out` is cleared first either way.
bool source_text_normalize(scav_byte const *bytes,
                           size_t len,
                           DocId doc,
                           std::vector<scav_byte> &out,
                           std::vector<Diagnostic> &diags);

// Codepoint at a time, for callers working on decoded escapes rather than on a
// whole document.
bool source_text_utf8_decode(scav_byte const *bytes,
                             size_t len,
                             size_t at,
                             uint32_t &cp,
                             uint32_t &width,
                             DiagCode &err);
void source_text_utf8_encode(uint32_t cp, std::vector<scav_byte> &out);

// True when no codepoint could move under NFC. Byte-scans the ASCII run.
bool source_text_is_nfc(scav_byte const *bytes, size_t len);

// Assumes valid UTF-8; `out` is cleared first. Returns true when it changed
// something.
bool source_text_to_nfc(scav_byte const *bytes, size_t len, std::vector<scav_byte> &out);

// Assumes already-normalized bytes.
bool source_text_is_ascii(scav_byte const *bytes, size_t len);

std::string_view source_text_view(std::vector<scav_byte> const &bytes, Span span);

// Lexer =====================================================================

// Bytes in, one flat token vector out. The vector is scratch, freed after the
// parse.

// No keyword kinds. `s`, `m` and `t` are keywords only in statement-leading
// position, which the lexer cannot see from where it stands.
enum class TokKind : uint32_t {
  End,  // one sentinel, always last, so lookahead needs no bounds check

  Ident,
  Number,
  String,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Equals,
  At,
  Colon,
  Slash,
  Star,
  Arrow,
};

// 12 bytes, no padding.
struct Token {
  uint32_t off, len;
  TokKind kind;
};

// Comments travel beside the token stream rather than in it. The two flags are
// what the printer needs to classify a comment's position.
struct LexComment {
  Span src;              // includes the "//", excludes the newline
  uint32_t code_before;  // non-whitespace earlier on the same line
  uint32_t blank_after;  // a blank line, or the end of input, follows
};

struct LexResult {
  std::vector<Token> tokens;
  std::vector<LexComment> comments;
};

// Skips an unexpected character and keeps going, so one run reports all of
// them; stops at anything else. Returns false when it reported anything.
bool lex_source(scav_byte const *bytes,
                size_t byte_count,
                DocId doc,
                LexResult &out,
                std::vector<Diagnostic> &diags);

char const *lex_token_kind_name(TokKind kind);

// Reserved in every position. `choice`, `history`, `as`, `kind`, `s`, `m` and
// `t` are absent: they are contextual, so a state may be named one.
bool lex_is_reserved_word(std::string_view text);

// `lexeme` includes its delimiters. Applies escapes, dedents a raw string to
// its closing column, and NFC-folds, since a \u escape can decompose.
bool lex_decode_string_literal(scav_byte const *bytes,
                               Span lexeme,
                               DocId doc,
                               std::vector<scav_byte> &out,
                               std::vector<Diagnostic> &diags);

// Parser ====================================================================

// LL(1) descent with the frames in a heap vector rather than the call stack, so
// a hostile nesting depth is a diagnostic instead of a stack overflow.

// Bytes must already be normalized. `name` is what a diagnostic quotes.
bool parse_tokens(scav_byte const *bytes,
                  size_t byte_count,
                  LexResult const &lexed,
                  DocId doc,
                  std::string_view name,
                  ParseOptions const &opts,
                  ParsedDocument &out,
                  std::vector<Diagnostic> &diags);

// The chart statement, always row zero. INVALID when nothing parsed.
uint32_t syntax_root_statement(ParsedDocument const &pd);

// Lowering ==================================================================

// One document into an empty chart: rebases pd's front-end slice, creates its
// entity rows, leaves every include unresolved. False if it reported anything.
bool lower_document(Chart &c, ParsedDocument const &pd, std::vector<Diagnostic> &diags);

// Footprint =================================================================

// Bytes held, summing capacity rather than size.
uint64_t lex_footprint(LexResult const &result);
uint64_t parse_footprint(ParsedDocument const &pd);
uint64_t chart_footprint(Chart const &c);

}  // namespace scav

#endif  // SCAV_CORE_CORE_INTERNAL_H_INCLUDED
