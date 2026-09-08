// The C projection of scav_layout.h. Each function converts its arguments,
// calls one layout function, and converts the result.

#include "scav/scav_layout_c.h"

#include "scav/scav_core_c.h"
#include "scav/scav_layout.h"
#include "scav/scav_types.h"
#include "scav_c_abi.h"
#include "scav_c_handles.h"

#include <cstdint>
#include <cstring>
#include <vector>

static_assert(sizeof(scav_box_space) == 12);
static_assert(sizeof(scav_path_clear) == 8);
static_assert(sizeof(scav_path_box) == 16);
static_assert(sizeof(scav_profile) == 48 * sizeof(int32_t));
// Four {pointer, count, stride} triples and nothing else: the strides landed in
// the padding the pointer-and-count pairs left, so a hole here would mean one of
// them moved.
static_assert(sizeof(scav_spaces) ==
              (4 * (sizeof(scav_box_space const *) + (2 * sizeof(uint32_t)))));

extern "C" {

scav_result scav_profile_named(char const *name, scav_profile *out, uint32_t out_size) {
  if (out_size != sizeof(scav_profile)) { return SCAV_E_ABI; }
  if ((name == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  return scav::profile_named(name, *out) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

scav_result scav_profile_validate(scav_profile const *profile, uint32_t profile_size) {
  if (profile_size != sizeof(scav_profile)) { return SCAV_E_ABI; }
  if (profile == nullptr) { return SCAV_E_INVALID_ARG; }
  return scav::profile_validate(*profile) ? SCAV_OK : SCAV_E_INVALID_ARG;
}

scav_result scav_layout_run(scav_chart *chart,
                            scav_spaces const *spaces,
                            uint32_t spaces_size,
                            scav_layout_opts const *opts,
                            uint32_t opts_size,
                            scav_placed *placed,
                            uint32_t placed_cap,
                            uint32_t placed_size,
                            uint32_t *out_count) {
  if ((spaces_size != sizeof(scav_spaces)) || (opts_size != sizeof(scav_layout_opts)) ||
      (placed_size != sizeof(scav_placed))) {
    return SCAV_E_ABI;
  }
  if ((spaces != nullptr) && !scav::spaces_strides_agree(*spaces)) { return SCAV_E_ABI; }
  if ((chart == nullptr) || (opts == nullptr) || (out_count == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  if (opts->router >= scav::router_count()) { return SCAV_E_INVALID_ARG; }
  scav_spaces const none{};
  scav_spaces const &s{ (spaces != nullptr) ? *spaces : none };
  *out_count = s.n_path_box;
  if ((placed_cap == 0) && (s.n_path_box != 0)) { return SCAV_OK; /* count query */ }
  if (placed_cap < s.n_path_box) { return SCAV_E_CAPACITY; }
  if ((placed == nullptr) && (s.n_path_box != 0)) { return SCAV_E_INVALID_ARG; }

  chart->diags.clear();
  std::vector<scav_placed> rows;
  if (!scav::layout_run(chart->chart, s, *opts, rows, chart->diags)) {
    return SCAV_E_LAYOUT;
  }
  if ((placed != nullptr) && !rows.empty()) {
    std::memcpy(placed, rows.data(), rows.size() * sizeof(scav_placed));
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
