#ifndef SCAV_CORE_H_INCLUDED
#define SCAV_CORE_H_INCLUDED

// libscavcore's public API: parse a document, assemble a network, or build a
// Chart directly, then walk the arrays.

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

// An id is an ordinal *and* an array index into the entity array it names.
// Deletion tombstones rather than compacting, so that stays true.

constexpr uint32_t INVALID{ 0xFFFF'FFFFU };

struct DocId {
  uint32_t v;
  constexpr bool operator==(DocId const &) const = default;
};
struct StmtId {
  uint32_t v;
  constexpr bool operator==(StmtId const &) const = default;
};
struct InstId {  // an include instantiation = index into Chart::includes
  uint32_t v;
  constexpr bool operator==(InstId const &) const = default;
};
struct StateId {
  uint32_t v;
  constexpr bool operator==(StateId const &) const = default;
};
struct SubmachineId {
  uint32_t v;
  constexpr bool operator==(SubmachineId const &) const = default;
};
struct TransId {
  uint32_t v;
  constexpr bool operator==(TransId const &) const = default;
};
struct AttrKeyId {  // interned attribute key, `ns:key` or bare
  uint32_t v;
  constexpr bool operator==(AttrKeyId const &) const = default;
};
struct ColumnId {  // index into Chart::columns
  uint32_t v;
  constexpr bool operator==(ColumnId const &) const = default;
};

// Ids compare for equality only. A sort that means to order by ordinal says
// `.v`.

// What an entity is. StmtKind is the parallel enum over what a line of source
// is.
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
  constexpr bool operator==(ElemRef const &) const = default;
};

// Into StringPool::bytes. Two equal refs are the same span; equal *text* is a
// view comparison, because the pool never deduplicates.
struct StrRef {
  uint32_t off, len;
  constexpr bool operator==(StrRef const &) const = default;
};

// Into a side array.
struct Span {
  uint32_t off, len;
  constexpr bool operator==(Span const &) const = default;
};

// An empty span is `{}`.
constexpr StrRef str_ref(uint32_t off, uint32_t len) { return { .off = off, .len = len }; }
constexpr Span make_span(uint32_t off, uint32_t len) { return { .off = off, .len = len }; }

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

// Diagnostics ===============================================================

// A code plus either a source span or an entity, depending on which existed
// when it fired. Line and column are derived on demand.

enum class DiagCode : uint32_t {
  Ok,

  DocumentTooLarge,

  // Normalization. Spans index the *raw* input: the normalized buffer does not
  // exist yet when these fire.
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

  // Loading. The span indexes the bytes handed to load_add, and a session
  // reporting one of these produces no chart.
  IncludePathInvalid,
  IncludePathUnresolved,
  IncludeCycle,
  IncludeExpansionTooLarge,
  DocumentNotRequested,
  DocumentAlreadyLoaded,
  LoadSessionEmpty,

  // Lowering. These carry the offending statement's span; the entity they
  // would have named was never created.
  MisplacedStatement,
  WildcardBothEndpoints,
  EndpointUnresolved,
  BadSubmachineQualifier,
  EndpointCrossesInclude,

  // Validation. These carry an ElemRef; file, line and column come from
  // walking to that entity's statement.
  DanglingRef,
  MissingRequiredId,
  TombstonedTarget,
  DuplicateName,
  MultipleInitial,
  NameHasMetacharacter,
  StatementSpanOutOfRange,
  ColumnCountMismatch,
};

// A producer running before entities exist fills `src`; one running after fills
// `subject`, and the reader walks to that subject's statement for a position.
struct Diagnostic {
  DiagCode code;
  ElemRef subject{ .kind = ElemKind::None, .ordinal = INVALID };
  DocId doc;
  Span src;
};

// One-based; the column counts codepoints, not bytes. Offsets index normalized
// bytes, so both numbers are the same on every platform.
struct LineCol {
  uint32_t line, column;
};

LineCol diag_line_col(scav_byte const *bytes, size_t len, size_t offset);

// A short, locale-free description. Formatting a message is the caller's.
char const *diag_message(DiagCode code);

bool diag_has_errors(std::vector<Diagnostic> const &diags);

// String pool ===============================================================

// An offset and length into StringPool::bytes, not NUL-terminated. The pool
// keeps duplicates, so equal text can hold two refs: compare the views.

struct StringPool {
  std::vector<scav_byte> bytes;
};

// A zero-length ref reads as the empty string without touching `pool`. The cast
// is legal because `char` may alias any object representation.
inline std::string_view string_pool_view(StringPool const &pool, StrRef ref) {
  if (ref.len == 0) { return {}; }
  return { reinterpret_cast<char const *>(pool.bytes.data() + ref.off), ref.len };
}

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

// Bytes held, summing capacity rather than size.
uint64_t lex_footprint(LexResult const &result);

// Syntax tree ===============================================================

// The front end's whole output. `stmt_payload` indexes the array a statement's
// `kind` names; `stmt_children` indexes its block.

enum class StmtKind : uint32_t { Chart, Include, State, Submachine, Trans, Attr };

// `Initial` and `Final` are reachable only through `*` in an endpoint; no
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

// `Flag` is `@k` with nothing after it, kept distinct from `@k = "true"`.
enum class AttrValueKind : uint32_t { Flag, Scalar, List };

// Leading and own-line both precede the owner; the difference is a blank line
// between them. Inside-the-block versus before-it follows from the offsets.
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

// `On:main` and `On:1`: a submachine qualifier by name or by ordinal. Both
// absent is the unqualified case.
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

// One row for `@k`, `@ns:k` and `@ns { a, b }` alike. The block spelling is n
// entries under one namespace; the others are one.
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

// False when the word names no kind. `initial` and `final` are rejected; the
// format reaches those through `*`.
bool syntax_state_kind_from_name(std::string_view text, StateKind &out);
bool syntax_trans_kind_from_name(std::string_view text, TransKind &out);

// The chart statement, always row zero. INVALID when nothing parsed.
uint32_t syntax_root_statement(ParsedDocument const &pd);

// Parser ====================================================================

// LL(1) descent with the frames in a heap vector rather than the call stack, so
// a hostile nesting depth is a diagnostic instead of a stack overflow.

// Block levels, not state levels: the format spends two blocks per state level
// plus one for the chart, so a depth-16 chart nests 33 deep.
constexpr uint32_t DEFAULT_MAX_DEPTH{ 256 };

struct ParseOptions {
  uint32_t max_depth;
};

ParseOptions parse_default_options();

// Bytes must already be normalized. `name` is what a diagnostic quotes.
bool parse_tokens(scav_byte const *bytes,
                  size_t byte_count,
                  LexResult const &lexed,
                  DocId doc,
                  std::string_view name,
                  ParseOptions const &opts,
                  ParsedDocument &out,
                  std::vector<Diagnostic> &diags);

// normalize -> lex -> parse, over bytes from anywhere.
bool parse_document(scav_byte const *bytes,
                    size_t len,
                    std::string_view name,
                    ParseOptions const &opts,
                    ParsedDocument &out,
                    std::vector<Diagnostic> &diags);

// Bytes held, summing capacity rather than size.
uint64_t parse_footprint(ParsedDocument const &pd);

// Model =====================================================================

// Flat POD arrays linked by ordinal. An id is an array index, deletion sets
// `live = 0`, and rows reached through an entity's span inherit its liveness.

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

// One row per instantiation, its ordinal the InstId. `path` is the authored
// string verbatim; a load session fills `target`.
struct Include {
  StrRef alias;
  StrRef path;
  DocId target;
  StateId host;  // the alias state it synthesizes; never INVALID
  StmtId stmt;
};

// `stmt` is the attr's own statement, not its subject's: `state On { @doc }` is
// two authored statements, and a reader pointed at the state cannot find the
// attribute.
struct Attr {
  AttrKeyId key;
  StrRef value;
  StmtId stmt;
};

// Type-erased byte arrays with a stride, indexed by entity ordinal.

enum class ValueKind : uint32_t { U32, I32, U64, I64, StrRef, Span, Blob, Pod };

// ColumnDesc.flags bit 0. A derived column is skipped by the serializer and
// exempt from round-trip-unknown.
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

// One document network. All documents share these arrays, each entity carrying
// its declaring statement and its instantiation.
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

  // Interned, so equal keys give equal ids. Deduplicated here, which is why
  // the bytes live apart from `strings`. AttrKeyId indexes attr_key_names.
  std::vector<StrRef> attr_key_names;
  StringPool attr_keys;

  std::vector<Column> columns;  // indexed by ColumnId
  StringPool column_names;      // a registered name is not authored text

  std::vector<StateId> state_ids;  // Span targets
  std::vector<SubmachineId> submachine_ids;

  StringPool strings;  // authored names and labels; append order

  // Braced so `Chart c;` is a usable empty chart. The vectors above initialize
  // themselves; this tail would otherwise be indeterminate.
  StrRef name{}, label{};
  SubmachineId root_submachine{ INVALID };
  Span chart_attrs{};  // -> attrs
};

// Chart queries =============================================================

// Reads over the model. Each checks liveness where a walk could yield a dead
// row, and none allocates except chart_path_of.

inline std::string_view chart_string(Chart const &c, StrRef ref) {
  return string_pool_view(c.strings, ref);
}

// The key's canonical spelling, `ns:key` or bare. Empty for an id never handed
// out by this chart.
std::string_view chart_attr_key(Chart const &c, AttrKeyId key);

// The interned id for `key`, or INVALID when this chart never met it.
AttrKeyId chart_attr_key_find(Chart const &c, std::string_view key);

// Rows of the entity array `kind` names. Chart counts as one; Point and
// PathBox have no entity array, so they count zero.
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
// A list-valued attr is N rows under one key; walk the span for the rest.
uint32_t chart_attr_find(Chart const &c, ElemRef subject, std::string_view key);

// The submachine-qualified path, `On:main/Idle`; an unnamed pseudostate spells
// as `$<kind>`, ordinal-suffixed past the first. Appends to `out`.
void chart_path_of(Chart const &c, StateId id, std::string &out);

// Bytes held, summing capacity rather than size.
uint64_t chart_footprint(Chart const &c);

// Builder ===================================================================

// Append-only, so an id stays valid for the life of the chart. A bad call
// appends nothing and returns INVALID.

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

// Appends one Attr row and returns its index, interning `key`; a repeated key
// appends another row. A later build_attr elsewhere shifts that index, so a
// caller stamping `stmt` does it before the next call.
uint32_t build_attr(Chart &c,
                    ElemRef subject,
                    std::string_view key,
                    std::string_view value);

// Synthesizes the alias host state in `parent` and appends the Include row
// naming it. `path` is stored verbatim; a load session fills `target`.
InstId build_include(Chart &c,
                     SubmachineId parent,
                     std::string_view alias,
                     std::string_view path);

// Columns ===================================================================

// Extension data, stored indexed by entity ordinal and kept index-aligned
// under append and tombstoning.

// Appends a zero-filled column sized to the entity's current count. INVALID for
// a duplicate name, an entity of None or PathBox, or a bad size or alignment.
ColumnId column_register(Chart &c,
                         std::string_view name,
                         ElemKind entity,
                         ValueKind kind,
                         uint32_t elem_size,
                         uint32_t elem_align,
                         uint32_t flags);

// INVALID when no column has that name. Linear scan over the registered
// names.
ColumnId column_find(Chart const &c, std::string_view name);

scav_byte *column_data(Chart &c, ColumnId id);
scav_byte const *column_data(Chart const &c, ColumnId id);

// Rows, not bytes: bytes.size() / elem_size.
uint32_t column_count(Chart const &c, ColumnId id);

// Resolution ================================================================

// Which rule a failed path broke, not merely that one did.
enum class ResolveStatus : uint32_t { Ok, NotFound, BadQualifier, CrossesInclude };

// Resolves a state path against `scope`: the first segment innermost-outward
// within its own document, later ones descending strictly.
ResolveStatus resolve_path(Chart const &c,
                           SubmachineId scope,
                           std::string_view path,
                           StateId &out);

// Lowering ==================================================================

// One document into an empty chart: rebases pd's front-end slice, creates its
// entity rows, leaves every include unresolved. False if it reported anything.
bool lower_document(Chart &c, ParsedDocument const &pd, std::vector<Diagnostic> &diags);

// Document paths ============================================================

// A document name is a key, resolved by byte-wise segment folding. Names are
// `/`-separated on every transport.

// Resolves `ref` against `base`, assigning `out`. A relative ref joins `base`'s
// directory and folds `.` and `..`; an absolute or scheme-carrying one does not.
bool path_resolve(std::string_view base, std::string_view ref, std::string &out);

// Load session ==============================================================

// Add the root, read `pending`, resolve and add each, repeat until empty,
// finish. Parsed once per distinct name, instantiated once per include.

struct Pending {
  Span path;          // -> the session's path pool; read with load_pending_path
  DocId from;         // the document whose include statement claimed this one
  uint32_t stmt_row;  // that statement's row in `from`'s parsed document
};

// A DocId is fixed by the first include statement naming that path, ordered by
// (requesting DocId, statement ordinal). Arrival order does not enter into it.
struct LoadDoc {
  StrRef name;        // the resolved key, into LoadSession::paths
  uint32_t arrived;   // 0 until load_add supplied its bytes
  Span edges;         // -> LoadSession::edges, this document's includes
  DocId from;         // who claimed it; INVALID for the root
  uint32_t stmt_row;  // the claiming include statement's row in `from`
};

// One include statement, discovered. The junction between the document graph
// the session walks and the instantiation tree finish builds.
struct IncludeEdge {
  DocId from;
  DocId to;
  uint32_t stmt_row;  // the include statement's row in `from`'s document
};

// Parsed documents are held until finish, which instantiates from their
// statement payloads.
struct LoadSession {
  StringPool paths;                    // resolved keys, and Pending spans into it
  std::vector<LoadDoc> docs;           // indexed by DocId, claim order
  std::vector<ParsedDocument> parsed;  // parallel to docs; empty until arrived
  std::vector<IncludeEdge> edges;
  std::vector<Pending> pending;   // refreshed by load_pending
  std::vector<Diagnostic> diags;  // document-local; see DiagCode above
  uint32_t poisoned{ 0 };         // a failed add; finish can only report
};

// Normalizes, lexes and parses `bytes`, claiming a DocId per path its includes
// name. The first call is the root; a later one must name a pending path.
bool load_add(LoadSession &s, scav_byte const *bytes, size_t len, std::string_view name);

// What the session still needs, in DocId order. The returned view is
// invalidated by the next load_add. Empty means finish.
std::vector<Pending> const &load_pending(LoadSession &s);

inline std::string_view load_pending_path(LoadSession const &s, Pending const &p) {
  return string_pool_view(s.paths, str_ref(p.path.off, p.path.len));
}

// The resolved key `doc` was claimed under. Empty for an unknown DocId.
std::string_view load_document_name(LoadSession const &s, DocId doc);

// The normalized bytes parsed for `doc`, which a document-local diagnostic's
// span indexes. False for a document still pending.
bool load_document_bytes(LoadSession const &s,
                         DocId doc,
                         scav_byte const **out,
                         uint32_t *len);

// Builds a complete network into an empty `out`, spending the session. Whether
// `out` gained documents says which pool the diagnostics index.
bool load_finish(LoadSession &s, Chart &out, std::vector<Diagnostic> &diags);

// The filesystem transport, composed from the calls above.

// Reads `path` whole. Returns false when it cannot be opened or read.
bool read_file(char const *path, std::vector<scav_byte> &out);

// The load loop with `read_file` in the fetch slot. `session` outlives the call
// for load_document_bytes; `failed_path` names a document it could not read.
bool load_file(char const *path,
               LoadSession &session,
               Chart &out,
               std::vector<Diagnostic> &diags,
               std::string &failed_path);

// Structural hash ===========================================================

// The canonical serialization of a chart's entity arrays and authored text,
// field by field in array order with every string length-prefixed.
void chart_digest_bytes(Chart const &c, std::vector<scav_byte> &out);

// xxHash32 over the above.
uint32_t chart_structural_hash(Chart const &c);

// Validation ================================================================

// Structural checks over the live rows, a tombstoned one only as a target.
// Appends findings sorted by (code, subject kind, subject ordinal).
bool validate_chart(Chart const &c, std::vector<Diagnostic> &diags);

}  // namespace scav

#endif  // SCAV_CORE_H_INCLUDED
