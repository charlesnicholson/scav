#ifndef SCAV_CORE_MODEL_INTERNAL_H_INCLUDED
#define SCAV_CORE_MODEL_INTERNAL_H_INCLUDED

// Cross-TU internals of the model spine. Private header: consumers get the
// public API in scav/scav_core.h, and nothing here survives to the install
// tree.

#include "scav/scav_core.h"

namespace scav {

// Appends one zero-filled row to every column registered for `entity`. The
// builder calls this on every entity append, which is what keeps columns
// index-aligned with their entity array -- the lockstep half of what the core
// owes an extension.
void columns_append_entity_row(Chart &c, ElemKind entity);

}  // namespace scav

#endif  // SCAV_CORE_MODEL_INTERNAL_H_INCLUDED
