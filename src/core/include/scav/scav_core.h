#ifndef SCAV_CORE_H_INCLUDED
#define SCAV_CORE_H_INCLUDED

// libscavcore's public API. A Chart is the product: load a document network or
// build one directly, query it, validate it, hash it.

#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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

  // Loading. The span indexes the bytes handed to load_add, and a loader
  // reporting one of these produces no chart.
  IncludePathInvalid,
  IncludePathUnresolved,
  IncludeCycle,
  IncludeExpansionTooLarge,
  DocumentNotRequested,
  DocumentAlreadyLoaded,
  LoaderEmpty,

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

  // Space requests. These carry the requesting entity, so the failure is
  // attributed to the request that caused it.
  SpaceOutOfRange,
  SpaceOrderDuplicate,
  SpaceSubjectInvalid,
  SpaceCountMismatch,

  // Layout.
  ProfileOutOfRange,
  CoordinateOverflow,
  RouterUnknown,

  // Appended past the groups above so no earlier code's value moves.
  // Validation: a containment relation whose two sides disagree, or a cycle.
  ContainmentInconsistent,
  // Layout: a column already registered under a scav.geom name with another
  // shape, which layout refuses to write through.
  GeometryColumnClash,
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

// Decimal and eight lowercase hex digits. Hand-rolled, so that no locale can
// reach a byte scav emits.
void string_append_u32(std::string &out, uint32_t value);
void string_append_hex32(std::string &out, uint32_t value);

// Statements ================================================================

// Authored source, kept beside the entities it produced. A Chart and a
// ParsedDocument both index these rows.

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
  // A blank line separated this statement from whatever came before, its own
  // leading comments included. The only whitespace the model records.
  uint32_t blank_before;
};

struct Trivia {
  Span src;  // -> src_bytes; includes the "//"
  CommentPos pos;
};

char const *syntax_stmt_kind_name(StmtKind kind);
char const *syntax_state_kind_name(StateKind kind);
char const *syntax_trans_kind_name(TransKind kind);

// False when the word names no kind. `initial` and `final` are rejected; the
// format reaches those through `*`.
bool syntax_state_kind_from_name(std::string_view text, StateKind &out);
bool syntax_trans_kind_from_name(std::string_view text, TransKind &out);

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
// string verbatim; a loader fills `target`.
struct Include {
  StrRef alias;
  StrRef path;
  DocId target;
  StateId host;  // the alias state it synthesizes; never INVALID
  StmtId stmt;
};

// `stmt` is the attr's own statement, not its subject's: `state On { @doc }` is
// two authored statements.
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

// Resolution ================================================================

// Which rule a failed path broke, not merely that one did.
enum class ResolveStatus : uint32_t { Ok, NotFound, BadQualifier, CrossesInclude };

// Resolves a state path against `scope`: the first segment innermost-outward
// within its own document, later ones descending strictly.
ResolveStatus resolve_path(Chart const &c,
                           SubmachineId scope,
                           std::string_view path,
                           StateId &out);

// Validation ================================================================

// Structural checks over the live rows, a tombstoned one only as a target.
// Appends findings sorted by (code, subject kind, subject ordinal).
bool validate_chart(Chart const &c, std::vector<Diagnostic> &diags);

// Structural hash ===========================================================

// The canonical serialization of a chart's entity arrays and authored text,
// field by field in array order with every string length-prefixed.
void chart_digest_bytes(Chart const &c, std::vector<scav_byte> &out);

// xxHash32 over the above.
uint32_t chart_structural_hash(Chart const &c);

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
// appends another. A later build_attr elsewhere shifts that index.
uint32_t build_attr(Chart &c,
                    ElemRef subject,
                    std::string_view key,
                    std::string_view value);

// Synthesizes the alias host state in `parent` and appends the Include row
// naming it. `path` is stored verbatim; a loader fills `target`.
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

// Sets a self-length (ElemKind::Point) column's row count; growth zero-fills.
// False for any other entity or an id out of range.
bool column_resize(Chart &c, ColumnId id, uint32_t rows);

// Syntax tree ===============================================================

// One document, parsed. `stmt_payload` indexes the array a statement's `kind`
// names; `stmt_children` indexes its block.

// `Flag` is `@k` with nothing after it, kept distinct from `@k = "true"`.
enum class AttrValueKind : uint32_t { Flag, Scalar, List };

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

// Block levels, not state levels: the format spends two blocks per state level
// plus one for the chart, so a depth-16 chart nests 33 deep.
constexpr uint32_t DEFAULT_MAX_DEPTH{ 256 };

struct ParseOptions {
  uint32_t max_depth;
};

ParseOptions parse_default_options();

// normalize -> lex -> parse, over bytes from anywhere. `name` is what a
// diagnostic quotes.
bool parse_document(scav_byte const *bytes,
                    size_t len,
                    std::string_view name,
                    ParseOptions const &opts,
                    ParsedDocument &out,
                    std::vector<Diagnostic> &diags);

// Canonical printing ========================================================

// A parsed document back to text, reconstructed rather than echoed, so two
// documents differing only in formatting print the same bytes. See the README.

// A block fitting inside the budget stays on one line.
constexpr uint32_t PRINT_COLUMNS_MIN{ 20 };
constexpr uint32_t PRINT_COLUMNS_MAX{ 4096 };
constexpr uint32_t DEFAULT_PRINT_COLUMNS{ 90 };

struct PrintOptions {
  uint32_t columns;
};

PrintOptions print_default_options();

bool print_options_validate(PrintOptions const &opts);

// Appends canonical text for `pd`, newline-terminated. False only when the
// options are out of range; a half-parsed document prints the rows it produced.
bool print_document(ParsedDocument const &pd, PrintOptions const &opts, std::string &out);

// Document paths ============================================================

// A document name is a key, resolved by byte-wise segment folding. Names are
// `/`-separated on every transport.

// Resolves `ref` against `base`, assigning `out`. A relative ref joins `base`'s
// directory and folds `.` and `..`; an absolute or scheme-carrying one does not.
bool path_resolve(std::string_view base, std::string_view ref, std::string &out);

// Loader ====================================================================

// Add the root, read `pending`, resolve and add each, repeat until empty,
// finish. Parsed once per distinct name, instantiated once per include.

struct Pending {
  Span path;          // -> the loader's path pool; read with load_pending_path
  DocId from;         // the document whose include statement claimed this one
  uint32_t stmt_row;  // that statement's row in `from`'s parsed document
};

// A DocId is fixed by the first include statement naming that path, ordered by
// (requesting DocId, statement ordinal). Arrival order does not enter into it.
struct LoadDoc {
  StrRef name;        // the resolved key, into Loader::paths
  uint32_t arrived;   // 0 until load_add supplied its bytes
  Span edges;         // -> Loader::edges, this document's includes
  DocId from;         // who claimed it; INVALID for the root
  uint32_t stmt_row;  // the claiming statement's row in `from`
};

// One include statement, discovered. The junction between the document graph
// the loader walks and the instantiation tree finish builds.
struct IncludeEdge {
  DocId from;
  DocId to;
  uint32_t stmt_row;  // that statement's row in `from`'s document
};

// Parsed documents are held until finish, which instantiates from their
// statement payloads.
struct Loader {
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
bool load_add(Loader &loader, scav_byte const *bytes, size_t len, std::string_view name);

// What the loader still needs, in DocId order. The returned view is
// invalidated by the next load_add. Empty means finish.
std::vector<Pending> const &load_pending(Loader &loader);

inline std::string_view load_pending_path(Loader const &loader, Pending const &p) {
  return string_pool_view(loader.paths, str_ref(p.path.off, p.path.len));
}

// The resolved key `doc` was claimed under. Empty for an unknown DocId.
std::string_view load_document_name(Loader const &loader, DocId doc);

// The normalized bytes parsed for `doc`, which a document-local diagnostic's
// span indexes. False for a document still pending.
bool load_document_bytes(Loader const &loader,
                         DocId doc,
                         scav_byte const **out,
                         uint32_t *len);

// Builds a complete network into an empty `out`, spending the loader. Whether
// `out` gained documents says which pool the diagnostics index.
bool load_finish(Loader &loader, Chart &out, std::vector<Diagnostic> &diags);

// Diagnostic rendering ======================================================

// `name:line:col: message`, newline-terminated, appended to `out`. A diagnostic
// carries a code plus a span or a subject, so the position is derived here.

// A finding from before any chart existed -- a parse error, a cycle, a missing
// document -- whose span indexes the bytes the loader still holds.
void diag_append(std::string &out,
                 Loader const &loader,
                 Diagnostic const &d,
                 std::string_view fallback_name);

// A model finding, positioned by walking to its subject's statement. That
// statement often sits in a document other than the one the caller named.
void diag_append(std::string &out,
                 Chart const &chart,
                 Diagnostic const &d,
                 std::string_view fallback_name);

// Filesystem transport ======================================================

// Reads `path` whole. Returns false when it cannot be opened or read.
bool read_file(char const *path, std::vector<scav_byte> &out);

// Writes `bytes` over `path`. Returns false when it cannot be opened, written
// or closed.
bool write_file(char const *path, scav_byte const *bytes, size_t len);

// The load loop with `read_file` in the fetch slot. `loader` outlives the call
// for load_document_bytes; `failed_path` names a document it could not read.
bool load_file(char const *path,
               Loader &loader,
               Chart &out,
               std::vector<Diagnostic> &diags,
               std::string &failed_path);

}  // namespace scav

#endif  // SCAV_CORE_H_INCLUDED
