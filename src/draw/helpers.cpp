// The optional helper layer: interior subdivision, the lines an author wrote,
// and shape emission. Pure functions over PODs, and nothing in scav's pipeline
// calls any of them.

#include "scav/scav_draw.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_int.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

void stack_v(scav_rect r, int32_t const *heights, uint32_t n, scav_rect *out) {
  if ((heights == nullptr) || (out == nullptr)) { return; }
  int32_t y{ r.y };
  for (uint32_t i = 0; i < n; ++i) {
    int32_t const remaining{ imax(0, (r.y + r.h) - y) };
    int32_t const h{ imin(imax(0, heights[i]), remaining) };
    out[i] = { .x = r.x, .y = y, .w = r.w, .h = h };
    y += h;
  }
}

void row_h(scav_rect r, int32_t const *widths, uint32_t n, scav_rect *out) {
  if ((widths == nullptr) || (out == nullptr)) { return; }
  int32_t x{ r.x };
  for (uint32_t i = 0; i < n; ++i) {
    int32_t const remaining{ imax(0, (r.x + r.w) - x) };
    int32_t const w{ imin(imax(0, widths[i]), remaining) };
    out[i] = { .x = x, .y = r.y, .w = w, .h = r.h };
    x += w;
  }
}

scav_rect align(scav_rect r, int32_t w, int32_t h, Anchor a) {
  uint32_t const cell{ static_cast<uint32_t>(a) };
  uint32_t const col{ cell % 3U };
  uint32_t const row{ cell / 3U };
  // floor_div, not `/`: the slack is negative whenever the content is wider
  // than its rect, and truncation toward zero would bias that case one way.
  int32_t const slack_x{ r.w - w };
  int32_t const slack_y{ r.h - h };
  auto const place = [](int32_t origin, int32_t slack, uint32_t lane) {
    switch (lane) {
      case 0U: return origin;
      case 1U: return origin + floor_div(slack, 2);
      default: return origin + slack;
    }
  };
  return { .x = place(r.x, slack_x, col), .y = place(r.y, slack_y, row), .w = w, .h = h };
}

std::vector<std::string_view> text_lines(std::string_view s) {
  std::vector<std::string_view> lines;
  size_t start{ 0 };
  for (size_t i = 0; i <= s.size(); ++i) {
    if ((i != s.size()) && (s[i] != '\n')) { continue; }
    if ((i == s.size()) && (i != 0) && (s[i - 1] == '\n')) { break; }
    lines.push_back(s.substr(start, i - start));
    start = i + 1;
  }
  if (lines.empty()) { lines.emplace_back(); }
  return lines;
}

bool image_register(Images &images,
                    std::string_view id,
                    scav_byte const *bytes,
                    uint32_t len,
                    int32_t w,
                    int32_t h,
                    std::string_view mime) {
  if (id.empty() || mime.empty() || (bytes == nullptr) || (len == 0U) || (w <= 0) ||
      (h <= 0)) {
    return false;
  }
  if (image_find(images, id) != INVALID) { return false; }  // an id names one image

  auto const claim = [&images](void const *from, size_t n) {
    uint32_t const off{ static_cast<uint32_t>(images.pool.size()) };
    auto const *raw{ static_cast<scav_byte const *>(from) };
    images.pool.insert(images.pool.end(), raw, raw + n);
    return Span{ .off = off, .len = static_cast<uint32_t>(n) };
  };
  Span const id_span{ claim(id.data(), id.size()) };
  Span const mime_span{ claim(mime.data(), mime.size()) };
  Span const byte_span{ claim(bytes, len) };
  images.rows.push_back({ .id = { .off = id_span.off, .len = id_span.len },
                          .mime = { .off = mime_span.off, .len = mime_span.len },
                          .bytes = byte_span,
                          .w = w,
                          .h = h });
  return true;
}

uint32_t image_find(Images const &images, std::string_view id) {
  for (uint32_t i = 0; i < images.rows.size(); ++i) {
    if (image_str(images, images.rows[i].id) == id) { return i; }
  }
  return INVALID;
}

void push_arrowhead(DrawList &d,
                    int32_t depth,
                    uint32_t style,
                    scav_point tip,
                    scav_point from,
                    int32_t size,
                    ElemRef origin) {
  if (size <= 0) { return; }
  Wide const dx{ static_cast<Wide>(tip.x) - from.x };
  Wide const dy{ static_cast<Wide>(tip.y) - from.y };
  // isqrt floors, so the barbs sit a shade long rather than a shade short --
  // the same direction every other rounding here leans.
  Wide const len{ static_cast<Wide>(isqrt(static_cast<uint64_t>((dx * dx) + (dy * dy)))) };
  if (len == 0) { return; }

  Wide const back_x{ tip.x - floor_div(dx * size, len) };
  Wide const back_y{ tip.y - floor_div(dy * size, len) };
  Wide const half_x{ floor_div(dy * size, 2 * len) };
  Wide const half_y{ floor_div(dx * size, 2 * len) };
  std::array<scav_point, 3> const pts{
    tip,
    { .x = static_cast<int32_t>(back_x - half_x),
      .y = static_cast<int32_t>(back_y + half_y) },
    { .x = static_cast<int32_t>(back_x + half_x),
      .y = static_cast<int32_t>(back_y - half_y) },
  };
  push_path(d, depth, style, pts.data(), 3, origin);
}

}  // namespace scav
