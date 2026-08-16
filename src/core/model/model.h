#ifndef SCAV_CORE_MODEL_MODEL_H_INCLUDED
#define SCAV_CORE_MODEL_MODEL_H_INCLUDED

// Cross-TU internals of the model. Private by location: nothing outside src/
// can reach this path, and nothing here is installed.

#include "scav/scav_core.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace scav {

// Appends one zero-filled row to every column registered for `entity`, keeping
// them index-aligned with the array they parallel.
void model_append_column_rows(Chart &c, ElemKind entity);

// One path segment for `id`: its authored name, or `$kind` with a stable
// ordinal. Shared by chart_path_of and the resolver.
void model_state_segment(Chart const &c, StateId id, std::string &out);

// Appends the row and grows its columns; the caller owes the containment spans.
// A bulk producer uses these and rebuilds the spans once.
StateId model_append_state_row(Chart &c, State const &row);
SubmachineId model_append_submachine_row(Chart &c, Submachine const &row);

// A pre-split path segment, so a caller holding PathSeg rows resolves without
// printing text it would immediately re-parse.
struct ResolveSeg {
  std::string_view name;
  std::string_view qualifier;  // empty when absent or numeric
  uint32_t ordinal;            // INVALID when absent or named
};

ResolveStatus model_resolve_segments(Chart const &c,
                                     SubmachineId scope,
                                     ResolveSeg const *segs,
                                     uint32_t count,
                                     StateId &out);

// Lowering's four steps. lower_document runs them once each; a load session
// runs 1 per file, 2 per instantiation, and 3 and 4 once per network.

// 1. The front-end slice of `pd`, rebased into the chart's shared pools.
// Returns the DocId and the base a pd statement row adds to become a StmtId.
DocId model_attach_document(Chart &c, ParsedDocument const &pd, uint32_t &stmt_base);

// What to instantiate and where to hang it. A root document has no host; any
// other attaches under the alias state its include synthesized.
struct InstJob {
  DocId doc;
  InstId inst;   // INVALID in the root document
  StateId host;  // INVALID in the root document
  uint32_t stmt_base;
};

// A transition statement held until every document is attached, with the
// lexical scope its wildcards synthesize into and its paths resolve from.
struct PendingTrans {
  uint32_t row;  // -> the parsed document `doc` names
  DocId doc;
  InstId inst;
  SubmachineId scope;
  uint32_t stmt_base;
};

// An include row whose target the caller must fill. The entity pass has no
// document graph to consult.
struct PendingInc {
  uint32_t row;  // the include statement's row in `doc`
  DocId doc;
  InstId inst;
};

// 2. One instantiation's entity rows: states, submachines, includes, attrs.
// Include rows land with `target` INVALID and are reported through `incs`.
bool model_instantiate(Chart &c,
                       ParsedDocument const &pd,
                       InstJob const &job,
                       std::vector<PendingTrans> &trans,
                       std::vector<PendingInc> &incs,
                       std::vector<Diagnostic> &diags);

// 3. Every containment span, rebuilt in one pass. Idempotent, and ordering
// children by state ordinal agrees with step 4's wildcard splice.
void model_finalize_containment(Chart &c);

// 4. Endpoints, wildcards, and the transition rows, over the whole network.
// `docs` is indexed by DocId and must cover every doc a PendingTrans names.
bool model_resolve_transitions(Chart &c,
                               ParsedDocument const *const *docs,
                               uint32_t doc_count,
                               std::vector<PendingTrans> const &pending,
                               std::vector<Diagnostic> &diags);

}  // namespace scav

#endif  // SCAV_CORE_MODEL_MODEL_H_INCLUDED
