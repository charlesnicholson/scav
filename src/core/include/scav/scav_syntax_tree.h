#ifndef SCAV_SYNTAX_TREE_H_INCLUDED
#define SCAV_SYNTAX_TREE_H_INCLUDED

// Functions here are named `syntax_*`, after the header; the POD rows keep
// the names PRD 7 gives them.
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

#include "scav/scav_ids.h"
#include "scav/scav_string_pool.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

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

}  // namespace scav

#endif  // SCAV_SYNTAX_TREE_H_INCLUDED
