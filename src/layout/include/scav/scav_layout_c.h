#ifndef SCAV_LAYOUT_C_H_INCLUDED
#define SCAV_LAYOUT_C_H_INCLUDED

/* libscavlayout's C API over fixed-width PODs: space tables, profile, layout, routers.
 * Each POD's size argument is checked first, NULL or not; a mismatch is SCAV_E_ABI. */

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

/* Interior space a state or submachine box reserves; all-zero requests nothing.
 * Each band is an obstacle: routes stay out of it and ports off the face it lines. */
typedef struct {
  int32_t min_w;    /* interior at least this wide */
  int32_t h_before; /* height reserved before the packed-submachine area */
  int32_t h_after;  /* ... after */
  int32_t w_before; /* width reserved before it, between the two height bands */
  int32_t w_after;  /* ... after */
  uint32_t ruled;   /* bit k set: the k-th band above has a drawn inner edge */
} scav_box_space;

/* The least straight run a route keeps at each end, for arrowheads and terminal glyphs. */
typedef struct {
  int32_t src, dst;
} scav_path_clear;

/* A rect layout places along `subject`'s route. */
typedef struct {
  uint32_t subject; /* TransId ordinal */
  int32_t w, h;
  uint32_t order; /* position among the subject's boxes; unique per subject */
} scav_path_box;

/* Caller-owned tables scav only reads. Box and clear counts match their entity array
 * or are 0; path boxes are 0..N per transition. Each stride must equal its row's
 * sizeof, even for an empty table. */
typedef struct {
  scav_box_space const *box_state;
  uint32_t n_box_state;
  uint32_t box_state_stride;
  scav_box_space const *box_sub;
  uint32_t n_box_sub;
  uint32_t box_sub_stride;
  scav_path_clear const *path_clear;
  uint32_t n_path_clear;
  uint32_t path_clear_stride;
  scav_path_box const *path_box;
  uint32_t n_path_box;
  uint32_t path_box_stride;
} scav_spaces;

/* Every knob layout reads, as int32 fields with no padding. Each field's comment
 * gives its valid range. */
typedef struct {
  int32_t profile_id;      /* [0, INT32_MAX] */
  int32_t profile_version; /* [1, INT32_MAX] */

  int32_t pad; /* a box's interior ring; [0, COORD_MAX/4] */

  /* Gaps between two things; [0, COORD_MAX/4] each. */
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

  /* Tier-2 weights, in CostTerms order; each [0, 1024]. */
  int32_t w_bends;
  int32_t w_corridor;
  int32_t w_crossings;
  int32_t w_excess_len;
  int32_t w_adjacency;
  int32_t w_label;
  int32_t w_label_near;
  int32_t w_aspect;
  int32_t w_area;
  int32_t w_crowding;
  int32_t w_length;
  int32_t w_transit_bends;
  int32_t w_whitespace;
  int32_t w_backward_starts;
  int32_t w_leaf_aspect;

  int32_t portfolio_k;                 /* bounded moves scored; [0, 2^20] */
  int32_t portfolio_m;                 /* chart-global phase-2 tuples; [1, 16] */
  int32_t search_cull;                 /* 0 the full search, 1 culled (shipped); [0, 1] */
  int32_t kick_rows;                   /* rows the culled search kicks; [1, 16] */
  int32_t jitter_seed;                 /* nonzero breaks near-ties; [0, INT32_MAX] */
  int32_t sweep_count;                 /* [0, 1024] */
  int32_t spacing_inflation_cap;       /* [0, 1024] */
  int32_t spacing_inflation_increment; /* [0, COORD_MAX/4] */
} scav_profile;

/* One row of the portslot geometry column. */
typedef struct {
  int32_t x, y;            /* the port */
  uint32_t side;           /* face crossed: 0 left, 1 right, 2 top, 3 bottom */
  uint32_t boundary_depth; /* state borders enclosing that boundary */
} scav_port_slot;

typedef struct {
  scav_profile profile;
  scav_router_id router;
  /* Worker threads for the sharded phases and search candidates; 0 uses the host's
   * concurrency. Any value is legal and gives the same output. */
  uint32_t threads;
} scav_layout_opts;

/* NOLINTEND(modernize-use-using, readability-identifier-naming) */

/* Fills `out` from a shipped profile, "compact" or "readable". An unknown name is
 * SCAV_E_INVALID_ARG, a wrong `out_size` SCAV_E_ABI; neither writes anything. */
scav_result scav_profile_named(char const *name, scav_profile *out, uint32_t out_size);

/* SCAV_E_INVALID_ARG when a field is outside its bound above. */
scav_result scav_profile_validate(scav_profile const *profile, uint32_t profile_size);

/* Validates the profile and spaces, runs layout, writes the geometry columns, and
 * fills `placed` parallel to the path boxes. NULL `spaces` requests nothing.
 * placed_cap 0 with boxes pending queries the count; too small is SCAV_E_CAPACITY.
 * SCAV_E_LAYOUT: findings on scav_chart_diag; columns keep the last successful run.
 * SCAV_OK may also leave findings: one per transition the router could not thread,
 * drawn as a straight line. The three sizes and the `spaces` strides are checked
 * first; a mismatch is SCAV_E_ABI, writing nothing. `opts_size` covers the profile. */
scav_result scav_layout_run(scav_chart *chart,
                            scav_spaces const *spaces,
                            uint32_t spaces_size,
                            scav_layout_opts const *opts,
                            uint32_t opts_size,
                            scav_placed *placed,
                            uint32_t placed_cap,
                            uint32_t placed_size,
                            uint32_t *out_count);

/* The router registry: count, name by index, id by name. */
scav_result scav_router_list(uint32_t *out_count);
scav_result scav_router_name(uint32_t index, scav_byte const **out, uint32_t *out_len);
scav_result scav_router_by_name(scav_byte const *name, uint32_t len, scav_router_id *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_LAYOUT_C_H_INCLUDED */
