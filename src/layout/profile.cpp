// The shipped profiles as data, and the bound check every consumer of a
// profile runs before trusting one.

#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <cstdint>
#include <cstring>

namespace scav {

namespace {

constexpr int32_t PT{ 16 };  // grid units are 1/16 pt

// One of the two shipped profiles scav_profile_named hands out by name.
constexpr scav_profile READABLE{
  .profile_id = 2,
  .profile_version = 4,
  .pad = 8 * PT,
  .rank_sep = 36 * PT,
  .node_sep = 18 * PT,
  .sub_sep = 12 * PT,
  .font_size_grid = 12 * PT,
  .line_height_k_num = 7,
  .line_height_k_den = 5,
  // StateKind order: Normal, Initial, Final, Choice, Junction, Fork, Join,
  // History, DeepHistory. The fork and join bars are thin across the layering
  // axis and long across the other, because ranks run in +x (11.3): an edge
  // reaches them on a left or right face, and those are the faces that must be
  // long enough to hold a fan-out.
  .kind_min_w = { 40 * PT,
                  14 * PT,
                  14 * PT,
                  20 * PT,
                  12 * PT,
                  4 * PT,
                  4 * PT,
                  16 * PT,
                  16 * PT },
  .kind_min_h = { 24 * PT,
                  14 * PT,
                  14 * PT,
                  20 * PT,
                  12 * PT,
                  60 * PT,
                  60 * PT,
                  16 * PT,
                  16 * PT },
  .dar_num = 16,
  .dar_den = 10,
  .trybox = 1,
  .sm_tiebreak = 0,
  .w_bends = 64,
  .w_corridor = 48,
  .w_crossings = 32,
  .w_excess_len = 4,
  .w_adjacency = 16,
  .w_label = 24,
  .w_label_near = 48,
  .w_aspect = 2,
  .w_area = 1,
  .portfolio_k = 4,
  .sweep_count = 8,
  .congestion_iterations = 8,
  .ripup_cap = 16,
  .spacing_inflation_cap = 8,
  .spacing_inflation_increment = 2 * PT,
  .print_columns = static_cast<int32_t>(DEFAULT_PRINT_COLUMNS),
};

// The other shipped profile: tighter spacing and type for dense charts.
constexpr scav_profile COMPACT{
  .profile_id = 1,
  .profile_version = 4,
  .pad = 4 * PT,
  .rank_sep = 22 * PT,
  .node_sep = 11 * PT,
  .sub_sep = 8 * PT,
  .font_size_grid = 10 * PT,
  .line_height_k_num = 6,
  .line_height_k_den = 5,
  .kind_min_w = { 32 * PT,
                  12 * PT,
                  12 * PT,
                  16 * PT,
                  10 * PT,
                  3 * PT,
                  3 * PT,
                  14 * PT,
                  14 * PT },
  .kind_min_h = { 18 * PT,
                  12 * PT,
                  12 * PT,
                  16 * PT,
                  10 * PT,
                  48 * PT,
                  48 * PT,
                  14 * PT,
                  14 * PT },
  .dar_num = 4,
  .dar_den = 3,
  .trybox = 1,
  .sm_tiebreak = 0,
  .w_bends = 64,
  .w_corridor = 48,
  .w_crossings = 32,
  .w_excess_len = 4,
  .w_adjacency = 16,
  .w_label = 24,
  .w_label_near = 48,
  .w_aspect = 2,
  .w_area = 1,
  .portfolio_k = 4,
  .sweep_count = 8,
  .congestion_iterations = 8,
  .ripup_cap = 16,
  .spacing_inflation_cap = 8,
  .spacing_inflation_increment = 2 * PT,
  .print_columns = static_cast<int32_t>(DEFAULT_PRINT_COLUMNS),
};

bool in_range(int32_t v, int32_t lo, int32_t hi) { return (v >= lo) && (v <= hi); }

}  // namespace

bool profile_named(char const *name, scav_profile &out) {
  if (name == nullptr) { return false; }
  if (std::strcmp(name, "readable") == 0) {
    out = READABLE;
    return true;
  }
  if (std::strcmp(name, "compact") == 0) {
    out = COMPACT;
    return true;
  }
  return false;
}

bool profile_validate(scav_profile const &p) {
  constexpr int32_t I32_MAX{ 0x7FFF'FFFF };
  bool ok{ in_range(p.profile_id, 0, I32_MAX) && in_range(p.profile_version, 1, I32_MAX) &&
           in_range(p.pad, 0, SPACE_MAX) && in_range(p.rank_sep, 0, SPACE_MAX) &&
           in_range(p.node_sep, 0, SPACE_MAX) && in_range(p.sub_sep, 0, SPACE_MAX) &&
           in_range(p.font_size_grid, 1, SPACE_MAX) &&
           in_range(p.line_height_k_num, 1, 1024) &&
           in_range(p.line_height_k_den, 1, 1024) && in_range(p.dar_num, 1, 1024) &&
           in_range(p.dar_den, 1, 1024) && in_range(p.trybox, 0, 1) &&
           in_range(p.sm_tiebreak, 0, 1) && in_range(p.w_bends, 0, 1024) &&
           in_range(p.w_corridor, 0, 1024) && in_range(p.w_crossings, 0, 1024) &&
           in_range(p.w_excess_len, 0, 1024) && in_range(p.w_adjacency, 0, 1024) &&
           in_range(p.w_label, 0, 1024) && in_range(p.w_label_near, 0, 1024) &&
           in_range(p.w_aspect, 0, 1024) && in_range(p.w_area, 0, 1024) &&
           in_range(p.portfolio_k, 1, 64) && in_range(p.sweep_count, 0, 1024) &&
           in_range(p.congestion_iterations, 0, 1024) && in_range(p.ripup_cap, 0, 1024) &&
           in_range(p.spacing_inflation_cap, 0, 1024) &&
           in_range(p.spacing_inflation_increment, 0, SPACE_MAX) &&
           in_range(p.print_columns,
                    static_cast<int32_t>(PRINT_COLUMNS_MIN),
                    static_cast<int32_t>(PRINT_COLUMNS_MAX)) };
  for (int32_t const v : p.kind_min_w) { ok = ok && in_range(v, 0, SPACE_MAX); }
  for (int32_t const v : p.kind_min_h) { ok = ok && in_range(v, 0, SPACE_MAX); }
  return ok;
}

}  // namespace scav
