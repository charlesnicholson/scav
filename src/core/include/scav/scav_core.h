#ifndef SCAV_CORE_H_INCLUDED
#define SCAV_CORE_H_INCLUDED

// libscavcore's entire public API. One header per library (PRD 3.3), so there is
// never a question of which one to include -- there is one.
//
// P0 ships the front end: bytes in, a statement stream out. Read a `.scav`
// document with parse_document() and walk ParsedDocument; everything else here
// is either a piece that call is built from or a way to inspect what it
// produced.
//
// Every function carries the prefix of the section it lives in, so a symbol
// names its neighbourhood:
//
//   (none)          ids, spans and the INVALID sentinel
//   narrow*         the one checked narrowing helper (PRD 6)
//   diag_*          diagnostic codes, and line/column from a span
//   string_*        reading a finalized string pool
//   source_text_*   normalization on read: BOM, line endings, UTF-8, NFC
//   lex_*           tokens, trivia, string-literal decoding
//   syntax_*        the statement rows and their spellings
//   parse_*         the entry points most callers want
//
// Nothing here takes a path. Parsing takes bytes; acquiring bytes is a different
// system (PRD 16.2).

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scav {

// Ids and spans =============================================================

// Strong id types and the two span shapes every column indexes through. P0 owns
// only the front-end slice of PRD 7 -- documents, statements and the string
// pool -- so the entity ids land with the model spine in P1.

// PRD 7 spells this `kInvalid`; the pinned naming rules make a constant
// UPPER_CASE, and a rule that is mechanically enforced wins over one that is
// not.
constexpr uint32_t INVALID{ 0xFFFF'FFFFU };

struct DocId {
  uint32_t v;
};
struct StmtId {
  uint32_t v;
};

constexpr bool operator==(DocId a, DocId b) { return a.v == b.v; }
constexpr bool operator!=(DocId a, DocId b) { return a.v != b.v; }
constexpr bool operator==(StmtId a, StmtId b) { return a.v == b.v; }
constexpr bool operator!=(StmtId a, StmtId b) { return a.v != b.v; }

// Into StringPool::bytes.
struct StrRef {
  uint32_t off, len;
};

// Into a side array.
struct Span {
  uint32_t off, len;
};

constexpr bool operator==(StrRef a, StrRef b) {
  return (a.off == b.off) && (a.len == b.len);
}
constexpr bool operator!=(StrRef a, StrRef b) { return !(a == b); }
constexpr bool operator==(Span a, Span b) { return (a.off == b.off) && (a.len == b.len); }
constexpr bool operator!=(Span a, Span b) { return !(a == b); }

// Named constructors, because these two are built more than anything else in the
// front end and a designated-initializer list at every site reads worse than the
// call does. An empty span is `{}`.
constexpr StrRef str_ref(uint32_t off, uint32_t len) { return { .off = off, .len = len }; }
constexpr Span make_span(uint32_t off, uint32_t len) { return { .off = off, .len = len }; }

// Narrowing =================================================================

// PRD 6 bans narrowing without an explicit range check and names one helper for
// it, checked in every build. This is that helper.
//
// It exists because of the split between the two integer worlds scav lives in.
// A caller measures a buffer with `size_t` -- that is what the type is for, and
// `std::string_view::size()` returns one. The *model* indexes everything with
// `uint32_t`, because `Span` and `StrRef` are `{uint32 off, len}` at the C ABI
// (PRD 16) and because PRD 6 bans `size_t` from value computation outright:
// it is 32 bits on wasm32 and 64 elsewhere, so an expression over one silently
// gives a different correct answer per platform.
//
// So `size_t` stops at the front door. Public entry points take one, check it
// here, and hand a `uint32_t` inward. The check is the only place the two worlds
// touch.

// True when `value` round-trips into T. No exceptions and no assert: the caller
// gets a bool and decides, which for a document that is too large means a
// diagnostic rather than a crash.
template <typename T, typename U>
bool narrow(U value, T &out) {
  static_assert(std::is_unsigned_v<T> && std::is_unsigned_v<U>,
                "scav narrows unsigned to unsigned; a signed length is a bug upstream");
  if (value > static_cast<U>(std::numeric_limits<T>::max())) { return false; }
  out = static_cast<T>(value);
  return true;
}

// For the places where the bound is already known to hold and saying so is
// clearer than a branch -- a span's length against the document it came from,
// say. Still checked: it clamps rather than wrapping, so a mistake is a short
// read instead of a 4-gigabyte one.
template <typename T, typename U>
T narrow_clamp(U value) {
  static_assert(std::is_unsigned_v<T> && std::is_unsigned_v<U>, "unsigned only");
  constexpr U LIMIT{ static_cast<U>(std::numeric_limits<T>::max()) };
  return static_cast<T>((value > LIMIT) ? LIMIT : value);
}

// Diagnostics ===============================================================

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

// Constants are CamelCase here and in every other scav enum, though PRD 7
// sketches them lower_case: the pinned naming rules are mechanically enforced
// and the document is not. The DSL spelling of a StateKind is a table either
// way, so nothing is lost.
enum class DiagCode : uint32_t {
  Ok,

  // A caller handed in more bytes than a Span can address. Spans are
  // {uint32 off, len} at the ABI (PRD 16), so 4 GiB is the document ceiling,
  // and saying so beats truncating into a document that parses differently.
  DocumentTooLarge,

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

LineCol diag_line_col(scav_byte const *bytes, size_t len, size_t offset);

// A short, stable, locale-free description. Not a formatted message: rendering
// one is the application's, and PRD 4 keeps stream globals out of a library.
char const *diag_message(DiagCode code);

bool diag_has_errors(std::vector<Diagnostic> const &diags);

// String pool ===============================================================

// The finalized string pool and the two ways to read one.
//
// A StrRef is an offset and a length into StringPool::bytes. The pool is not
// NUL-terminated and never will be: PRD 16 hands strings out as spans, so a
// caller that wants a C string makes one itself.
//
// How a pool is *built* is not here -- see model/interner.h, which is private
// because interning is a parse-time concern and a client only ever reads.

struct StringPool {
  std::vector<scav_byte> bytes;
};

// A zero-length ref reads as the empty string without touching `pool`.
std::string_view string_pool_view(StringPool const &pool, StrRef ref);

// Byte-wise, unsigned, shorter-prefix-first. PRD 6: collation is byte-wise only,
// so Hebrew and Arabic sort in codepoint order and that is an accepted trade.
// Exposed because it is the order the pool is finalized in, and anything
// comparing names has to agree with it.
int string_compare_bytes(scav_byte const *a, size_t alen, scav_byte const *b, size_t blen);

// Source text ===============================================================

// Text is normalized on read (PRD 6): BOM stripped, line endings folded to LF,
// UTF-8 validated, NFC applied. Without this, core.autocrlf on Windows and NFD
// on macOS put different bytes in the string pool from the same commit, and the
// format hash follows the pool.
//
// Every offset downstream -- Statement.src, every diagnostic span -- indexes the
// *normalized* bytes, which is what makes a reported line and column the same
// number on every platform. The exception is the diagnostics this file itself
// produces: there is no normalized buffer yet when a UTF-8 error fires, so those
// spans index the raw input.

// Returns false and leaves `out` empty when the input is not valid UTF-8.
// `out` is cleared first either way.
bool source_text_normalize(scav_byte const *bytes,
                           size_t len,
                           DocId doc,
                           std::vector<scav_byte> &out,
                           std::vector<Diagnostic> &diags);

// The pieces, exposed because they are separately testable and because the
// string-literal decoder needs the last two on decoded escapes rather than on a
// whole document.
bool source_text_utf8_decode(scav_byte const *bytes,
                             size_t len,
                             size_t at,
                             uint32_t &cp,
                             uint32_t &width,
                             DiagCode &err);
void source_text_utf8_encode(uint32_t cp, std::vector<scav_byte> &out);

// True when the bytes contain no codepoint that NFC could move. Byte-scans the
// ASCII run, which is the whole of a typical chart.
bool source_text_is_nfc(scav_byte const *bytes, size_t len);

// Assumes valid UTF-8; `out` is cleared first. Returns true when it changed
// something.
bool source_text_to_nfc(scav_byte const *bytes, size_t len, std::vector<scav_byte> &out);

// LF-only and no BOM, without the UTF-8 or NFC passes. Exposed for the raw
// string decoder, which works on bytes that are already normalized.
bool source_text_is_ascii(scav_byte const *bytes, size_t len);

std::string_view source_text_view(std::vector<scav_byte> const &bytes, Span span);

// Lexer =====================================================================

// Bytes in, one flat token vector out. Not a pull or callback lexer: PRD 17
// asks for a throughput floor on lexing and on parsing *separately*, which needs a
// materialized intermediate, and the parser's few lookahead sites read better as
// tokens[i + 1] than as a saved-token slot. The vector is scratch -- it is freed
// once the parse finishes, and the model keeps only src_bytes.
//
// Every span indexes the normalized bytes (PRD 6), so a token's off/len can be
// handed straight to a diagnostic.

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
                size_t byte_count,
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

// Syntax tree ===============================================================

// Functions here are named `syntax_*`; the POD rows keep the names PRD 7 gives
// them.
//
// The front-end slice of PRD 7: src_bytes, Document, Statement, trivia and the
// string pool. No entity arrays, no includes, no resolution -- a statement
// stream is all a parser owes, and lowering it is P1's.
//
// `Statement` carries exactly the four fields PRD 7 gives it. What a statement
// *said* lives in columns beside it -- `stmt_payload` selects a row in the array
// its `kind` names, `stmt_children` spans its block -- because widening the row
// for six mutually exclusive payloads is the shape the columnar model exists to
// avoid. It also means P1 lowers from parsed values rather than re-lexing a src
// span.

enum class ElemKind : uint32_t { Chart, Include, State, Submachine, Trans, Attr };

// Names match the DSL's state_kind (PRD 15). `Initial` and `Final` are reachable
// only through `*` in an endpoint, never through this slot, so no spelling of
// the format produces them here.
enum class StateKind : uint32_t {
  Normal,
  Initial,
  Final,
  Choice,
  Junction,
  Fork,
  Join,
  History,
  DeepHistory,
};

enum class TransKind : uint32_t { External, Internal, Local };

// `Flag` is `@k` with nothing after it. It stays distinct from `@k = "true"`
// here: which of the two is canonical is the printer's rule (PRD 15), and a
// parser that folded them would have decided it early and irreversibly.
enum class AttrValueKind : uint32_t { Flag, Scalar, List };

// Where a comment sits relative to the statement that owns it. Leading and
// own-line both precede it; the difference is a blank line. Whether an own-line
// comment is inside the owner's block or before it is derivable -- compare its
// offset against the owner's src.off -- so it needs no fourth value.
enum class CommentPos : uint32_t { Leading, Trailing, OwnLine };

struct Document {
  StrRef path;
  Span text;        // -> src_bytes
  Span statements;  // -> stmts, authored source order
};

struct Statement {
  ElemKind kind;
  DocId doc;
  Span src;       // -> src_bytes; the whole construct, block included
  Span comments;  // -> comments, grouped by owner
};

struct Trivia {
  Span src;  // -> src_bytes; includes the "//"
  CommentPos pos;
};

// `On:main` and `On:1` -- a submachine qualifier by name or by ordinal (PRD 9).
// Both absent is the unqualified case.
struct PathSeg {
  StrRef name;
  StrRef qualifier;
  uint32_t ordinal;  // INVALID when the qualifier is a name or absent
};

struct Endpoint {
  uint32_t wildcard;  // `*`: initial or final by position, so segs is empty
  Span segs;          // -> path_segs
};

struct ChartStmt {
  StrRef name, label;
};

struct IncludeStmt {
  StrRef path, alias;
};

struct StateStmt {
  StrRef name, label;
  StateKind kind;
  uint32_t has_block;  // `state Foo` and `state Foo {}` are different source
};

struct SubmachineStmt {
  StrRef name, label;  // both may be empty; the block is mandatory
};

struct TransStmt {
  Endpoint src, dst;
  StrRef label;
  TransKind kind;
  uint32_t has_block;
};

struct AttrEntry {
  StrRef key;
  Span values;  // -> attr_values; empty for Flag, one for Scalar, n for List
  AttrValueKind kind;
};

// One row for `@k`, `@ns:k` and `@ns { a, b }` alike: the block spelling is n
// entries under one namespace, and the others are one entry.
struct AttrStmt {
  StrRef ns;
  Span entries;  // -> attr_entries
};

struct ParsedDocument {
  DocId id;
  Document doc;
  std::vector<scav_byte> src_bytes;  // normalized (PRD 6); never canonicalized

  std::vector<Statement> stmts;
  std::vector<uint32_t> stmt_payload;  // parallel to stmts; row in the kind's array
  std::vector<Span> stmt_children;     // parallel to stmts; -> stmt_ids
  std::vector<StmtId> stmt_ids;
  std::vector<Trivia> comments;

  std::vector<ChartStmt> charts;
  std::vector<IncludeStmt> includes;
  std::vector<StateStmt> states;
  std::vector<SubmachineStmt> submachines;
  std::vector<TransStmt> transitions;
  std::vector<AttrStmt> attrs;
  std::vector<AttrEntry> attr_entries;
  std::vector<StrRef> attr_values;
  std::vector<PathSeg> path_segs;

  StringPool strings;  // byte-sorted, two-pass interned
};

char const *syntax_elem_kind_name(ElemKind kind);
char const *syntax_state_kind_name(StateKind kind);
char const *syntax_trans_kind_name(TransKind kind);

// The DSL spelling of a state kind, or false when the word names none. `initial`
// and `final` are rejected: the format reaches them through `*`.
bool syntax_state_kind_from_name(std::string_view text, StateKind &out);
bool syntax_trans_kind_from_name(std::string_view text, TransKind &out);

// The chart statement, which is always the first row. INVALID when the parse
// produced nothing.
uint32_t syntax_root_statement(ParsedDocument const &pd);

// Parser ====================================================================

// LL(1) descent over PRD 15's grammar, with the descent held in an explicit
// std::vector of frames rather than in the call stack.
//
// That is not a style choice. Nesting depth is attacker-controlled, so a
// call-recursive parser's failure mode for a hostile document is a stack
// overflow -- which is a crash, not a diagnostic, and no depth cap can be
// checked reliably against a stack whose size the standard does not describe.
// With the frames on the heap the cap is an ordinary comparison and the answer
// is DiagCode::DepthLimitExceeded.

// Depth 16 is the design target (PRD 11), not a limit the grammar enforces. The
// format spends two block levels per state level plus one for the chart, so a
// legal depth-16 chart nests 33 deep; 256 leaves room for anything an author
// writes and still rejects a hostile document before the heap notices.
constexpr uint32_t DEFAULT_MAX_DEPTH{ 256 };

struct ParseOptions {
  uint32_t max_depth;
};

ParseOptions parse_default_options();

// Bytes must already be normalized. `name` is the document's path, and exists
// so a diagnostic can say `wifi.scav:12` rather than `<buffer>:12`.
bool parse_tokens(scav_byte const *bytes,
                  size_t byte_count,
                  LexResult const &lexed,
                  DocId doc,
                  std::string_view name,
                  ParseOptions const &opts,
                  ParsedDocument &out,
                  std::vector<Diagnostic> &diags);

// normalize -> lex -> parse, over bytes from anywhere. There is no overload
// taking a path: acquiring bytes is a different system (PRD 16.2).
bool parse_document(scav_byte const *bytes,
                    size_t len,
                    std::string_view name,
                    ParseOptions const &opts,
                    ParsedDocument &out,
                    std::vector<Diagnostic> &diags);

// Bytes the parsed document holds, for the memory-ratio assertion in the
// performance tests.
uint64_t parse_footprint(ParsedDocument const &pd);

}  // namespace scav

#endif  // SCAV_CORE_H_INCLUDED
