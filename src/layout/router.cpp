// The router registry. Routers cross every boundary by name; the id is an
// index into this table and nothing more.

#include "scav/scav_layout.h"
#include "scav/scav_types.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace scav {

namespace {

// `name` points at a string literal: static storage, so a handed-out name
// lives as long as the process. The constexpr table cannot compile otherwise.
struct RouterRow {
  char const *name;
  uint32_t name_len;
};

constexpr std::array<RouterRow, 1> ROUTERS{ {
    { .name = "straight", .name_len = 8 },
} };

}  // namespace

uint32_t router_count() { return static_cast<uint32_t>(ROUTERS.size()); }

bool router_name(uint32_t index, scav_byte const *&out, uint32_t &len) {
  if (index >= ROUTERS.size()) { return false; }
  out = reinterpret_cast<scav_byte const *>(ROUTERS[index].name);
  len = ROUTERS[index].name_len;
  return true;
}

bool router_by_name(scav_byte const *name, uint32_t len, scav_router_id &out) {
  if (name == nullptr) { return false; }
  for (uint32_t i = 0; i < ROUTERS.size(); ++i) {
    if ((ROUTERS[i].name_len == len) && (std::memcmp(ROUTERS[i].name, name, len) == 0)) {
      out = i;
      return true;
    }
  }
  return false;
}

}  // namespace scav
