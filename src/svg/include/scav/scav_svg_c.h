#ifndef SCAV_SVG_C_H_INCLUDED
#define SCAV_SVG_C_H_INCLUDED

/* libscavsvg's C API: one DrawList to one SVG document, under the out-param
 * protocol every other span accessor follows. */

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
  int32_t embed_font; /* 1 = base64 the bundled TTF into <defs>; [0, 1] */
  int32_t margin;     /* grid units of clear space around the content */
} scav_svg_options;

/* NOLINTEND(modernize-use-using, readability-identifier-naming) */

/* Writes the document into `out`. Pass cap = 0 with a non-null out_count to
 * query the byte count, then call again with a buffer; a cap too small is
 * SCAV_E_CAPACITY and writes the required count rather than truncating. The
 * bytes are not NUL-terminated.
 *
 * `images` may be NULL when no primitive names one. SCAV_E_DRAWLIST covers
 * every refusal -- an invalid list, a kind this backend does not render, an
 * unknown image id, a glyph the font lacks -- because each of them means the
 * DrawList and the backend disagree about what is drawable. */
scav_result scav_svg_write(scav_drawlist const *list,
                           scav_metrics const *metrics,
                           scav_images const *images,
                           scav_svg_options const *options,
                           scav_byte *out,
                           uint32_t cap,
                           uint32_t *out_count);

/* The tight bounding box over every primitive's points, in grid units. */
scav_result scav_svg_bounds(scav_drawlist const *list, scav_rect *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCAV_SVG_C_H_INCLUDED */
