#ifndef SCAV_DRAW_H_INCLUDED
#define SCAV_DRAW_H_INCLUDED

// libscavdraw's public API: font metrics, the DrawList render IR, the optional
// helper layer, and the reference builder.
//
// A builder reads model columns and produces a DrawList; a backend consumes
// one. Neither knows about the other, and neither is required to be scav's.

#include "scav/scav_core.h"
#include "scav/scav_draw_c.h"
// The space tables and the profile, for the measurement pass. A header of PODs
// and no functions, so this is vocabulary and not a link on layout -- which
// draw still may not have, since a builder does not care who wrote the geometry.
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

// Font metrics ==============================================================

// Parsed table offsets plus a copy of the bytes they index. Immutable once
// created, which is what lets one instance serve every thread.
struct Metrics {
  std::vector<scav_byte> ttf;
  uint32_t identity{ 0 };  // xxh32 of `ttf`: the font's identity and version
  uint32_t units_per_em{ 0 };
  uint32_t num_glyphs{ 0 };
  uint32_t num_h_metrics{ 0 };
  Span hmtx{};             // into `ttf`
  Span cmap_sub{};         // the chosen subtable, into `ttf`
  uint32_t cmap_format{ 0 };  // 4 or 12
};

// The bundled font, embedded in the library because it is a hashed input and
// must travel with the code.
scav_byte const *bundled_font(uint32_t &len);

// Empty `ttf` selects the bundled font. False on a font missing a table the
// measurement needs, or one whose tables do not agree with each other.
bool metrics_create(scav_byte const *ttf, uint32_t len, Metrics &out);

// The glyph a codepoint maps to, or 0 for `.notdef` -- which callers treat as
// an error, never as a zero-width glyph.
uint32_t metrics_glyph(Metrics const &m, uint32_t codepoint);

// A glyph's advance in font design units. The last hmtx record's advance
// applies to every glyph past the table, which is the rule that breaks
// monospaced fonts when it is missed.
uint32_t metrics_advance(Metrics const &m, uint32_t glyph);

// One line of NFC UTF-8. Accumulates in int64 and divides exactly once,
// ceiling: an under-sized box is a diagram that lies. Kerning is deliberately
// ignored, which for Latin over-sizes and so never under-sizes.
enum class MeasureStatus : uint32_t { Ok, BadUtf8, Newline, MissingGlyph, BadSize };
MeasureStatus measure_text(Metrics const &m,
                           scav_byte const *utf8_nfc,
                           uint32_t len,
                           int32_t font_size_grid,
                           scav_extent &out);

// Not font vertical metrics: hhea, OS/2.sTypo* and usWin* disagree by 10-20%
// within one font. Zero when the ratio is out of range.
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

// Absolute grid units, one frame, no per-primitive frame tag: a builder reads
// the geometry columns and knows where things are.
struct DrawList {
  std::vector<scav_prim> prims;
  std::vector<scav_style> styles;
  std::vector<scav_point> points;
  std::vector<scav_rect> clips;
  StringPool text;
};

// Interning, so a fat per-primitive style never forces a full rebuild when
// only a colour changed. Returns the row, appending only what is new.
uint32_t drawlist_style(DrawList &d, scav_style const &s);
uint32_t drawlist_clip(DrawList &d, scav_rect const &r);
scav_span drawlist_text(DrawList &d, std::string_view s);

// The emitters. Depth is a parameter because scav reserves no bands and
// assigns no depth semantics: the caller owns the numbering.
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

// Sorts by (depth, primitive bytes), deduplicates the style and clip tables in
// field order, and rewrites the indices. Content rather than emission order is
// what makes two builders drawing one picture compare equal; an emission-index
// tiebreak would not. Idempotent.
void drawlist_canonicalize(DrawList &d);

// xxh32 over the canonical form, opening with the font's identity. Field by
// field, never a struct's bytes.
uint32_t drawlist_digest(DrawList const &d, Metrics const &m);

// Rebases `src`'s style, clip, point and payload indices onto `dst`.
void drawlist_append(DrawList &dst, DrawList const &src);

// Helper layer ==============================================================

// Utilities an app may call, never machinery that calls the app. Pure
// functions over PODs, all optional; nothing in scav's pipeline invokes them.

enum class Anchor : uint32_t {
  TopLeft, TopCentre, TopRight,
  MidLeft, MidCentre, MidRight,
  BottomLeft, BottomCentre, BottomRight,
};

// Turns "I have a rect and three things" into positions. Heights and widths
// come in, rects go out; a request past the rect is clipped to it.
void stack_v(scav_rect r, int32_t const *heights, uint32_t n, scav_rect *out);
void row_h(scav_rect r, int32_t const *widths, uint32_t n, scav_rect *out);
scav_rect align(scav_rect r, int32_t w, int32_t h, Anchor a);

// The lines an author wrote, as views into `s`. No re-wrapping: wrap width is
// always an input.
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
                Chart const &c,
                Palette const &p,
                uint32_t trans,
                int32_t depth);
void emit_label(DrawList &d,
                Chart const &c,
                Metrics const &m,
                Palette const &p,
                uint32_t trans,
                int32_t depth);

// Every emitter in an order this function documents and nothing else depends
// on: submachines, states, routes, then labels. False when the chart carries
// no geometry, which means layout has not run.
bool emit_chart(DrawList &d,
                 Chart const &c,
                 Metrics const &m,
                 Palette const &p,
                 int32_t depth);

// The measurement pass, and the stated policy every corpus golden is against:
// a state's interior reserves its title, a transition's label becomes one path
// box, and nothing else asks for room. Sizes the three tables to the chart.
struct Spaces {
  std::vector<scav_box_space> box_state, box_sub;
  std::vector<scav_path_clear> path_clear;
  std::vector<scav_path_box> path_box;
  std::vector<StrRef> label;  // parallel to path_box: what it was measured from
};

bool measure_chart(Chart const &c, Metrics const &m, scav_profile const &p, Spaces &out);

// Base pointers and counts over a Spaces, for handing to layout.
scav_spaces as_spaces(Spaces const &s);

}  // namespace scav

#endif  // SCAV_DRAW_H_INCLUDED
