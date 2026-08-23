// The render IR: interning, the emitters, the per-kind validator, and the
// canonical form the golden compares. Depth is the caller's throughout; scav
// reserves no bands and assigns no depth semantics.

#include "scav/scav_draw.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_stable_sort.h"
#include "scav_xxhash.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace scav {

namespace {

static_assert(sizeof(scav_prim) == 48, "a primitive must stay twelve 4-byte fields");
static_assert(sizeof(scav_style) == 20, "a style must stay five 4-byte fields");

void append_u32(std::vector<scav_byte> &out, uint32_t v) {
  out.push_back(static_cast<scav_byte>(v & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 8U) & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 16U) & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 24U) & 0xFFU));
}

void append_i32(std::vector<scav_byte> &out, int32_t v) {
  append_u32(out, static_cast<uint32_t>(v));
}

bool style_eq(scav_style const &a, scav_style const &b) {
  return (a.stroke_rgba == b.stroke_rgba) && (a.fill_rgba == b.fill_rgba) &&
         (a.stroke_w == b.stroke_w) && (a.dash == b.dash) &&
         (a.font_size_grid == b.font_size_grid);
}

bool style_less(scav_style const &a, scav_style const &b) {
  if (a.stroke_rgba != b.stroke_rgba) { return a.stroke_rgba < b.stroke_rgba; }
  if (a.fill_rgba != b.fill_rgba) { return a.fill_rgba < b.fill_rgba; }
  if (a.stroke_w != b.stroke_w) { return a.stroke_w < b.stroke_w; }
  if (a.dash != b.dash) { return a.dash < b.dash; }
  return a.font_size_grid < b.font_size_grid;
}

bool rect_eq(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

bool rect_less(scav_rect const &a, scav_rect const &b) {
  if (a.x != b.x) { return a.x < b.x; }
  if (a.y != b.y) { return a.y < b.y; }
  if (a.w != b.w) { return a.w < b.w; }
  return a.h < b.h;
}

// How many points a kind takes, and whether that is a floor or the exact
// count. A kind whose count disagrees is invalid, never repaired.
struct PointRule {
  uint32_t count;
  bool exact;
};

PointRule point_rule(uint32_t kind) {
  switch (kind) {
    case SCAV_PRIM_RECT:
    case SCAV_PRIM_RRECT:
    case SCAV_PRIM_LINE:
    case SCAV_PRIM_ARC:
    case SCAV_PRIM_IMAGE: return { .count = 2, .exact = true };
    case SCAV_PRIM_POLYLINE:
    case SCAV_PRIM_PATH: return { .count = 2, .exact = false };
    case SCAV_PRIM_TEXT:
    case SCAV_PRIM_CIRCLE: return { .count = 1, .exact = true };
    default: return { .count = 0, .exact = true };
  }
}

uint32_t push_points(DrawList &d, scav_point const *pts, uint32_t n) {
  uint32_t const off{ static_cast<uint32_t>(d.points.size()) };
  d.points.insert(d.points.end(), pts, pts + n);
  return off;
}

void push_prim(DrawList &d,
               uint32_t kind,
               int32_t depth,
               uint32_t style,
               scav_span points,
               scav_span payload,
               int32_t a,
               int32_t b,
               ElemRef origin) {
  d.prims.push_back({ .kind = kind,
                      .depth = depth,
                      .style = style,
                      .clip = SCAV_CLIP_NONE,
                      .origin_kind = static_cast<uint32_t>(origin.kind),
                      .origin_ordinal = origin.ordinal,
                      .points = points,
                      .payload = payload,
                      .a = a,
                      .b = b });
}

// A primitive's content, so two lists that draw one picture sort alike no
// matter what order their builders emitted in. Indices into per-list arrays
// cannot serve: they encode emission order, which is the thing being erased.
void append_content(std::vector<scav_byte> &key, DrawList const &d, scav_prim const &p) {
  append_i32(key, p.depth);
  append_u32(key, p.kind);
  append_u32(key, p.style);
  append_u32(key, p.clip);
  append_u32(key, p.origin_kind);
  append_u32(key, p.origin_ordinal);
  append_i32(key, p.a);
  append_i32(key, p.b);
  append_u32(key, p.points.len);
  for (uint32_t i = 0; i < p.points.len; ++i) {
    append_i32(key, d.points[p.points.off + i].x);
    append_i32(key, d.points[p.points.off + i].y);
  }
  append_u32(key, p.payload.len);
  for (uint32_t i = 0; i < p.payload.len; ++i) {
    key.push_back(d.text.bytes[p.payload.off + i]);
  }
}

}  // namespace

uint32_t drawlist_style(DrawList &d, scav_style const &s) {
  for (uint32_t i = 0; i < d.styles.size(); ++i) {
    if (style_eq(d.styles[i], s)) { return i; }
  }
  d.styles.push_back(s);
  return static_cast<uint32_t>(d.styles.size()) - 1U;
}

uint32_t drawlist_clip(DrawList &d, scav_rect const &r) {
  for (uint32_t i = 0; i < d.clips.size(); ++i) {
    if (rect_eq(d.clips[i], r)) { return i; }
  }
  d.clips.push_back(r);
  return static_cast<uint32_t>(d.clips.size()) - 1U;
}

scav_span drawlist_text(DrawList &d, std::string_view s) {
  if (s.empty()) { return {}; }
  uint32_t const off{ static_cast<uint32_t>(d.text.bytes.size()) };
  d.text.bytes.insert(d.text.bytes.end(), s.begin(), s.end());
  return { .off = off, .len = static_cast<uint32_t>(s.size()) };
}

void push_rect(DrawList &d, int32_t depth, uint32_t style, scav_rect r, ElemRef origin) {
  scav_point const pts[2]{ { .x = r.x, .y = r.y },
                           { .x = r.x + r.w, .y = r.y + r.h } };
  push_prim(d,
            SCAV_PRIM_RECT,
            depth,
            style,
            { .off = push_points(d, pts, 2), .len = 2 },
            {},
            0,
            0,
            origin);
}

void push_rrect(DrawList &d,
                int32_t depth,
                uint32_t style,
                scav_rect r,
                int32_t radius,
                ElemRef origin) {
  scav_point const pts[2]{ { .x = r.x, .y = r.y },
                           { .x = r.x + r.w, .y = r.y + r.h } };
  push_prim(d,
            SCAV_PRIM_RRECT,
            depth,
            style,
            { .off = push_points(d, pts, 2), .len = 2 },
            {},
            radius,
            0,
            origin);
}

void push_line(DrawList &d,
               int32_t depth,
               uint32_t style,
               scav_point a,
               scav_point b,
               ElemRef origin) {
  scav_point const pts[2]{ a, b };
  push_prim(d,
            SCAV_PRIM_LINE,
            depth,
            style,
            { .off = push_points(d, pts, 2), .len = 2 },
            {},
            0,
            0,
            origin);
}

void push_polyline(DrawList &d,
                   int32_t depth,
                   uint32_t style,
                   scav_point const *pts,
                   uint32_t n,
                   ElemRef origin) {
  push_prim(d,
            SCAV_PRIM_POLYLINE,
            depth,
            style,
            { .off = push_points(d, pts, n), .len = n },
            {},
            0,
            0,
            origin);
}

void push_path(DrawList &d,
               int32_t depth,
               uint32_t style,
               scav_point const *pts,
               uint32_t n,
               ElemRef origin) {
  push_prim(d,
            SCAV_PRIM_PATH,
            depth,
            style,
            { .off = push_points(d, pts, n), .len = n },
            {},
            0,
            0,
            origin);
}

void push_text(DrawList &d,
               int32_t depth,
               uint32_t style,
               scav_point baseline,
               std::string_view s,
               ElemRef origin) {
  push_prim(d,
            SCAV_PRIM_TEXT,
            depth,
            style,
            { .off = push_points(d, &baseline, 1), .len = 1 },
            drawlist_text(d, s),
            0,
            0,
            origin);
}

void push_circle(DrawList &d,
                 int32_t depth,
                 uint32_t style,
                 scav_point centre,
                 int32_t radius,
                 ElemRef origin) {
  push_prim(d,
            SCAV_PRIM_CIRCLE,
            depth,
            style,
            { .off = push_points(d, &centre, 1), .len = 1 },
            {},
            radius,
            0,
            origin);
}

void push_arc(DrawList &d,
              int32_t depth,
              uint32_t style,
              scav_rect bounds,
              int32_t start_64,
              int32_t sweep_64,
              ElemRef origin) {
  scav_point const pts[2]{ { .x = bounds.x, .y = bounds.y },
                           { .x = bounds.x + bounds.w, .y = bounds.y + bounds.h } };
  push_prim(d,
            SCAV_PRIM_ARC,
            depth,
            style,
            { .off = push_points(d, pts, 2), .len = 2 },
            {},
            start_64,
            sweep_64,
            origin);
}

void push_image(DrawList &d,
                int32_t depth,
                uint32_t style,
                scav_rect r,
                std::string_view id,
                ElemRef origin) {
  scav_point const pts[2]{ { .x = r.x, .y = r.y },
                           { .x = r.x + r.w, .y = r.y + r.h } };
  push_prim(d,
            SCAV_PRIM_IMAGE,
            depth,
            style,
            { .off = push_points(d, pts, 2), .len = 2 },
            drawlist_text(d, id),
            0,
            0,
            origin);
}

bool drawlist_validate(DrawList const &d, uint32_t &bad) {
  for (uint32_t i = 0; i < d.prims.size(); ++i) {
    scav_prim const &p{ d.prims[i] };
    bad = i;
    if (p.kind >= SCAV_PRIM_KIND_COUNT) { return false; }
    PointRule const rule{ point_rule(p.kind) };
    if (rule.exact ? (p.points.len != rule.count) : (p.points.len < rule.count)) {
      return false;
    }
    if ((p.points.off > d.points.size()) ||
        (p.points.len > (d.points.size() - p.points.off))) {
      return false;
    }
    if ((p.payload.off > d.text.bytes.size()) ||
        (p.payload.len > (d.text.bytes.size() - p.payload.off))) {
      return false;
    }
    if (p.style >= d.styles.size()) { return false; }
    if ((p.clip != SCAV_CLIP_NONE) && (p.clip >= d.clips.size())) { return false; }
    // Text names a string and an image names an id; the other kinds carry none,
    // so a payload on one of them means a builder wrote to the wrong field.
    bool const wants_payload{ (p.kind == SCAV_PRIM_TEXT) ||
                              (p.kind == SCAV_PRIM_IMAGE) };
    if (!wants_payload && (p.payload.len != 0U)) { return false; }
    if ((p.kind == SCAV_PRIM_IMAGE) && (p.payload.len == 0U)) { return false; }
    if (((p.kind == SCAV_PRIM_RRECT) || (p.kind == SCAV_PRIM_CIRCLE)) && (p.a < 0)) {
      return false;
    }
  }
  bad = INVALID;
  return true;
}

void drawlist_canonicalize(DrawList &d) {
  // Styles and clips first: the prims' sort key names their new indices, so
  // rewriting after the prim sort would sort on indices about to change.
  std::vector<scav_style> styles{ d.styles };
  scav_stable_sort(styles, style_less);
  std::vector<scav_style> unique_styles;
  for (scav_style const &s : styles) {
    if (unique_styles.empty() || !style_eq(unique_styles.back(), s)) {
      unique_styles.push_back(s);
    }
  }
  std::vector<uint32_t> style_map(d.styles.size(), 0);
  for (uint32_t i = 0; i < d.styles.size(); ++i) {
    for (uint32_t j = 0; j < unique_styles.size(); ++j) {
      if (style_eq(unique_styles[j], d.styles[i])) {
        style_map[i] = j;
        break;
      }
    }
  }

  std::vector<scav_rect> clips{ d.clips };
  scav_stable_sort(clips, rect_less);
  std::vector<scav_rect> unique_clips;
  for (scav_rect const &r : clips) {
    if (unique_clips.empty() || !rect_eq(unique_clips.back(), r)) {
      unique_clips.push_back(r);
    }
  }
  std::vector<uint32_t> clip_map(d.clips.size(), 0);
  for (uint32_t i = 0; i < d.clips.size(); ++i) {
    for (uint32_t j = 0; j < unique_clips.size(); ++j) {
      if (rect_eq(unique_clips[j], d.clips[i])) {
        clip_map[i] = j;
        break;
      }
    }
  }

  for (scav_prim &p : d.prims) {
    p.style = style_map.empty() ? 0U : style_map[p.style];
    if (p.clip != SCAV_CLIP_NONE) { p.clip = clip_map.empty() ? 0U : clip_map[p.clip]; }
  }
  d.styles = unique_styles;
  d.clips = unique_clips;

  // The key is built once per primitive rather than inside the comparator: a
  // merge sort asks O(n log n) times and the bytes do not change.
  struct Keyed {
    std::vector<scav_byte> key;
    uint32_t row;
  };
  std::vector<Keyed> keyed(d.prims.size());
  for (uint32_t i = 0; i < d.prims.size(); ++i) {
    keyed[i].row = i;
    append_content(keyed[i].key, d, d.prims[i]);
  }
  scav_stable_sort(keyed, [](Keyed const &a, Keyed const &b) { return a.key < b.key; });

  // Points and text are rebuilt in the new order, so their offsets carry no
  // memory of emission order either.
  DrawList out;
  out.styles = d.styles;
  out.clips = d.clips;
  out.prims.reserve(d.prims.size());
  out.points.reserve(d.points.size());
  out.text.bytes.reserve(d.text.bytes.size());
  for (Keyed const &k : keyed) {
    scav_prim p{ d.prims[k.row] };
    uint32_t const points_off{ static_cast<uint32_t>(out.points.size()) };
    for (uint32_t i = 0; i < p.points.len; ++i) {
      out.points.push_back(d.points[p.points.off + i]);
    }
    p.points = { .off = points_off, .len = p.points.len };
    uint32_t const text_off{ static_cast<uint32_t>(out.text.bytes.size()) };
    for (uint32_t i = 0; i < p.payload.len; ++i) {
      out.text.bytes.push_back(d.text.bytes[p.payload.off + i]);
    }
    p.payload = { .off = (p.payload.len == 0U) ? 0U : text_off, .len = p.payload.len };
    out.prims.push_back(p);
  }
  d = out;
}

uint32_t drawlist_digest(DrawList const &d, Metrics const &m) {
  std::vector<scav_byte> b;
  // The font opens it: this is where glyph advances live, so it is where font
  // identity is a hashed input rather than something inferred from geometry.
  append_u32(b, m.identity);
  append_u32(b, static_cast<uint32_t>(d.styles.size()));
  for (scav_style const &s : d.styles) {
    append_u32(b, s.stroke_rgba);
    append_u32(b, s.fill_rgba);
    append_i32(b, s.stroke_w);
    append_u32(b, s.dash);
    append_i32(b, s.font_size_grid);
  }
  append_u32(b, static_cast<uint32_t>(d.clips.size()));
  for (scav_rect const &r : d.clips) {
    append_i32(b, r.x);
    append_i32(b, r.y);
    append_i32(b, r.w);
    append_i32(b, r.h);
  }
  append_u32(b, static_cast<uint32_t>(d.prims.size()));
  for (scav_prim const &p : d.prims) { append_content(b, d, p); }
  return xxhash32(b.data(), b.size(), 0);
}

void drawlist_append(DrawList &dst, DrawList const &src) {
  uint32_t const point_base{ static_cast<uint32_t>(dst.points.size()) };
  uint32_t const text_base{ static_cast<uint32_t>(dst.text.bytes.size()) };
  dst.points.insert(dst.points.end(), src.points.begin(), src.points.end());
  dst.text.bytes.insert(dst.text.bytes.end(), src.text.bytes.begin(),
                        src.text.bytes.end());

  // Styles and clips intern rather than concatenate, so appending a list twice
  // does not double the tables it shares.
  std::vector<uint32_t> style_map(src.styles.size(), 0);
  for (uint32_t i = 0; i < src.styles.size(); ++i) {
    style_map[i] = drawlist_style(dst, src.styles[i]);
  }
  std::vector<uint32_t> clip_map(src.clips.size(), 0);
  for (uint32_t i = 0; i < src.clips.size(); ++i) {
    clip_map[i] = drawlist_clip(dst, src.clips[i]);
  }

  for (scav_prim const &p : src.prims) {
    scav_prim moved{ p };
    moved.style = (p.style < style_map.size()) ? style_map[p.style] : 0U;
    if (p.clip != SCAV_CLIP_NONE) {
      moved.clip = (p.clip < clip_map.size()) ? clip_map[p.clip] : SCAV_CLIP_NONE;
    }
    moved.points = { .off = p.points.off + point_base, .len = p.points.len };
    moved.payload = { .off = (p.payload.len == 0U) ? 0U : (p.payload.off + text_base),
                      .len = p.payload.len };
    dst.prims.push_back(moved);
  }
}

}  // namespace scav
