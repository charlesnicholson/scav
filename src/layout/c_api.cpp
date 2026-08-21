// The C projection of scav_layout.h. Each function converts its arguments,
// calls one layout function, and converts the result.

#include "scav/scav_layout_c.h"

#include "scav/scav_core_c.h"
#include "scav/scav_layout.h"
#include "scav/scav_types.h"

#include <cstdint>

static_assert(sizeof(scav_box_space) == 12);
static_assert(sizeof(scav_path_clear) == 8);
static_assert(sizeof(scav_path_box) == 16);
static_assert(sizeof(scav_profile) == 43 * sizeof(int32_t));

extern "C" {

scav_result scav_profile_named(char const *name, scav_profile *out) {
  if ((name == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  return scav::profile_named(name, *out) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

scav_result scav_profile_validate(scav_profile const *profile) {
  if (profile == nullptr) { return SCAV_E_INVALID_ARG; }
  return scav::profile_validate(*profile) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

scav_result scav_router_list(uint32_t *out_count) {
  if (out_count == nullptr) { return SCAV_E_INVALID_ARG; }
  *out_count = scav::router_count();
  return SCAV_OK;
}

scav_result scav_router_name(uint32_t index, scav_byte const **out, uint32_t *out_len) {
  if ((out == nullptr) || (out_len == nullptr)) { return SCAV_E_INVALID_ARG; }
  return scav::router_name(index, *out, *out_len) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

scav_result scav_router_by_name(scav_byte const *name,
                                uint32_t len,
                                scav_router_id *out) {
  if ((name == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  return scav::router_by_name(name, len, *out) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

} /* extern "C" */
