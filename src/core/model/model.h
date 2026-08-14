#ifndef SCAV_CORE_MODEL_MODEL_H_INCLUDED
#define SCAV_CORE_MODEL_MODEL_H_INCLUDED

// Cross-TU internals of the model spine. Private by location, not by name:
// nothing outside src/ can reach this path, and nothing here survives to the
// install tree.

#include "scav/scav_core.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace scav {

// Appends one zero-filled row to every column registered for `entity`. The
// builder calls this on every entity append, which is what keeps columns
// index-aligned with their entity array -- the lockstep half of what the core
// owes an extension.
void columns_append_entity_row(Chart &c, ElemKind entity);

// One path segment for `id`: its authored name, or the `$kind` synthetic
// spelling with a stable ordinal. Shared by chart_path_of and the resolver so
// an address prints and parses through the same bytes.
void model_state_segment(Chart const &c, StateId id, std::string &out);

// Append the row and grow its columns, and nothing else: the caller owes the
// containment spans. build_state's per-call span insert is O(shift), so a bulk
// producer -- lowering -- appends rows through these and rebuilds the spans
// once, which is §7.3's rebuild rule applied at scale.
StateId model_append_state_row(Chart &c, State const &row);
SubmachineId model_append_submachine_row(Chart &c, Submachine const &row);

// A pre-split path segment, so lowering can resolve straight from PathSeg rows
// without printing text it would immediately re-parse.
struct ResolveSeg {
  std::string_view name;
  std::string_view qualifier;  // empty when absent or numeric
  uint32_t ordinal;            // INVALID when absent or named
};

ResolveStatus resolve_segments(Chart const &c,
                               SubmachineId scope,
                               ResolveSeg const *segs,
                               uint32_t count,
                               StateId &out);

}  // namespace scav

#endif  // SCAV_CORE_MODEL_MODEL_H_INCLUDED
