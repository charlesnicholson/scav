// Writes a DrawList as an SVG document with integer arithmetic only and the scale in the
// viewBox; every build and platform emits the same bytes.

#include "scav/scav_svg.h"

#include "scav/scav_draw.h"
#include "scav/scav_types.h"
#include "scav_int.h"
#include "scav_xxhash.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

namespace {

constexpr int32_t GRID_PER_PT{ 16 };

void put_i32(std::string &out, int32_t v) {
  if (v < 0) {
    out += '-';
    // Negates in int64 so INT32_MIN's magnitude fits in uint32.
    string_append_u32(out, static_cast<uint32_t>(-static_cast<int64_t>(v)));
    return;
  }
  string_append_u32(out, static_cast<uint32_t>(v));
}

void attr(std::string &out, char const *name, int32_t v) {
  out += ' ';
  out += name;
  out += "=\"";
  put_i32(out, v);
  out += '"';
}

// Escapes the five predefined XML entities and copies every other byte unchanged.
void put_text(std::string &out, std::string_view s) {
  for (char const c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c; break;
    }
  }
}

// `#rrggbb` plus a separate `-opacity` when alpha is below 255; zero alpha or `none`
// writes "none".
void put_paint(std::string &out, char const *name, uint32_t rgba, bool none) {
  out += ' ';
  out += name;
  if (none || ((rgba & 0xFFU) == 0U)) {
    out += "=\"none\"";
    return;
  }
  constexpr char const *HEX{ "0123456789abcdef" };
  out += "=\"#";
  for (uint32_t shift = 24; shift >= 8; shift -= 8) {
    uint32_t const byte{ (rgba >> shift) & 0xFFU };
    out += HEX[byte >> 4U];
    out += HEX[byte & 0x0FU];
  }
  out += '"';
  uint32_t const alpha{ rgba & 0xFFU };
  if (alpha != 0xFFU) {
    // Opacity as three decimal digits of alpha / 255, by integer division.
    uint32_t const thousandths{ (alpha * 1000U) / 255U };
    out += ' ';
    out += name;
    out += "-opacity=\"0.";
    out += static_cast<char>('0' + ((thousandths / 100U) % 10U));
    out += static_cast<char>('0' + ((thousandths / 10U) % 10U));
    out += static_cast<char>('0' + (thousandths % 10U));
    out += '"';
  }
}

// Standard base64 with `=` padding and no line breaks.
void put_base64(std::string &out, scav_byte const *bytes, uint32_t len) {
  constexpr char const *SET{
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  };
  uint32_t i{ 0 };
  for (; (i + 3U) <= len; i += 3U) {
    uint32_t const word{ (static_cast<uint32_t>(bytes[i]) << 16U) |
                         (static_cast<uint32_t>(bytes[i + 1U]) << 8U) | bytes[i + 2U] };
    out += SET[(word >> 18U) & 0x3FU];
    out += SET[(word >> 12U) & 0x3FU];
    out += SET[(word >> 6U) & 0x3FU];
    out += SET[word & 0x3FU];
  }
  uint32_t const left{ len - i };
  if (left == 1U) {
    uint32_t const word{ static_cast<uint32_t>(bytes[i]) << 16U };
    out += SET[(word >> 18U) & 0x3FU];
    out += SET[(word >> 12U) & 0x3FU];
    out += "==";
  } else if (left == 2U) {
    uint32_t const word{ (static_cast<uint32_t>(bytes[i]) << 16U) |
                         (static_cast<uint32_t>(bytes[i + 1U]) << 8U) };
    out += SET[(word >> 18U) & 0x3FU];
    out += SET[(word >> 12U) & 0x3FU];
    out += SET[(word >> 6U) & 0x3FU];
    out += '=';
  }
}

char const *kind_name(uint32_t origin_kind) {
  switch (static_cast<ElemKind>(origin_kind)) {
    case ElemKind::State: return "state";
    case ElemKind::Submachine: return "sub";
    case ElemKind::Transition: return "trans";
    case ElemKind::Chart: return "chart";
    case ElemKind::Point: return "point";
    case ElemKind::PathBox: return "pathbox";
    case ElemKind::None: return nullptr;
  }
  return nullptr;
}

void put_points(std::string &out, DrawList const &d, scav_prim const &p) {
  out += " points=\"";
  for (uint32_t i = 0; i < p.points.len; ++i) {
    if (i != 0) { out += ' '; }
    put_i32(out, d.points[p.points.off + i].x);
    out += ',';
    put_i32(out, d.points[p.points.off + i].y);
  }
  out += '"';
}

// Writes the fill, stroke, stroke-width and dash attributes; `stroke_only` sets the
// fill to none.
void put_style(std::string &out, scav_style const &s, bool stroke_only) {
  put_paint(out, "fill", s.fill_rgba, stroke_only);
  put_paint(out, "stroke", s.stroke_rgba, false);
  if ((s.stroke_rgba & 0xFFU) != 0U) { attr(out, "stroke-width", s.stroke_w); }
  if (s.dash != 0U) {
    // Any nonzero dash draws as 4 on, 3 off, in stroke widths (at least 1 each).
    out += " stroke-dasharray=\"";
    put_i32(out, imax(1, s.stroke_w * 4));
    out += ',';
    put_i32(out, imax(1, s.stroke_w * 3));
    out += '"';
  }
}

}  // namespace

scav_rect svg_bounds(DrawList const &d) {
  bool seen{ false };
  int32_t x0{ 0 };
  int32_t y0{ 0 };
  int32_t x1{ 0 };
  int32_t y1{ 0 };
  for (scav_prim const &p : d.prims) {
    for (uint32_t i = 0; i < p.points.len; ++i) {
      scav_point const pt{ d.points[p.points.off + i] };
      int32_t const reach{ (p.kind == SCAV_PRIM_CIRCLE) ? imax(0, p.a) : 0 };
      if (!seen) {
        x0 = pt.x - reach;
        y0 = pt.y - reach;
        x1 = pt.x + reach;
        y1 = pt.y + reach;
        seen = true;
        continue;
      }
      x0 = imin(x0, pt.x - reach);
      y0 = imin(y0, pt.y - reach);
      x1 = imax(x1, pt.x + reach);
      y1 = imax(y1, pt.y + reach);
    }
  }
  if (!seen) { return {}; }
  return { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
}

std::string svg_class(scav_prim const &p) {
  char const *const kind{ kind_name(p.origin_kind) };
  if (kind == nullptr) { return {}; }
  std::string out{ "scav-" };
  out += kind;
  out += " scav-id-";
  string_append_u32(out, p.origin_ordinal);
  return out;
}

namespace {

void put_class(std::string &out, scav_prim const &p) {
  std::string const value{ svg_class(p) };
  if (value.empty()) { return; }
  out += " class=\"";
  out += value;
  out += '"';
}

}  // namespace

SvgStatus svg_write(DrawList const &d,
                    Metrics const &m,
                    Images const &images,
                    SvgOptions const &o,
                    std::string &out,
                    uint32_t &bad) {
  bad = INVALID;
  if ((o.embed_font != nullptr) &&
      (xxhash32(o.embed_font, o.embed_font_len, 0) != m.identity)) {
    return SvgStatus::FontMismatch;
  }
  if (!drawlist_validate(d, bad)) { return SvgStatus::InvalidDrawList; }

  // Refuse before writing anything, so a failure never leaves half a document
  // in the caller's string.
  for (uint32_t i = 0; i < d.prims.size(); ++i) {
    scav_prim const &p{ d.prims[i] };
    bad = i;
    if (p.kind == SCAV_PRIM_ARC) { return SvgStatus::UnsupportedPrim; }
    if ((p.kind == SCAV_PRIM_IMAGE) &&
        (image_find(images,
                    { reinterpret_cast<char const *>(d.text.bytes.data() + p.payload.off),
                      p.payload.len }) == INVALID)) {
      return SvgStatus::UnknownImage;
    }
  }
  bad = INVALID;

  scav_rect const tight{ svg_bounds(d) };
  int32_t const margin{ imax(0, o.margin) };
  Wide const both_margins{ Wide{ 2 } * margin };
  Wide const view_w{ static_cast<Wide>(tight.w) + both_margins };
  Wide const view_h{ static_cast<Wide>(tight.h) + both_margins };
  if ((view_w > COORD_MAX) || (view_h > COORD_MAX)) { return SvgStatus::ExtentOverflow; }

  std::string body;
  for (scav_prim const &p : d.prims) {
    scav_style const &s{ d.styles[p.style] };
    scav_point const *pts{ d.points.data() + p.points.off };
    bool const text_style{ s.font_size_grid > 0 };

    body += "  <";
    switch (p.kind) {
      case SCAV_PRIM_RECT:
      case SCAV_PRIM_RRECT:
        body += "rect";
        attr(body, "x", pts[0].x);
        attr(body, "y", pts[0].y);
        attr(body, "width", pts[1].x - pts[0].x);
        attr(body, "height", pts[1].y - pts[0].y);
        if (p.kind == SCAV_PRIM_RRECT) { attr(body, "rx", p.a); }
        put_style(body, s, false);
        break;
      case SCAV_PRIM_LINE:
        body += "line";
        attr(body, "x1", pts[0].x);
        attr(body, "y1", pts[0].y);
        attr(body, "x2", pts[1].x);
        attr(body, "y2", pts[1].y);
        put_style(body, s, true);
        break;
      case SCAV_PRIM_POLYLINE:
        body += "polyline";
        put_points(body, d, p);
        put_style(body, s, true);  // an open run is a stroke, never a fill
        break;
      case SCAV_PRIM_PATH:
        body += "polygon";
        put_points(body, d, p);
        put_style(body, s, false);
        break;
      case SCAV_PRIM_CIRCLE:
        body += "circle";
        attr(body, "cx", pts[0].x);
        attr(body, "cy", pts[0].y);
        attr(body, "r", p.a);
        put_style(body, s, false);
        break;
      case SCAV_PRIM_IMAGE: {
        uint32_t const row{ image_find(
            images,
            { reinterpret_cast<char const *>(d.text.bytes.data() + p.payload.off),
              p.payload.len }) };
        Images::Row const &img{ images.rows[row] };
        body += "image";
        attr(body, "x", pts[0].x);
        attr(body, "y", pts[0].y);
        attr(body, "width", pts[1].x - pts[0].x);
        attr(body, "height", pts[1].y - pts[0].y);
        // Embeds the image bytes as a base64 data URI.
        body += " href=\"data:";
        body += image_str(images, img.mime);
        body += ";base64,";
        put_base64(body, images.pool.data() + img.bytes.off, img.bytes.len);
        body += '"';
        break;
      }
      case SCAV_PRIM_TEXT: {
        std::string_view const text{ reinterpret_cast<char const *>(d.text.bytes.data() +
                                                                    p.payload.off),
                                     p.payload.len };
        // textLength is the metrics' advance sum; a substituted font is spaced to fit it.
        scav_extent ext{};
        MeasureStatus const st{ measure_text(
            m,
            reinterpret_cast<scav_byte const *>(text.data()),
            static_cast<uint32_t>(text.size()),
            s.font_size_grid,
            ext) };
        if (st != MeasureStatus::Ok) {
          bad = static_cast<uint32_t>(&p - d.prims.data());
          return (st == MeasureStatus::MissingGlyph) ? SvgStatus::MissingGlyph
                                                     : SvgStatus::InvalidDrawList;
        }
        body += "text";
        attr(body, "x", pts[0].x);
        attr(body, "y", pts[0].y);
        attr(body, "font-size", s.font_size_grid);
        if (ext.w > 0) {
          attr(body, "textLength", ext.w);
          body += " lengthAdjust=\"spacing\"";
        }
        put_paint(body, "fill", s.fill_rgba, false);
        put_class(body, p);
        body += '>';
        put_text(body, text);
        body += "</text>\n";
        continue;  // the only kind with a closing tag
      }
      case SCAV_PRIM_ARC:
      default: return SvgStatus::UnsupportedPrim;  // refused above
    }
    if (!text_style) { put_class(body, p); }
    body += "/>\n";
  }

  std::string doc{ "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg" };
  doc += " xmlns=\"http://www.w3.org/2000/svg\"";
  // Output size in whole points, rounded up; the viewBox holds the exact extent.
  attr(doc, "width", ceil_div(static_cast<int32_t>(view_w), GRID_PER_PT));
  attr(doc, "height", ceil_div(static_cast<int32_t>(view_h), GRID_PER_PT));
  doc += " viewBox=\"";
  put_i32(doc, tight.x - margin);
  doc += ' ';
  put_i32(doc, tight.y - margin);
  doc += ' ';
  put_i32(doc, static_cast<int32_t>(view_w));
  doc += ' ';
  put_i32(doc, static_cast<int32_t>(view_h));
  doc += "\">\n";

  // JetBrains Mono with a monospace fallback, kerning off to match the metrics.
  doc +=
      "  <style>text { font-family: \"JetBrains Mono\", monospace;"
      " font-kerning: none; }</style>\n";
  if (o.embed_font != nullptr) {
    doc +=
        "  <defs><style>@font-face { font-family: \"JetBrains Mono\";"
        " src: url(data:font/ttf;base64,";
    put_base64(doc, o.embed_font, o.embed_font_len);
    doc += ") format(\"truetype\"); }</style></defs>\n";
  }
  doc += body;
  doc += "</svg>\n";
  out += doc;
  return SvgStatus::Ok;
}

}  // namespace scav
