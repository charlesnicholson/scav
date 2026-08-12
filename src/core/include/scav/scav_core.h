#ifndef SCAV_CORE_H_INCLUDED
#define SCAV_CORE_H_INCLUDED

// libscavcore's whole public API: parse a document with parse_document(), or
// build a Chart with the build_ functions, and walk the arrays either way.
// Each function carries the prefix of its section, and nothing here takes a path.

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scav {

// Ids and spans =============================================================

// Ids, and the two span shapes every column indexes through. An id is an
// ordinal *and* an array index; tombstoning keeps that true under deletion.

constexpr uint32_t INVALID{ 0xFFFF'FFFFU };

struct DocId {
  uint32_t v;
};
struct StmtId {
  uint32_t v;
};
struct InstId {  // an include instantiation = index into Chart::includes
  uint32_t v;
};
struct StateId {
  uint32_t v;
};
struct SubmachineId {
  uint32_t v;
};
struct TransId {
  uint32_t v;
};
struct AttrKeyId {  // interned attribute key, `ns:key` or bare
  uint32_t v;
};
struct ColumnId {  // index into Chart::columns
  uint32_t v;
};

constexpr bool operator==(DocId a, DocId b) { return a.v == b.v; }
constexpr bool operator!=(DocId a, DocId b) { return a.v != b.v; }
constexpr bool operator==(StmtId a, StmtId b) { return a.v == b.v; }
constexpr bool operator!=(StmtId a, StmtId b) { return a.v != b.v; }
constexpr bool operator==(InstId a, InstId b) { return a.v == b.v; }
constexpr bool operator!=(InstId a, InstId b) { return a.v != b.v; }
constexpr bool operator==(StateId a, StateId b) { return a.v == b.v; }
constexpr bool operator!=(StateId a, StateId b) { return a.v != b.v; }
constexpr bool operator==(SubmachineId a, SubmachineId b) { return a.v == b.v; }
constexpr bool operator!=(SubmachineId a, SubmachineId b) { return a.v != b.v; }
constexpr bool operator==(TransId a, TransId b) { return a.v == b.v; }
constexpr bool operator!=(TransId a, TransId b) { return a.v != b.v; }
constexpr bool operator==(AttrKeyId a, AttrKeyId b) { return a.v == b.v; }
constexpr bool operator!=(AttrKeyId a, AttrKeyId b) { return a.v != b.v; }
constexpr bool operator==(ColumnId a, ColumnId b) { return a.v == b.v; }
constexpr bool operator!=(ColumnId a, ColumnId b) { return a.v != b.v; }

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
  BlockCommentUnsupported,
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
// strings come out as spans.
//
// The pool is not deduplicated, so two equal strings can hold two StrRefs.
// Compare the views, not the refs.

struct StringPool {
  std::vector<scav_byte> bytes;
};

// A zero-length ref reads as the empty string without touching `pool`. Inline
// because it is the whole of reading a pool: one bounds-free span over bytes the
// caller already owns. `char` may alias any object representation, which is what
// the cast relies on; the other direction would not be safe.
inline std::string_view string_pool_view(StringPool const &pool, StrRef ref) {
  if (ref.len == 0) { return {}; }
  return { reinterpret_cast<char const *>(pool.bytes.data() + ref.off), ref.len };
}

// Appends as met, never deduplicates (see above). The empty string is the empty
// ref, so it costs no pool growth and every empty field looks the same.
StrRef string_pool_add(StringPool &pool, std::string_view text);

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

enum class StmtKind : uint32_t { Chart, Include, State, Submachine, Trans, Attr };

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
  StmtKind kind;
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

  StringPool strings;  // append-order, not deduplicated
};

char const *syntax_stmt_kind_name(StmtKind kind);
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

// Model =====================================================================

// Flat arrays of POD aggregates linked by ordinal; no pointer or reference
// between records. An id is an array index, deletion is a tombstone
// (`live = 0`), ids are never reused, and every walk skips dead rows -- that is
// the whole liveness protocol. Attrs carry no `live` of their own: liveness
// belongs to the entity, and rows reached through an entity's span inherit it.

// What an entity *is*, for columns, diagnostics, and DrawList origins. Not
// StmtKind, which enumerates what a line of source is: `Include` and `Attr`
// exist only as statements, `Point` and `PathBox` only as entities.
enum class ElemKind : uint32_t {
  State,
  Submachine,
  Transition,
  Chart,
  Point,
  PathBox,
  None,
};

struct ElemRef {
  ElemKind kind;
  uint32_t ordinal;
};

constexpr bool operator==(ElemRef a, ElemRef b) {
  return (a.kind == b.kind) && (a.ordinal == b.ordinal);
}
constexpr bool operator!=(ElemRef a, ElemRef b) { return !(a == b); }

struct State {
  StrRef name;   // empty for pseudostates synthesized from `*`
  StrRef label;  // the positional string; opaque, may be empty
  SubmachineId parent;
  StateKind kind;
  Span submachines;  // -> submachine_ids
  Span attrs;        // -> attrs
  StmtId stmt;       // the statement that declared it; INVALID when code-built
  InstId inst;       // INVALID in the root document
  uint32_t live;     // 0 = tombstone
};

struct Submachine {
  StateId owner;     // INVALID for a document root
  uint32_t ordinal;  // position among the owner's submachines, fixed at build
  StrRef name;
  StrRef label;
  Span children;  // -> state_ids, document order
  Span attrs;     // -> attrs
  StmtId stmt;
  InstId inst;
  uint32_t live;
};

struct Transition {
  StateId src, dst;
  TransKind kind;
  StrRef label;  // opaque; there is no event entity in the core
  Span attrs;    // -> attrs
  StmtId stmt;
  InstId inst;
  uint32_t live;
};

// One row per instantiation; its ordinal is the InstId. `target` stays INVALID
// until the loader (P2) resolves the path to a document; the alias host state
// exists from the moment the include does, because an alias is a state.
struct Include {
  StrRef alias;
  DocId target;
  StateId host;  // the alias state it synthesizes; never INVALID
  StmtId stmt;
};

struct Attr {
  AttrKeyId key;
  StrRef value;
};

// Columns: type-erased byte arrays with a stride, indexed by entity ordinal.
// How extension data lives *in* the model rather than in app-side tables.

enum class ValueKind : uint32_t { U32, I32, U64, I64, StrRef, Span, Blob, Pod };

// ColumnDesc.flags bit 0: a derived column is skipped by the serializer and
// exempt from round-trip-unknown, or a stale geometry snapshot would survive a
// save and get trusted instead of recomputed.
constexpr uint32_t COLUMN_DERIVED{ 1U };

struct ColumnDesc {  // 28 bytes, no padding
  StrRef name;       // "libhsm.events"; -> Chart::column_names, not Chart::strings
  ElemKind entity;   // `None` and `PathBox` never appear here
  ValueKind kind;
  uint32_t elem_size, elem_align;
  uint32_t flags;
};

struct Column {
  ColumnDesc desc;
  std::vector<scav_byte> bytes;  // count * elem_size
};

// One document network rooted at one document. All documents share these
// arrays, each entity tagged with the statement that declared it and the
// instantiation it belongs to -- no flattening step and no second model shape.
struct Chart {
  std::vector<Document> documents;
  std::vector<Statement> stmts;
  std::vector<Trivia> comments;      // Statement.comments spans into this
  std::vector<scav_byte> src_bytes;  // normalized source; never canonicalized

  std::vector<State> states;            // indexed by StateId
  std::vector<Submachine> submachines;  // indexed by SubmachineId
  std::vector<Transition> transitions;  // indexed by TransId
  std::vector<Include> includes;        // indexed by InstId
  std::vector<Attr> attrs;

  // Attribute keys are genuinely interned -- equal keys give equal ids, which
  // is the point of an id -- so their bytes live apart from the never-
  // deduplicated `strings`. AttrKeyId indexes attr_key_names.
  std::vector<StrRef> attr_key_names;
  StringPool attr_keys;

  std::vector<Column> columns;  // indexed by ColumnId
  StringPool column_names;      // a registered name is not authored text

  std::vector<StateId> state_ids;  // Span targets
  std::vector<SubmachineId> submachine_ids;

  StringPool strings;  // authored names and labels; append order

  // Braced so `Chart c;` is a usable empty chart: the vectors above initialize
  // themselves, and a POD tail left indeterminate would make every walk UB.
  StrRef name{}, label{};
  SubmachineId root_submachine{ INVALID };
  Span chart_attrs{};  // -> attrs
};

// Chart queries =============================================================

// Reads over the model. Everything here checks liveness where a walk could
// yield a dead row; nothing here allocates except chart_path_of, which builds
// a string.

inline std::string_view chart_string(Chart const &c, StrRef ref) {
  return string_pool_view(c.strings, ref);
}

// The key's canonical spelling, `ns:key` or bare. Empty for an id never handed
// out by this chart.
std::string_view chart_attr_key(Chart const &c, AttrKeyId key);

// The interned id for `key`, or INVALID when this chart never met it.
AttrKeyId chart_attr_key_find(Chart const &c, std::string_view key);

// Rows of the entity array `kind` names. Chart is one entity; Point and
// PathBox have no entity array yet, so their count is the column's business.
uint32_t chart_entity_count(Chart const &c, ElemKind kind);

// In range and of an entity kind that has rows. INVALID ordinals fail.
bool chart_ref_valid(Chart const &c, ElemRef ref);

// False for a tombstoned row, and for any ref chart_ref_valid rejects. The
// chart entity is always live.
bool chart_live(Chart const &c, ElemRef ref);

// The subject's attrs span. Empty for a ref without attrs (Point, PathBox,
// None, out of range).
Span chart_attrs_of(Chart const &c, ElemRef ref);

// Index into Chart::attrs of the subject's first attr with `key`, or INVALID.
// A list-valued attr is N rows under one key: walk the span for the rest.
uint32_t chart_attr_find(Chart const &c, ElemRef subject, std::string_view key);

// The submachine-qualified `/`-separated path: `On:main/Idle`. Unnamed
// pseudostates spell as `$<kind>`, ordinal-suffixed past the first, so every
// live state has an address. Appends to `out` without clearing it.
void chart_path_of(Chart const &c, StateId id, std::string &out);

// Bytes held, for the performance tests' memory-ratio assertion. Capacity, not
// size: the peak is what matters.
uint64_t chart_footprint(Chart const &c);

// Builder ===================================================================

// Append-only: rows are never removed, reordered, or compacted, so an id handed
// out stays valid for the life of the chart. Appending where a span is not at
// the tail of its shared array rebuilds that array in O(n) -- microseconds at
// the scale target, and cheaper than an indirection every read would pay
// forever.
//
// A bad call -- an id out of range, a tombstoned parent, a second build_chart --
// appends nothing and returns the INVALID id. The builder will not manufacture
// a structurally bad model from a bad argument; validation (§10) is for models,
// not for calls.

// Names the chart, creates the root submachine, and returns it. The chart must
// be empty. Everything else hangs off the returned id.
SubmachineId build_chart(Chart &c, std::string_view name, std::string_view label);

StateId build_state(Chart &c,
                    SubmachineId parent,
                    std::string_view name,
                    StateKind kind,
                    std::string_view label);

SubmachineId build_submachine(Chart &c,
                              StateId owner,
                              std::string_view name,
                              std::string_view label);

TransId build_trans(Chart &c,
                    StateId src,
                    StateId dst,
                    TransKind kind,
                    std::string_view label);

// Appends one Attr row to the subject and returns its index into Chart::attrs,
// interning `key` if this chart never met it. A repeated key appends another
// row -- that is how a list value is stored. Note the returned index is the one
// place the append-only rule bends: a later build_attr for a *different*
// subject can shift attr rows to keep spans contiguous, so hold the subject and
// re-find rather than holding the index across builds.
uint32_t build_attr(Chart &c,
                    ElemRef subject,
                    std::string_view key,
                    std::string_view value);

// Synthesizes the alias host state in `parent` and appends the Include row
// naming it. The authored path is source text, not model data, so it is not
// taken here; the loader (P2) fills `target`.
InstId build_include(Chart &c, SubmachineId parent, std::string_view alias);

// Columns ===================================================================

// What the core owes an extension: store it indexed by entity ordinal, keep it
// index-aligned under append and tombstoning, round-trip it losslessly, pass it
// through unread.

// Appends a column sized to the entity's current count, zero-filled, and
// returns its id. The name is interned into Chart::column_names. Rejected with
// INVALID: an entity of None or PathBox, a zero elem_size, an alignment that is
// zero or does not divide elem_size, and a name already registered -- a column
// is an identity, so re-registration is a caller bug, not an upsert.
ColumnId column_register(Chart &c,
                         std::string_view name,
                         ElemKind entity,
                         ValueKind kind,
                         uint32_t elem_size,
                         uint32_t elem_align,
                         uint32_t flags);

// INVALID when no column has that name. Linear scan by bytes: registration
// order is not canonical, and a chart holds few columns.
ColumnId column_find(Chart const &c, std::string_view name);

scav_byte *column_data(Chart &c, ColumnId id);
scav_byte const *column_data(Chart const &c, ColumnId id);

// Rows, not bytes: bytes.size() / elem_size.
uint32_t column_count(Chart const &c, ColumnId id);

}  // namespace scav

#endif  // SCAV_CORE_H_INCLUDED
