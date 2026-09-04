// The router registry. Routers cross every boundary by name; the id is an
// index into this table and nothing more.

#include "layout/router.h"

#include "scav/scav_layout.h"
#include "scav/scav_types.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace scav {

namespace {

// Stateless and const, so these are constant-initialised and the table below
// cannot depend on when another translation unit's statics ran.
constinit StraightRouter const STRAIGHT;
constinit OrthogonalRouter const ORTHOGONAL;

// Index 0 is what a caller that has no opinion gets.
constexpr std::array<Router const *, 2> ROUTERS{ { &ORTHOGONAL, &STRAIGHT } };

}  // namespace

Router const *router_at(uint32_t index) {
  return (index < ROUTERS.size()) ? ROUTERS[index] : nullptr;
}

uint32_t router_count() { return static_cast<uint32_t>(ROUTERS.size()); }

bool router_name(uint32_t index, scav_byte const *&out, uint32_t &len) {
  if (index >= ROUTERS.size()) { return false; }
  RouterName const name{ ROUTERS[index]->name() };
  out = reinterpret_cast<scav_byte const *>(name.bytes);
  len = name.len;
  return true;
}

bool router_version(uint32_t index, uint32_t &out) {
  if (index >= ROUTERS.size()) { return false; }
  out = ROUTERS[index]->version();
  return true;
}

bool router_by_name(scav_byte const *name, uint32_t len, scav_router_id &out) {
  if (name == nullptr) { return false; }
  for (uint32_t i = 0; i < ROUTERS.size(); ++i) {
    RouterName const at{ ROUTERS[i]->name() };
    if ((at.len == len) && (std::memcmp(at.bytes, name, len) == 0)) {
      out = i;
      return true;
    }
  }
  return false;
}

}  // namespace scav
