#ifndef SCAV_DRAW_C_H_INCLUDED
#define SCAV_DRAW_C_H_INCLUDED

/* libscavdraw's C API. Each caller-owned POD or row array passes its size, checked first,
 * NULL or not; a mismatch is SCAV_E_ABI. Each returned array reports its row stride. */

#include "scav/scav_core_c.h"
/* The space tables and the placed boxes, which is where a label's rect is. */
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

/* NOLINTNEXTLINE(modernize-deprecated-headers) -- this header must compile as C */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using, readability-identifier-naming) --
 * `typedef` is the C spelling, and a handle's tag is its ABI name. */

typedef struct scav_metrics scav_metrics;
typedef struct scav_images scav_images;
typedef struct scav_drawlist scav_drawlist;

/* Primitive kinds. Each kind fixes the meaning of `points`, `a` and `b`. */
enum {
  SCAV_PRIM_RECT = 0,     /* 2 points: opposite corners */
  SCAV_PRIM_RRECT = 1,    /* 2 points, a = corner radius */
  SCAV_PRIM_LINE = 2,     /* 2 points */
  SCAV_PRIM_POLYLINE = 3, /* N >= 2 points, open */
  SCAV_PRIM_PATH = 4,     /* N >= 2 points, closed */
  SCAV_PRIM_TEXT = 5,     /* 1 point: baseline origin; payload = the string */
  SCAV_PRIM_CIRCLE = 6,   /* 1 point: centre, a = radius */
  SCAV_PRIM_ARC = 7,      /* 2 points as rect, a/b = start/sweep, 1/64 degree */
  SCAV_PRIM_IMAGE = 8,    /* 2 points, payload = a registered image id */
  SCAV_PRIM_KIND_COUNT = 9
};

/* Interned; primitives index a style table. 20 bytes, no padding. */
typedef struct {
  uint32_t stroke_rgba;
  uint32_t fill_rgba;
  int32_t stroke_w;       /* grid units */
  uint32_t dash;          /* 0 = solid; app-defined otherwise */
  int32_t font_size_grid; /* grid units, 1/16 pt */
} scav_style;

/* Draw order is `depth`, independent of array position. 48 bytes, no padding. */
typedef struct {
  uint32_t kind; /* one of SCAV_PRIM_* */
  int32_t depth;
  uint32_t style;       /* -> the style table */
  uint32_t clip;        /* -> the clip table; SCAV_CLIP_NONE = unclipped */
  uint32_t origin_kind; /* the defining entity, or none */
  uint32_t origin_ordinal;
  scav_span points;  /* -> the point array; meaning per kind */
  scav_span payload; /* -> the text pool: a string, or an image id */
  int32_t a, b;      /* kind-specific scalars */
} scav_prim;

/* The `clip` of an unclipped primitive. A macro, unsigned on every ABI. */
#define SCAV_CLIP_NONE 0xFFFFFFFFU

/* Metrics ================================================================= */

/* NULL `ttf` selects the bundled font. The handle copies `ttf`; the caller may free it.
 * Immutable after create; threads share it without locking. */
scav_result scav_metrics_create(scav_byte const *ttf, uint32_t len, scav_metrics **out);
void scav_metrics_destroy(scav_metrics *metrics);

/* xxh32 over the font's bytes: the font's identity and version. */
scav_result scav_metrics_identity(scav_metrics const *metrics, uint32_t *out);

/* Design units per em, and the glyph count the tail rule is bounded by. */
scav_result scav_metrics_units_per_em(scav_metrics const *metrics, uint32_t *out);
scav_result scav_metrics_glyph_count(scav_metrics const *metrics, uint32_t *out);

/* One line of NFC UTF-8: `w` is the advance sum in grid units, rounded up once; `h` is
 * `font_size_grid`. A newline is SCAV_E_INVALID_ARG, a missing glyph SCAV_E_NO_GLYPH. */
scav_result scav_measure_text(scav_metrics const *metrics,
                              scav_byte const *utf8_nfc,
                              uint32_t len,
                              int32_t font_size_grid,
                              scav_extent *out,
                              uint32_t out_size);

/* The height of one line at a profile's ratio: ceil_div(size * num, den). */
scav_result scav_line_height(int32_t font_size_grid,
                             int32_t k_num,
                             int32_t k_den,
                             int32_t *out);

/* Splits at author newlines only: `w` is the widest line, `h` the line count times the
 * line height. */
scav_result scav_measure_block(scav_metrics const *metrics,
                               scav_byte const *utf8_nfc,
                               uint32_t len,
                               int32_t font_size_grid,
                               int32_t k_num,
                               int32_t k_den,
                               scav_extent *out,
                               uint32_t out_size);

/* DrawList ================================================================ */

scav_result scav_drawlist_create(scav_drawlist **out);
void scav_drawlist_destroy(scav_drawlist *list);

/* Row counts of each array, and the text pool's byte count. NULL outputs are skipped. */
scav_result scav_drawlist_counts(scav_drawlist const *list,
                                 uint32_t *out_prims,
                                 uint32_t *out_styles,
                                 uint32_t *out_points,
                                 uint32_t *out_clips,
                                 uint32_t *out_text);

/* Each flat array as base pointer, row stride and count, the shape a column uses.
 * An empty array reads back NULL and zero. */
scav_result scav_drawlist_prims(scav_drawlist const *list,
                                scav_prim const **out,
                                uint32_t *out_stride,
                                uint32_t *out_count);
scav_result scav_drawlist_styles(scav_drawlist const *list,
                                 scav_style const **out,
                                 uint32_t *out_stride,
                                 uint32_t *out_count);
scav_result scav_drawlist_points(scav_drawlist const *list,
                                 scav_point const **out,
                                 uint32_t *out_stride,
                                 uint32_t *out_count);
scav_result scav_drawlist_clips(scav_drawlist const *list,
                                scav_rect const **out,
                                uint32_t *out_stride,
                                uint32_t *out_count);

/* A payload span against the list's own pool. Not NUL-terminated. */
scav_result scav_drawlist_str(scav_drawlist const *list,
                              scav_span payload,
                              scav_byte const **out,
                              uint32_t *out_len);

/* Every point count and scalar against its kind, plus every index in range.
 * SCAV_E_DRAWLIST names the offending primitive in `out_prim`. */
scav_result scav_drawlist_validate(scav_drawlist const *list, uint32_t *out_prim);

/* Sorts by (depth, prim content), dedupes the style and clip tables, and rewrites the
 * indices. Lists drawing one picture in any emission order compare equal. Idempotent. */
scav_result scav_drawlist_canonicalize(scav_drawlist *list);

/* xxh32 over the font's identity and the list's content in its current order. */
scav_result scav_drawlist_digest(scav_drawlist const *list,
                                 scav_metrics const *metrics,
                                 uint32_t *out);

/* Appends `src` onto `dst`, rebasing style, clip, point and payload indices. `depth`
 * orders the merged primitives. */
scav_result scav_drawlist_append(scav_drawlist *dst, scav_drawlist const *src);

/* Images ================================================================== */

/* A registry of raster images; dimensions are given at registration. */
scav_result scav_images_create(scav_images **out);
void scav_images_destroy(scav_images *images);
scav_result scav_image_register(scav_images *images,
                                char const *id,
                                scav_byte const *bytes,
                                uint32_t len,
                                int32_t w,
                                int32_t h,
                                char const *mime);
scav_result scav_image_count(scav_images const *images, uint32_t *out_count);
scav_result scav_image_find(scav_images const *images,
                            scav_byte const *id,
                            uint32_t id_len,
                            uint32_t *out_index);
scav_result scav_image_extent(scav_images const *images,
                              uint32_t index,
                              scav_extent *out,
                              uint32_t out_size);

/* Reference builder ======================================================= */

/* Emits a laid-out chart at `depth`; a NULL `palette` selects the standard one. Pass the
 * spaces and placed boxes layout was given. A chart without geometry is SCAV_E_STATE.
 *
 * The two row sizes and `spaces_size` are checked first and are SCAV_E_ABI when
 * any disagrees, a NULL palette, NULL spaces and zero placed rows included. */
scav_result scav_emit_chart(scav_drawlist *list,
                            scav_chart const *chart,
                            scav_metrics const *metrics,
                            scav_style const *palette,
                            uint32_t palette_len,
                            uint32_t palette_row_size,
                            scav_spaces const *spaces,
                            uint32_t spaces_size,
                            scav_placed const *placed,
                            uint32_t placed_count,
                            uint32_t placed_row_size,
                            int32_t depth);

/* The reference measurement pass: fills the four space tables for `chart`.
 *
 * Pass all four caps as 0 with a non-null `out_counts` to query the four
 * counts, then call again with buffers. `out_counts` receives four values in
 * this order: box_state, box_sub, path_clear, path_box. A cap too small is
 * SCAV_E_CAPACITY and never truncates. SCAV_E_STATE when the pass fails: a codepoint
 * with no glyph, or a request outside the legal domain.
 *
 * The profile size and all four row sizes are checked first, on the count query too;
 * any mismatch is SCAV_E_ABI. */
scav_result scav_measure_chart(scav_chart const *chart,
                               scav_metrics const *metrics,
                               scav_profile const *profile,
                               uint32_t profile_size,
                               scav_box_space *box_state,
                               uint32_t cap_box_state,
                               uint32_t box_state_row_size,
                               scav_box_space *box_sub,
                               uint32_t cap_box_sub,
                               uint32_t box_sub_row_size,
                               scav_path_clear *path_clear,
                               uint32_t cap_path_clear,
                               uint32_t path_clear_row_size,
                               scav_path_box *path_box,
                               uint32_t cap_path_box,
                               uint32_t path_box_row_size,
                               uint32_t *out_counts);

/* The palette `scav_emit_chart` wants, in this order. */
enum {
  SCAV_STYLE_STATE = 0,  /* state box outline and fill */
  SCAV_STYLE_SUB = 1,    /* submachine divider */
  SCAV_STYLE_ROUTE = 2,  /* transition polyline */
  SCAV_STYLE_TITLE = 3,  /* state name */
  SCAV_STYLE_LABEL = 4,  /* transition label */
  SCAV_STYLE_PSEUDO = 5, /* initial, final, choice, fork, join, history */
  SCAV_STYLE_ARROW = 6,  /* arrowhead triangle, tip on the target border */
  SCAV_STYLE_COUNT = 7
};

/* NOLINTEND(modernize-use-using, readability-identifier-naming) */

/* Writes the standard palette's SCAV_STYLE_COUNT rows to `out`. A smaller `cap` is
 * SCAV_E_CAPACITY. */
scav_result scav_palette_standard(scav_style *out, uint32_t cap, uint32_t row_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_DRAW_C_H_INCLUDED */
