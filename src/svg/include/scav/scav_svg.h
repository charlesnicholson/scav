#ifndef SCAV_SVG_H_INCLUDED
#define SCAV_SVG_H_INCLUDED

// libscavsvg's public API: one DrawList to one SVG document.
//
// The body is emitted in integer grid units with the whole scale in a single
// integer viewBox. No float is printed, ever: float-to-decimal conversion is
// not portable, and `-ffp-contract=fast` means `grid * scale` differs by an ULP
// between Debug and Release.

#include "scav/scav_draw.h"
#include "scav/scav_svg_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string>

namespace scav {

// Why a document could not be written. Every one names the offending primitive
// in `bad` rather than emitting something a viewer would misdraw.
enum class SvgStatus : uint32_t {
  Ok,
  InvalidDrawList,  // the DrawList validator refused it
  UnsupportedPrim,  // a kind this backend does not render; see below
  UnknownImage,     // an image primitive naming nothing in the registry
  MissingGlyph,     // textLength needs the advance sum, so the font must have it
  ExtentOverflow,   // the content does not fit an integer viewBox
  FontMismatch,     // the bytes to embed are not the font the metrics measured with
};

struct SvgOptions {
  // A TTF to base64 whole into <defs><style>@font-face, or null. Its xxh32 must be
  // the metrics' identity. The only exact renderer-metrics agreement that keeps
  // text selectable, and whole rather than subsetted because a subsetter is the
  // expensive half of a PDF backend.
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

// The tight bounding box over every primitive's points, in grid units. An empty
// list is an empty rect. Stroke width is not accounted for: a builder that
// wants its stroke inside the box reserves and insets.
scav_rect svg_bounds(DrawList const &d);

// The class attribute a primitive gets, synthesized from its origin --
// `scav-state scav-id-1234`. A backend's projection, never IR content.
std::string svg_class(scav_prim const &p);

}  // namespace scav

#endif  // SCAV_SVG_H_INCLUDED
