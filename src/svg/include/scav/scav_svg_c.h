#ifndef SCAV_SVG_C_H_INCLUDED
#define SCAV_SVG_C_H_INCLUDED

/* libscavsvg's C API: one DrawList to one SVG document.
 * Each POD size argument is checked first, NULL or not; a mismatch is SCAV_E_ABI. */

#include "scav/scav_core_c.h"
#include "scav/scav_draw_c.h"
#include "scav/scav_types.h"

/* NOLINTNEXTLINE(modernize-deprecated-headers) -- this header must compile as C */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using, readability-identifier-naming) */

typedef struct {
  scav_byte const *embed_font; /* a TTF to base64 into <defs>, or NULL */
  uint32_t embed_font_len;     /* 0 when embed_font is NULL */
  int32_t margin;              /* grid units of clear space around the content */
} scav_svg_options;

/* NOLINTEND(modernize-use-using, readability-identifier-naming) */

/* Writes the document into `out`, not NUL-terminated. cap = 0 with a non-null
 * `out_count` queries the byte count; a cap too small is SCAV_E_CAPACITY and still
 * writes the count. `images` may be NULL when no primitive names one; NULL `options`
 * takes the defaults, and `options_size` is checked either way.
 * SCAV_E_DRAWLIST: an invalid list, an unrendered kind, an unknown image id, a missing
 * glyph, or content too large for an integer viewBox.
 * SCAV_E_FONT: embed font bytes whose xxh32 is not the metrics' identity. */
scav_result scav_svg_write(scav_drawlist const *list,
                           scav_metrics const *metrics,
                           scav_images const *images,
                           scav_svg_options const *options,
                           uint32_t options_size,
                           scav_byte *out,
                           uint32_t cap,
                           uint32_t *out_count);

/* The tight bounding box of every primitive's points, circles grown by their radius, in
 * grid units. */
scav_result scav_svg_bounds(scav_drawlist const *list, scav_rect *out, uint32_t out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_SVG_C_H_INCLUDED */
