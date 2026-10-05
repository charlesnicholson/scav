#ifndef SCAV_SVG_H_INCLUDED
#define SCAV_SVG_H_INCLUDED

// libscavsvg's public API: one DrawList to one SVG document, in integer grid units
// with the whole scale in one integer viewBox.

#include "scav/scav_draw.h"
#include "scav/scav_svg_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string>

namespace scav {

// Result of `svg_write`. `bad` names the failing primitive, or is INVALID for Ok,
// ExtentOverflow and FontMismatch.
enum class SvgStatus : uint32_t {
  Ok,
  InvalidDrawList,  // the validator refused it, or a text payload failed to measure
  UnsupportedPrim,  // a kind this backend does not render: arcs
  UnknownImage,     // an image primitive naming nothing in the registry
  MissingGlyph,     // a text glyph missing from the metrics' font
  ExtentOverflow,   // the content does not fit an integer viewBox
  FontMismatch,     // the bytes to embed are not the font the metrics measured with
};

struct SvgOptions {
  // A TTF to base64 whole into <defs><style>@font-face, or null; its xxh32 must be
  // the metrics' identity.
  scav_byte const *embed_font{ nullptr };
  uint32_t embed_font_len{ 0 };
  int32_t margin{ 0 };  // grid units of clear space around the content
};

// `images` may be empty when no primitive names one. Appends to `out`, and
// leaves it untouched on anything but Ok.
SvgStatus svg_write(DrawList const &d,
                    Metrics const &m,
                    Images const &images,
                    SvgOptions const &o,
                    std::string &out,
                    uint32_t &bad);

// Tight bounding box of every primitive's points, circles grown by their radius, in
// grid units; empty for an empty list. Excludes stroke width.
scav_rect svg_bounds(DrawList const &d);

// The class attribute value for a primitive's origin, e.g. `scav-state scav-id-1234`;
// empty when it has no origin.
std::string svg_class(scav_prim const &p);

}  // namespace scav

#endif  // SCAV_SVG_H_INCLUDED
