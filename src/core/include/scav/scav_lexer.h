#ifndef SCAV_LEXER_H_INCLUDED
#define SCAV_LEXER_H_INCLUDED

// Everything here is named `lex_*`, after the header.
//
// Bytes in, one flat token vector out. Not a pull or callback lexer: PRD 17
// asks for a throughput floor on lexing and on parsing *separately*, which needs a
// materialized intermediate, and the parser's few lookahead sites read better as
// tokens[i + 1] than as a saved-token slot. The vector is scratch -- it is freed
// once the parse finishes, and the model keeps only src_bytes.
//
// Every span indexes the normalized bytes (PRD 6), so a token's off/len can be
// handed straight to a diagnostic.

#include "scav/scav_diagnostics.h"
#include "scav/scav_ids.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

// No keyword kinds. Reserved words are recognized by the parser from the token
// bytes, because `s`, `m` and `t` are keywords only in statement-leading
// position and a lexer that decided would have to know where it was.
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

// Trivia. Comments are lexed and never parsed, so they travel beside the token
// stream rather than in it. The two flags are what the position classification
// needs and all it needs: whether the comment shares its line with code, and
// whether a blank line separates it from what follows.
struct LexComment {
  Span src;              // includes the "//", excludes the newline
  uint32_t code_before;  // non-whitespace earlier on the same line
  uint32_t blank_after;  // a blank line, or the end of input, follows
};

struct LexResult {
  std::vector<Token> tokens;
  std::vector<LexComment> comments;
};

// Recovers from an unexpected character by skipping it, so a file with several
// stray bytes reports all of them. Stops at anything else -- an unterminated
// string has no recovery that is not a guess. Returns false when it reported
// anything.
bool lex_source(scav_byte const *bytes,
                uint32_t len,
                DocId doc,
                LexResult &out,
                std::vector<Diagnostic> &diags);

char const *lex_token_kind_name(TokKind kind);

// Reserved in every position (PRD 15). `choice`, `history`, `as`, `kind`, `s`,
// `m` and `t` are deliberately absent: they are contextual, so a state may be
// named any of them.
bool lex_is_reserved_word(std::string_view text);

// The lexeme includes its delimiters. Applies escapes for `"..."`, strips
// indentation to the closing delimiter's column for `"""..."""`, and NFC-folds
// the result -- a \u escape can otherwise put a decomposed sequence in the
// string pool that no byte of the source contained.
bool lex_decode_string_literal(scav_byte const *bytes,
                               Span lexeme,
                               DocId doc,
                               std::vector<scav_byte> &out,
                               std::vector<Diagnostic> &diags);

// Bytes held by the token stream, for the memory-ratio assertion in the
// performance tests. Capacity rather than size: the peak is what matters.
uint64_t lex_footprint(LexResult const &result);

}  // namespace scav

#endif  // SCAV_LEXER_H_INCLUDED
