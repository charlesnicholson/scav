#ifndef SCAV_LAYOUT_H_INCLUDED
#define SCAV_LAYOUT_H_INCLUDED

// libscavlayout's public API: validation and digest over the space tables,
// the shipped profiles, and the router registry, over the C ABI's own PODs.

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

// Space requests ============================================================

// A quarter of the coordinate domain, so a legal request cannot compose into
// an illegal box: the box formula adds requests, padding, and packed children.
inline constexpr int32_t SPACE_MAX{ COORD_MAX / 4 };

// Every row of every table against the domain, findings sorted by (code,
// kind, ordinal). False when it found anything; the caller rejects, never clamps.
bool spaces_validate(Chart const &c,
                     scav_spaces const &s,
                     std::vector<Diagnostic> &diags);

// xxh32 over counts and rows, field by field. A hashed layout input: a golden
// is reproducible only against a stated measurement policy.
uint32_t spaces_digest(scav_spaces const &s);

// Profile ===================================================================

// Fills `out` from a shipped profile, "compact" or "readable". False for an
// unknown name, writing nothing.
bool profile_named(char const *name, scav_profile &out);

// Every bound in the C header's table. scav_layout_run revalidates regardless.
bool profile_validate(scav_profile const &p);

// Routers ===================================================================

uint32_t router_count();

// The name of the router at `index`. False past the end.
bool router_name(uint32_t index, scav_byte const *&out, uint32_t &len);

// False when nothing registered has that name.
bool router_by_name(scav_byte const *name, uint32_t len, scav_router_id &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_H_INCLUDED
