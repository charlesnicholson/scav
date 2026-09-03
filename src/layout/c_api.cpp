// The C projection of scav_layout.h. Each function converts its arguments,
// calls one layout function, and converts the result.

#include "scav/scav_layout_c.h"

#include "scav/scav_core_c.h"
#include "scav/scav_layout.h"
#include "scav/scav_types.h"
#include "scav_c_handles.h"

#include <cstdint>
#include <cstring>
#include <vector>

static_assert(sizeof(scav_box_space) == 12);
static_assert(sizeof(scav_path_clear) == 8);
static_assert(sizeof(scav_path_box) == 16);
static_assert(sizeof(scav_profile) == 46 * sizeof(int32_t));

extern "C" {

scav_result scav_profile_named(char const *name, scav_profile *out) {
  if ((name == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  return scav::profile_named(name, *out) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

scav_result scav_profile_validate(scav_profile const *profile) {
  if (profile == nullptr) { return SCAV_E_INVALID_ARG; }
  return scav::profile_validate(*profile) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

scav_result scav_layout_run(scav_chart *chart,
                            scav_spaces const *spaces,
                            scav_layout_opts const *opts,
                            scav_placed *out_placed,
                            uint32_t cap,
                            uint32_t *out_count) {
  if ((chart == nullptr) || (opts == nullptr) || (out_count == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  if (opts->router >= scav::router_count()) { return SCAV_E_INVALID_ARG; }
  scav_spaces const none{};
  scav_spaces const &s{ (spaces != nullptr) ? *spaces : none };
  *out_count = s.n_path_box;
  if ((cap == 0) && (s.n_path_box != 0)) { return SCAV_OK; /* count query */ }
  if (cap < s.n_path_box) { return SCAV_E_CAPACITY; }
  if ((out_placed == nullptr) && (s.n_path_box != 0)) { return SCAV_E_INVALID_ARG; }

  chart->diags.clear();
  std::vector<scav_placed> placed;
  if (!scav::layout_run(chart->chart, s, *opts, placed, chart->diags)) {
    return SCAV_E_LAYOUT;
  }
  if ((out_placed != nullptr) && !placed.empty()) {
    std::memcpy(out_placed, placed.data(), placed.size() * sizeof(scav_placed));
  }
  return SCAV_OK;
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

scav_result scav_router_by_name(scav_byte const *name, uint32_t len, scav_router_id *out) {
  if ((name == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  return scav::router_by_name(name, len, *out) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

} /* extern "C" */
