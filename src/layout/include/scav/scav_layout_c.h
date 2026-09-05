#ifndef SCAV_LAYOUT_C_H_INCLUDED
#define SCAV_LAYOUT_C_H_INCLUDED

/* libscavlayout's C API: the space tables an application fills, the profile,
 * and the router registry. Flat PODs of fixed-width integers, C++ uses them too. */

#include "scav/scav_core_c.h"
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

/* Interior space a state or submachine box must make room for; all-zero
 * requests nothing. */
typedef struct {
  int32_t min_w;    /* interior at least this wide */
  int32_t h_before; /* height reserved before the packed-submachine area */
  int32_t h_after;  /* ... after */
} scav_box_space;

/* Route shortening at each end, for arrowheads and terminal glyphs. */
typedef struct {
  int32_t src, dst;
} scav_path_clear;

/* A rect layout slides along `subject`'s route. */
typedef struct {
  uint32_t subject; /* TransId ordinal */
  int32_t w, h;
  uint32_t order; /* position among the subject's boxes; unique per subject */
} scav_path_box;

/* The app owns every array; scav only reads. Box and clear counts match their
 * entity array or are zero; path boxes are 0..N per transition. */
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
 * a golden directly. Each field's bound rides it; validate rejects the rest. */
typedef struct {
  int32_t profile_id;      /* [0, INT32_MAX] */
  int32_t profile_version; /* [1, INT32_MAX] */

  int32_t pad; /* a box's interior ring only; [0, COORD_MAX/4] */

  /* Gaps between two things, as against `pad`, which is inside one thing.
   * [0, COORD_MAX/4] each. */
  int32_t rank_sep; /* adjacent ranks, along the layering axis */
  int32_t node_sep; /* adjacent nodes within one rank */
  int32_t sub_sep;  /* packed sibling submachines */

  int32_t font_size_grid;    /* 1/16 pt units; [1, COORD_MAX/4] */
  int32_t line_height_k_num; /* [1, 1024] */
  int32_t line_height_k_den; /* [1, 1024] */

  /* Min extents by StateKind ordinal; fork/join wide and thin. [0, COORD_MAX/4] */
  int32_t kind_min_w[9];
  int32_t kind_min_h[9];

  int32_t dar_num; /* desired aspect ratio as a pair; each [1, 1024] */
  int32_t dar_den;
  int32_t trybox;      /* 1 = evaluate the box packer alongside; [0, 1] */
  int32_t sm_tiebreak; /* 0 = area then aspect, 1 = reversed; [0, 1] */

  /* Tier-2 weights, highest to lowest; each [0, 1024], which keeps the summed
   * cost inside int64. */
  int32_t w_bends;
  int32_t w_corridor;
  int32_t w_crossings;
  int32_t w_excess_len;
  int32_t w_adjacency;
  int32_t w_label;
  int32_t w_aspect;
  int32_t w_area;

  int32_t portfolio_k;                 /* [1, 64] */
  int32_t sweep_count;                 /* [0, 1024] */
  int32_t congestion_iterations;       /* [0, 1024] */
  int32_t ripup_cap;                   /* [0, 1024] */
  int32_t spacing_inflation_cap;       /* [0, 1024] */
  int32_t spacing_inflation_increment; /* [0, COORD_MAX/4] */

  int32_t print_columns; /* the printer's line-break budget; [20, 4096] */
} scav_profile;

/* One row of the portslot geometry column: a port's coordinate, which side of
 * its boundary rect it sits on (0 left, 1 right, 2 top, 3 bottom), and how
 * many state borders enclose that boundary. */
typedef struct {
  int32_t x, y;
  uint32_t side;
  uint32_t boundary_depth;
} scav_port_slot;

typedef struct {
  scav_profile profile;
  scav_router_id router;
  uint32_t threads; /* scheduling only; ignored */
} scav_layout_opts;

/* NOLINTEND(modernize-use-using, readability-identifier-naming) */

/* Fills `out` from a shipped profile: "compact" or "readable". An unknown name
 * is SCAV_E_INVALID_ARG and writes nothing. */
scav_result scav_profile_named(char const *name, scav_profile *out);

/* Every bound above, checked; scav_layout_run revalidates regardless. */
scav_result scav_profile_validate(scav_profile const *profile);

/* Validates the profile and spaces, runs layout, writes the geometry columns,
 * and fills `out_placed` parallel to the path boxes. NULL `spaces` means no
 * requests. cap = 0 with boxes pending queries the required count; a cap too
 * small is SCAV_E_CAPACITY. SCAV_E_LAYOUT means findings await on the chart's
 * scav_chart_diag, and the columns keep the last successful run's values.
 *
 * SCAV_OK leaves findings there as well, marking geometry that was written: one
 * per transition the router could not thread even after widening the spacing,
 * whose route is the straight line between its ends. Read them, draw anyway. */
scav_result scav_layout_run(scav_chart *chart,
                            scav_spaces const *spaces,
                            scav_layout_opts const *opts,
                            scav_placed *out_placed,
                            uint32_t cap,
                            uint32_t *out_count);

/* Routers cross the ABI by name only; function pointers never do. */
scav_result scav_router_list(uint32_t *out_count);
scav_result scav_router_name(uint32_t index, scav_byte const **out, uint32_t *out_len);
scav_result scav_router_by_name(scav_byte const *name, uint32_t len, scav_router_id *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_LAYOUT_C_H_INCLUDED */
