#ifndef SCAV_LAYOUT_C_H_INCLUDED
#define SCAV_LAYOUT_C_H_INCLUDED

/* libscavlayout's C API: the space tables an application fills, the profile
 * that parameterizes layout, and the router registry. Every type is a flat POD
 * of fixed-width integers, so the same structs serve C++ callers directly. */

#include "scav/scav_types.h"

/* NOLINTNEXTLINE(modernize-deprecated-headers) -- this header must compile as C */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using, readability-identifier-naming) --
 * `typedef` is the C spelling, and an ABI type's tag is its name. */

typedef uint32_t scav_router_id;

/* One row of the layout out-param, parallel to the path boxes; w and h may
 * exceed what was requested. */
typedef scav_rect scav_placed;

/* Interior space a state or submachine box must make room for: a width floor,
 * and heights reserved before and after the packed-submachine area. All-zero
 * requests nothing. */
typedef struct {
  int32_t min_w;
  int32_t h_before;
  int32_t h_after;
} scav_box_space;

/* Route shortening at each end, for arrowheads and terminal glyphs. */
typedef struct {
  int32_t src, dst;
} scav_path_clear;

/* A rect layout slides along `subject`'s route. `order` positions it among the
 * boxes sharing that route and must be unique per subject. */
typedef struct {
  uint32_t subject; /* TransId ordinal */
  int32_t w, h;
  uint32_t order;
} scav_path_box;

/* The three space tables. The app owns every array; scav only reads. box and
 * clear counts either match their entity array or are zero; path boxes are
 * 0..N per transition. */
typedef struct {
  scav_box_space const *box_state;
  uint32_t n_box_state;
  scav_box_space const *box_sub;
  uint32_t n_box_sub;
  scav_path_clear const *path_clear;
  uint32_t n_path_clear;
  scav_path_box const *path_box;
  uint32_t n_path_box;
} scav_spaces;

/* Every knob layout reads, flat int32 with no padding, so the bytes hash into
 * a golden directly. Bounds, validated by scav_profile_validate and rejected
 * out of range:
 *
 *   pad, kind_min_*, spacing_inflation_increment    [0, COORD_MAX/4]
 *   font_size_grid                                  [1, COORD_MAX/4]
 *   line_height_k_num, _k_den, dar_num, dar_den     [1, 1024]
 *   trybox, sm_tiebreak                             [0, 1]
 *   w_*                                             [0, 1024]
 *   portfolio_k                                     [1, 64]
 *   sweep_count, congestion_iterations, ripup_cap,
 *   spacing_inflation_cap                           [0, 1024]
 *   print_columns                                   [20, 4096]
 *   profile_id                                      [0, INT32_MAX]
 *   profile_version                                 [1, INT32_MAX]
 *
 * Weight ceilings keep the summed cost inside int64; bounded dar and k keep
 * their products inside the isqrt and line-height intermediates. */
typedef struct {
  int32_t profile_id;
  int32_t profile_version;

  int32_t pad; /* interior padding on every composed box */

  int32_t font_size_grid; /* 1/16 pt units */
  int32_t line_height_k_num;
  int32_t line_height_k_den;

  /* Minimum extents indexed by StateKind ordinal; fork and join are wide and
   * thin, and nothing scales with arity. */
  int32_t kind_min_w[9];
  int32_t kind_min_h[9];

  int32_t dar_num; /* desired aspect ratio, held as a pair */
  int32_t dar_den;
  int32_t trybox;      /* 1 = evaluate the box packer alongside */
  int32_t sm_tiebreak; /* 0 = area then aspect, 1 = aspect then area */

  /* Tier-2 weights, highest to lowest. */
  int32_t w_bends;
  int32_t w_corridor;
  int32_t w_crossings;
  int32_t w_excess_len;
  int32_t w_adjacency;
  int32_t w_label;
  int32_t w_aspect;
  int32_t w_area;

  int32_t portfolio_k;
  int32_t sweep_count;
  int32_t congestion_iterations;
  int32_t ripup_cap;
  int32_t spacing_inflation_cap;
  int32_t spacing_inflation_increment;

  int32_t print_columns; /* the canonical printer's line-break budget */
} scav_profile;

/* NOLINTEND(modernize-use-using, readability-identifier-naming) */

/* Fills `out` from a shipped profile: "compact" or "readable". An unknown name
 * is SCAV_E_INVALID_ARG and writes nothing. */
scav_result scav_profile_named(char const *name, scav_profile *out);

/* Every bound above, checked; scav_layout_run revalidates regardless. */
scav_result scav_profile_validate(scav_profile const *profile);

/* Routers cross the ABI by name only; function pointers never do. */
scav_result scav_router_list(uint32_t *out_count);
scav_result scav_router_name(uint32_t index, scav_byte const **out, uint32_t *out_len);
scav_result scav_router_by_name(scav_byte const *name,
                                uint32_t len,
                                scav_router_id *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_LAYOUT_C_H_INCLUDED */
