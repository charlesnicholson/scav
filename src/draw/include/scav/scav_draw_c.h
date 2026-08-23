#ifndef SCAV_DRAW_C_H_INCLUDED
#define SCAV_DRAW_C_H_INCLUDED

/* libscavdraw's C API: the font metrics handle and the DrawList render IR.
 *
 * A DrawList is five flat arrays plus a string pool, read out with the same
 * span accessors as a column. Every field of scav_style and scav_prim is four
 * bytes wide, so the canonical form can be compared byte for byte without
 * reading padding. */

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

/* What a primitive is. `points` and the two scalars are fixed per kind, so a
 * backend switches once and never guesses. */
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
  int32_t stroke_w;        /* grid units */
  uint32_t dash;           /* 0 = solid; app-defined otherwise */
  int32_t font_size_grid;  /* 1/16 pt, the same width as everywhere else */
} scav_style;

/* Draw order is `depth`, not array position, which is what makes a DrawList
 * appendable. 48 bytes, no padding. */
typedef struct {
  uint32_t kind;         /* one of SCAV_PRIM_* */
  int32_t depth;
  uint32_t style;        /* -> the style table */
  uint32_t clip;         /* -> the clip table; SCAV_CLIP_NONE = unclipped */
  uint32_t origin_kind;  /* the defining entity, or none */
  uint32_t origin_ordinal;
  scav_span points;      /* -> the point array; meaning per kind */
  scav_span payload;     /* -> the text pool: a string, or an image id */
  int32_t a, b;          /* kind-specific scalars */
} scav_prim;

/* An unclipped primitive names no clip rect. */
enum { SCAV_CLIP_NONE = 0xFFFFFFFFU };

/* Metrics ================================================================= */

/* NULL `ttf` selects the bundled font. The handle copies what it parses, so
 * the caller's buffer need not outlive the call. Immutable after create, so it
 * is shared across threads without locking. */
scav_result scav_metrics_create(scav_byte const *ttf, uint32_t len, scav_metrics **out);
void scav_metrics_destroy(scav_metrics *metrics);

/* xxh32 over the font's bytes: identity and version in one number, which is
 * what a DrawList golden records rather than a name a file could lie about. */
scav_result scav_metrics_identity(scav_metrics const *metrics, uint32_t *out);

/* Design units per em, and the glyph count the tail rule is bounded by. */
scav_result scav_metrics_units_per_em(scav_metrics const *metrics, uint32_t *out);
scav_result scav_metrics_glyph_count(scav_metrics const *metrics, uint32_t *out);

/* One line of NFC UTF-8: `w` is the advance sum scaled to grid units, ceiled
 * once at the end, and `h` is `font_size_grid`. A newline is
 * SCAV_E_INVALID_ARG -- wrapping is the caller's, so it splits first -- and a
 * codepoint the font has no glyph for is SCAV_E_NO_GLYPH rather than a silent
 * zero, which would size a box narrower than its own text. */
scav_result scav_measure_text(scav_metrics const *metrics,
                              scav_byte const *utf8_nfc,
                              uint32_t len,
                              int32_t font_size_grid,
                              scav_extent *out);

/* The height of one line at a profile's ratio: ceil_div(size * num, den). */
scav_result scav_line_height(int32_t font_size_grid,
                             int32_t k_num,
                             int32_t k_den,
                             int32_t *out);

/* Author-supplied breaks only, so `w` is the widest line and `h` is the line
 * count times the line height. Layout never re-wraps. */
scav_result scav_measure_block(scav_metrics const *metrics,
                               scav_byte const *utf8_nfc,
                               uint32_t len,
                               int32_t font_size_grid,
                               int32_t k_num,
                               int32_t k_den,
                               scav_extent *out);

/* DrawList ================================================================ */

scav_result scav_drawlist_create(scav_drawlist **out);
void scav_drawlist_destroy(scav_drawlist *list);

/* Row counts, so a binding can size its reads without walking anything. */
scav_result scav_drawlist_counts(scav_drawlist const *list,
                                 uint32_t *out_prims,
                                 uint32_t *out_styles,
                                 uint32_t *out_points,
                                 uint32_t *out_clips,
                                 uint32_t *out_text);

/* The flat arrays, read out with the same three-call shape as a column. */
scav_result scav_drawlist_prims(scav_drawlist const *list,
                                scav_prim const **out,
                                uint32_t *out_count);
scav_result scav_drawlist_styles(scav_drawlist const *list,
                                 scav_style const **out,
                                 uint32_t *out_count);
scav_result scav_drawlist_points(scav_drawlist const *list,
                                 scav_point const **out,
                                 uint32_t *out_count);
scav_result scav_drawlist_clips(scav_drawlist const *list,
                                scav_rect const **out,
                                uint32_t *out_count);

/* A payload span against the list's own pool. Not NUL-terminated. */
scav_result scav_drawlist_str(scav_drawlist const *list,
                              scav_span payload,
                              scav_byte const **out,
                              uint32_t *out_len);

/* Every point count and scalar against its kind, plus every index in range.
 * SCAV_E_DRAWLIST names the offending primitive in `out_prim`. */
scav_result scav_drawlist_validate(scav_drawlist const *list, uint32_t *out_prim);

/* Sorts by (depth, prim bytes), deduplicates the style and clip tables, and
 * rewrites the indices. Content, not emission order, so two builders drawing
 * one picture in different orders compare equal. Idempotent. */
scav_result scav_drawlist_canonicalize(scav_drawlist *list);

/* xxh32 over the canonical form and the font's identity. The DrawList golden's
 * one number: the font is a hashed input here, where glyph advances live. */
scav_result scav_drawlist_digest(scav_drawlist const *list,
                                 scav_metrics const *metrics,
                                 uint32_t *out);

/* Appends `src` onto `dst`, rebasing style, clip, point and payload indices --
 * a shipped function rather than a documented insert() because those four
 * index per-list arrays. Depth resolves the interleaving. */
scav_result scav_drawlist_append(scav_drawlist *dst, scav_drawlist const *src);

/* Images ================================================================== */

/* Raster only, and dimensions come from registration rather than decoding, so
 * no backend needs a decoder to size an image. */
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
                              scav_extent *out);

/* Reference builder ======================================================= */

/* The standard appearance over a laid-out chart. Emitters take the depth to
 * draw at, so an app interleaves its own primitives without forking anything;
 * `scav_emit_chart` calls them in an order it documents and nothing else
 * depends on. Hand back the same space tables and placed boxes layout was
 * given: a label's rect is the one layout placed, not one a builder recomputes.
 * A chart with no geometry columns is SCAV_E_STATE. */
scav_result scav_emit_chart(scav_drawlist *list,
                            scav_chart const *chart,
                            scav_metrics const *metrics,
                            scav_style const *palette,
                            uint32_t palette_len,
                            scav_spaces const *spaces,
                            scav_placed const *placed,
                            uint32_t placed_count,
                            int32_t depth);

/* The palette `scav_emit_chart` wants, in this order. */
enum {
  SCAV_STYLE_STATE = 0,   /* state box outline and fill */
  SCAV_STYLE_SUB = 1,     /* submachine divider */
  SCAV_STYLE_ROUTE = 2,   /* transition polyline and arrowhead */
  SCAV_STYLE_TITLE = 3,   /* state name */
  SCAV_STYLE_LABEL = 4,   /* transition label */
  SCAV_STYLE_PSEUDO = 5,  /* initial, final, choice, fork, join, history */
  SCAV_STYLE_COUNT = 6
};

/* NOLINTEND(modernize-use-using, readability-identifier-naming) */

/* The shipped palette, so a caller that wants the standard look passes it
 * straight through. Writes SCAV_STYLE_COUNT rows. */
scav_result scav_palette_standard(scav_style *out, uint32_t cap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_DRAW_C_H_INCLUDED */
