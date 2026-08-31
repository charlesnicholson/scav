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
bool spaces_validate(Chart const &c, scav_spaces const &s, std::vector<Diagnostic> &diags);

// xxh32 over counts and rows, field by field. A hashed layout input: a golden
// is reproducible only against a stated measurement policy.
uint32_t spaces_digest(scav_spaces const &s);

// Profile ===================================================================

// Fills `out` from a shipped profile, "compact" or "readable". False for an
// unknown name, writing nothing.
bool profile_named(char const *name, scav_profile &out);

// Every bound in the C header's table. scav_layout_run revalidates regardless.
bool profile_validate(scav_profile const &p);

// Layout ====================================================================

// Decomposes, sizes, places, and routes: the box formula bottom-up,
// document-order stacking top-down, straight lines through the ports. Writes
// the geometry columns and sizes `placed` to the path boxes. False leaves the
// columns holding the last successful run, findings sorted like a validator's.
bool layout_run(Chart &c,
                scav_spaces const &s,
                scav_layout_opts const &o,
                std::vector<scav_placed> &placed,
                std::vector<Diagnostic> &diags);

// Split so a pure translation moves the coordinate hash and never the
// structural one: structure is port sides, depths, and per-route direction
// tokens; coordinates are every rect and point. Unlaid charts hash as empty.
uint32_t layout_structural_hash(Chart const &c);
uint32_t layout_coordinate_hash(Chart const &c);

// Everything the run depended on that is not geometry: the profile, the
// router's name and version, and the space tables -- through which the font
// reaches a digest it cannot be an argument to. A third value rather than a
// seed for the two above, which would move the structural hash on any space
// change and cost the split its whole point. 0 on a chart never laid out.
uint32_t layout_inputs_digest(Chart const &c);

// Routers ===================================================================

uint32_t router_count();

// The name of the router at `index`. False past the end.
bool router_name(uint32_t index, scav_byte const *&out, uint32_t &len);

// Its version, bumped whenever its output moves. False past the end.
bool router_version(uint32_t index, uint32_t &out);

// False when nothing registered has that name.
bool router_by_name(scav_byte const *name, uint32_t len, scav_router_id &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_H_INCLUDED
