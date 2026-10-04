#ifndef SCAV_DRAW_H_INCLUDED
#define SCAV_DRAW_H_INCLUDED

// libscavdraw's public API: font metrics, the DrawList render IR, the helper layer and
// the reference builder. A DrawList from any builder feeds any backend.

#include "scav/scav_core.h"
#include "scav/scav_draw_c.h"
// The space-table, profile and placed-box PODs; libscavdraw does not link layout.
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

// Font metrics ==============================================================

// Parsed table offsets plus a copy of the bytes they index, or the bundled
// font's compiled-in table. Immutable once created, so one instance serves every thread.
struct Metrics {
  std::vector<scav_byte> ttf;  // empty for the bundled font
  uint32_t identity{ 0 };      // xxh32 of the TTF: the font's identity and version
  uint32_t units_per_em{ 0 };
  uint32_t num_glyphs{ 0 };
  uint32_t num_h_metrics{ 0 };
  Span hmtx{};                // into `ttf`
  Span cmap_sub{};            // the chosen subtable, into `ttf`
  uint32_t cmap_format{ 0 };  // 4 or 12
  bool bundled{ false };      // glyphs and advances come from the compiled-in table
};

// xxh32 of assets/font/JetBrainsMono-Regular.ttf; the library embeds only its metrics.
uint32_t bundled_font_identity();

// Empty `ttf` selects the bundled font's table. False on a font missing a table the
// measurement needs, or one whose tables do not agree with each other.
bool metrics_create(scav_byte const *ttf, uint32_t len, Metrics &out);

// The glyph a codepoint maps to, or 0 (`.notdef`) when unmapped. Callers treat 0 as an
// error.
uint32_t metrics_glyph(Metrics const &m, uint32_t codepoint);

// A glyph's advance in font design units. Glyphs past the last hmtx record take its
// advance.
uint32_t metrics_advance(Metrics const &m, uint32_t glyph);

// One line of NFC UTF-8: advances summed in int64, scaled once, rounded up. Kerning
// is ignored.
enum class MeasureStatus : uint32_t { Ok, BadUtf8, Newline, MissingGlyph, BadSize };
MeasureStatus measure_text(Metrics const &m,
                           scav_byte const *utf8_nfc,
                           uint32_t len,
                           int32_t font_size_grid,
                           scav_extent &out);

// `ceil(font_size_grid * k_num / k_den)`, ignoring the font's vertical metrics. Zero
// when `font_size_grid <= 0`, a ratio term is outside 1..1024, or the result overflows.
int32_t line_height(int32_t font_size_grid, int32_t k_num, int32_t k_den);

// Author-supplied breaks only. `w` is the widest line, `h` the line count
// times the line height, and an empty input is one empty line.
MeasureStatus measure_block(Metrics const &m,
                            scav_byte const *utf8_nfc,
                            uint32_t len,
                            int32_t font_size_grid,
                            int32_t k_num,
                            int32_t k_den,
                            scav_extent &out);

// DrawList ==================================================================

// Absolute grid units in one frame, the frame of the geometry columns.
struct DrawList {
  std::vector<scav_prim> prims;
  std::vector<scav_style> styles;
  std::vector<scav_point> points;
  std::vector<scav_rect> clips;
  StringPool text;
};

// Interns a style or clip and returns its row, appending only when new.
// `drawlist_text` appends `s` to the text pool and returns its span.
uint32_t drawlist_style(DrawList &d, scav_style const &s);
uint32_t drawlist_clip(DrawList &d, scav_rect const &r);
scav_span drawlist_text(DrawList &d, std::string_view s);

// The emitters. `depth` is caller-defined draw order.
void push_rect(DrawList &d, int32_t depth, uint32_t style, scav_rect r, ElemRef origin);
void push_rrect(DrawList &d,
                int32_t depth,
                uint32_t style,
                scav_rect r,
                int32_t radius,
                ElemRef origin);
void push_line(DrawList &d,
               int32_t depth,
               uint32_t style,
               scav_point a,
               scav_point b,
               ElemRef origin);
void push_polyline(DrawList &d,
                   int32_t depth,
                   uint32_t style,
                   scav_point const *pts,
                   uint32_t n,
                   ElemRef origin);
void push_path(DrawList &d,
               int32_t depth,
               uint32_t style,
               scav_point const *pts,
               uint32_t n,
               ElemRef origin);
void push_text(DrawList &d,
               int32_t depth,
               uint32_t style,
               scav_point baseline,
               std::string_view s,
               ElemRef origin);
void push_circle(DrawList &d,
                 int32_t depth,
                 uint32_t style,
                 scav_point centre,
                 int32_t radius,
                 ElemRef origin);
void push_arc(DrawList &d,
              int32_t depth,
              uint32_t style,
              scav_rect bounds,
              int32_t start_64,
              int32_t sweep_64,
              ElemRef origin);
void push_image(DrawList &d,
                int32_t depth,
                uint32_t style,
                scav_rect r,
                std::string_view id,
                ElemRef origin);

// Every point count and scalar against its kind, and every index in range.
// False writes the offending primitive's row to `bad`.
bool drawlist_validate(DrawList const &d, uint32_t &bad);

// Sorts by (depth, primitive content) and dedupes the style and clip tables. Lists
// drawing one picture in any emission order canonicalize equal.
void drawlist_canonicalize(DrawList &d);

// xxh32 over the font's identity and the list's content in its current order, field
// by field.
uint32_t drawlist_digest(DrawList const &d, Metrics const &m);

// Rebases `src`'s style, clip, point and payload indices onto `dst`.
void drawlist_append(DrawList &dst, DrawList const &src);

// Images ====================================================================

// Raster images the app registers and DrawList image primitives reference by id.
struct Images {
  struct Row {
    StrRef id, mime;
    Span bytes;  // into `pool`
    int32_t w, h;
  };
  std::vector<Row> rows;
  std::vector<scav_byte> pool;  // ids, mime types and image bytes, one arena
};

// Registers an image with caller-given dimensions. False on a duplicate or empty id,
// an empty mime, no bytes, or a non-positive extent.
bool image_register(Images &images,
                    std::string_view id,
                    scav_byte const *bytes,
                    uint32_t len,
                    int32_t w,
                    int32_t h,
                    std::string_view mime);

// The row an id names, or INVALID.
uint32_t image_find(Images const &images, std::string_view id);

inline std::string_view image_str(Images const &images, StrRef ref) {
  if (ref.len == 0) { return {}; }
  return { reinterpret_cast<char const *>(images.pool.data() + ref.off), ref.len };
}

inline std::string_view image_bytes(Images const &images, Span ref) {
  if (ref.len == 0) { return {}; }
  return { reinterpret_cast<char const *>(images.pool.data() + ref.off), ref.len };
}

// Helper layer ==============================================================

// Optional pure functions over PODs, for apps and builders.

enum class Anchor : uint32_t {
  TopLeft,
  TopCentre,
  TopRight,
  MidLeft,
  MidCentre,
  MidRight,
  BottomLeft,
  BottomCentre,
  BottomRight,
};

// `stack_v` and `row_h` split `r` into consecutive rects from heights or widths,
// clipped to `r`; `align` places a `w` by `h` box at an anchor.
void stack_v(scav_rect r, int32_t const *heights, uint32_t n, scav_rect *out);
void row_h(scav_rect r, int32_t const *widths, uint32_t n, scav_rect *out);
scav_rect align(scav_rect r, int32_t w, int32_t h, Anchor a);

// The lines an author wrote, split at newlines only, as views into `s`.
std::vector<std::string_view> text_lines(std::string_view s);

// Shape emission over the primitives above.
void push_arrowhead(DrawList &d,
                    int32_t depth,
                    uint32_t style,
                    scav_point tip,
                    scav_point from,
                    int32_t size,
                    ElemRef origin);

// Reference builder =========================================================

// Standard appearance, as per-element-kind emitters. Each takes the depth to
// draw at, so an app calls the ones it wants and skips the rest.
using Palette = std::vector<scav_style>;

Palette palette_standard();

// Output of the measurement pass: a state's title and description, a submachine's
// name, arrowhead room, and a path box per labelled transition.
struct Spaces {
  std::vector<scav_box_space> box_state, box_sub;
  std::vector<scav_path_clear> path_clear;
  std::vector<scav_path_box> path_box;
  std::vector<StrRef> label;  // parallel to path_box: what it was measured from
};

bool measure_chart(Chart const &c, Metrics const &m, scav_profile const &p, Spaces &out);

// Base pointers, counts and strides over a Spaces, for handing to layout.
scav_spaces as_spaces(Spaces const &s);

// Where `trans`'s label goes: the box layout placed. False when it asked for none.
bool label_box(Chart const &c,
               scav_spaces const &s,
               scav_placed const *placed,
               uint32_t placed_count,
               uint32_t trans,
               scav_rect &out);

void emit_state(DrawList &d,
                Chart const &c,
                Metrics const &m,
                Palette const &p,
                uint32_t state,
                int32_t depth);
void emit_submachine(DrawList &d,
                     Chart const &c,
                     Palette const &p,
                     uint32_t sub,
                     int32_t depth);
void emit_route(DrawList &d,
                scav_spaces const &s,
                Chart const &c,
                Palette const &p,
                uint32_t trans,
                int32_t depth);

// Draws the label centred in `box`, the rect layout placed; it may exceed the request.
void emit_label(DrawList &d,
                Chart const &c,
                Metrics const &m,
                Palette const &p,
                uint32_t trans,
                scav_rect box,
                int32_t depth);

// Calls every emitter at `depth`: submachines, states, routes, labels. False when the
// chart has no geometry or the palette is short.
bool emit_chart(DrawList &d,
                Chart const &c,
                Metrics const &m,
                Palette const &p,
                scav_spaces const &s,
                scav_placed const *placed,
                uint32_t placed_count,
                int32_t depth);

}  // namespace scav

#endif  // SCAV_DRAW_H_INCLUDED
