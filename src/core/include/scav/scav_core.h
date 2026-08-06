#ifndef SCAV_CORE_H_INCLUDED
#define SCAV_CORE_H_INCLUDED

// libscavcore's whole public API: call parse_document() and walk ParsedDocument.
// Each function carries the prefix of its section, and nothing here takes a path.

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scav {

// Ids and spans =============================================================

// Ids, and the two span shapes every column indexes through. Entity ids arrive
// with the model spine.

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

// Named constructors: these are built constantly, and a designated-initializer
// list at every site reads worse. An empty span is `{}`.
constexpr StrRef str_ref(uint32_t off, uint32_t len) { return { .off = off, .len = len }; }
constexpr Span make_span(uint32_t off, uint32_t len) { return { .off = off, .len = len }; }

// Narrowing =================================================================

// Callers measure buffers with size_t; the model indexes with uint32_t, because
// spans are {uint32 off, len} and size_t is 32 bits on wasm32, 64 elsewhere.
// size_t stops here: entry points take one, check it, and hand uint32_t inward.

// True when `value` round-trips into T. Returns a bool rather than asserting, so
// an over-large document becomes a diagnostic instead of a crash.
template <typename T, typename U>
bool narrow(U value, T &out) {
  static_assert(std::is_unsigned_v<T> && std::is_unsigned_v<U>,
                "scav narrows unsigned to unsigned; a signed length is a bug upstream");
  if (value > static_cast<U>(std::numeric_limits<T>::max())) { return false; }
  out = static_cast<T>(value);
  return true;
}

// Where the bound is already known to hold. Still checked -- it clamps rather
// than wrapping, so a mistake is a short read and not a 4-gigabyte one.
template <typename T, typename U>
T narrow_clamp(U value) {
  static_assert(std::is_unsigned_v<T> && std::is_unsigned_v<U>, "unsigned only");
  constexpr U LIMIT{ static_cast<U>(std::numeric_limits<T>::max()) };
  return static_cast<T>((value > LIMIT) ? LIMIT : value);
}

// Diagnostics ===============================================================

// A code plus a source span. A syntax error is reported before any statement
// exists, so the span is the payload; line and column are derived on demand.

enum class DiagCode : uint32_t {
  Ok,

  // Spans are {uint32 off, len}, so 4 GiB is the ceiling. Saying so beats
  // truncating into a document that parses differently.
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

// One-based; the column counts codepoints, not bytes. Offsets index normalized
// bytes, so both numbers are the same on every platform.
struct LineCol {
  uint32_t line, column;
};

LineCol diag_line_col(scav_byte const *bytes, size_t len, size_t offset);

// A short, locale-free description. Rendering a formatted message is the
// application's job; a library does not touch the stream globals.
char const *diag_message(DiagCode code);

bool diag_has_errors(std::vector<Diagnostic> const &diags);

// String pool ===============================================================

// A StrRef is an offset and length into StringPool::bytes. Not NUL-terminated:
// strings come out as spans. Building a pool is private -- a client only reads.

struct StringPool {
  std::vector<scav_byte> bytes;
};

// A zero-length ref reads as the empty string without touching `pool`.
std::string_view string_pool_view(StringPool const &pool, StrRef ref);

// Byte-wise, unsigned, shorter-prefix-first -- the order the pool is finalized
// in, so anything comparing names has to agree with it.
int string_compare_bytes(scav_byte const *a, size_t alen, scav_byte const *b, size_t blen);

// Source text ===============================================================

// BOM stripped, line endings to LF, UTF-8 validated, NFC applied -- otherwise
// autocrlf and NFD put different bytes in the pool from the same commit. Offsets
// downstream index the normalized bytes; UTF-8 errors predate that buffer.

// Returns false and leaves `out` empty when the input is not valid UTF-8.
// `out` is cleared first either way.
bool source_text_normalize(scav_byte const *bytes,
                           size_t len,
                           DocId doc,
                           std::vector<scav_byte> &out,
                           std::vector<Diagnostic> &diags);

// Exposed for the string-literal decoder, which needs these on decoded escapes
// rather than on a whole document.
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

// Exposed for the raw-string decoder, which works on already-normalized bytes.
bool source_text_is_ascii(scav_byte const *bytes, size_t len);

std::string_view source_text_view(std::vector<scav_byte> const &bytes, Span span);

// Lexer =====================================================================

// Bytes in, one flat token vector out -- not a pull lexer, so lexing and parsing
// can be timed and fuzzed apart. The vector is scratch, freed after the parse.

// No keyword kinds: `s`, `m` and `t` are keywords only in statement-leading
// position, so a lexer that decided would have to know where it was.
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

// Comments are lexed and never parsed, so they travel beside the token stream.
// The two flags are exactly what position classification needs.
struct LexComment {
  Span src;              // includes the "//", excludes the newline
  uint32_t code_before;  // non-whitespace earlier on the same line
  uint32_t blank_after;  // a blank line, or the end of input, follows
};

struct LexResult {
  std::vector<Token> tokens;
  std::vector<LexComment> comments;
};

// Skips an unexpected character so one run reports all of them; stops at
// anything else. Returns false when it reported anything.
bool lex_source(scav_byte const *bytes,
                size_t byte_count,
                DocId doc,
                LexResult &out,
                std::vector<Diagnostic> &diags);

char const *lex_token_kind_name(TokKind kind);

// Reserved in every position. `choice`, `history`, `as`, `kind`, `s`, `m` and
// `t` are absent on purpose -- they are contextual, so a state may be named one.
bool lex_is_reserved_word(std::string_view text);

// Lexeme includes its delimiters. Applies escapes, dedents a raw string to its
// closing column, and NFC-folds -- a \u escape can decode to a decomposed form.
bool lex_decode_string_literal(scav_byte const *bytes,
                               Span lexeme,
                               DocId doc,
                               std::vector<scav_byte> &out,
                               std::vector<Diagnostic> &diags);

// Bytes held, for the performance tests' memory-ratio assertion. Capacity, not
// size: the peak is what matters.
uint64_t lex_footprint(LexResult const &result);

// Syntax tree ===============================================================

// The front end's whole output. No entity arrays and no resolution: a statement
// stream is all a parser owes. What a statement *said* lives in columns beside
// it -- `stmt_payload` indexes the array its `kind` names, `stmt_children` its
// block.

enum class ElemKind : uint32_t { Chart, Include, State, Submachine, Trans, Attr };

// `Initial` and `Final` are reachable only through `*` in an endpoint, so no
// spelling of the format produces them in this slot.
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

// `Flag` is `@k` with nothing after it, kept distinct from `@k = "true"`: which
// spelling is canonical is the printer's call, not the parser's.
enum class AttrValueKind : uint32_t { Flag, Scalar, List };

// Leading and own-line both precede the owner; the difference is a blank line.
// Inside-the-block versus before-it is derivable from the offsets.
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

// `On:main` and `On:1` -- a submachine qualifier by name or ordinal. Both absent
// is the unqualified case.
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
// entries under one namespace, the others are one.
struct AttrStmt {
  StrRef ns;
  Span entries;  // -> attr_entries
};

struct ParsedDocument {
  DocId id;
  Document doc;
  std::vector<scav_byte> src_bytes;  // normalized; never canonicalized

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

// False when the word names no kind. `initial` and `final` are rejected: the
// format reaches them through `*`.
bool syntax_state_kind_from_name(std::string_view text, StateKind &out);
bool syntax_trans_kind_from_name(std::string_view text, TransKind &out);

// The chart statement, always row zero. INVALID when nothing parsed.
uint32_t syntax_root_statement(ParsedDocument const &pd);

// Parser ====================================================================

// LL(1) descent, with the frames in a heap vector rather than the call stack:
// nesting depth is attacker-controlled, and a stack overflow is a crash where a
// depth cap is a diagnostic.

// Depth 16 is the design target. The format spends two block levels per state
// level plus one for the chart, so a legal depth-16 chart nests 33 deep.
constexpr uint32_t DEFAULT_MAX_DEPTH{ 256 };

struct ParseOptions {
  uint32_t max_depth;
};

ParseOptions parse_default_options();

// Bytes must already be normalized. `name` lets a diagnostic say `wifi.scav:12`
// rather than `<buffer>:12`.
bool parse_tokens(scav_byte const *bytes,
                  size_t byte_count,
                  LexResult const &lexed,
                  DocId doc,
                  std::string_view name,
                  ParseOptions const &opts,
                  ParsedDocument &out,
                  std::vector<Diagnostic> &diags);

// normalize -> lex -> parse, over bytes from anywhere. No overload takes a path:
// acquiring bytes is a different system.
bool parse_document(scav_byte const *bytes,
                    size_t len,
                    std::string_view name,
                    ParseOptions const &opts,
                    ParsedDocument &out,
                    std::vector<Diagnostic> &diags);

// Bytes held, for the performance tests' memory-ratio assertion.
uint64_t parse_footprint(ParsedDocument const &pd);

}  // namespace scav

#endif  // SCAV_CORE_H_INCLUDED
