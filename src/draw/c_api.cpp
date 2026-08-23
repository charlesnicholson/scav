// The C projection of scav_draw.h. Each function converts its arguments, calls
// one draw function, and converts the result.

#include "scav/scav_draw_c.h"

#include "scav/scav_core_c.h"
#include "scav/scav_draw.h"
#include "scav/scav_types.h"
#include "draw/handles.h"
#include "scav_c_handles.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

static_assert(sizeof(scav_style) == 20);
static_assert(sizeof(scav_prim) == 48);

namespace {

scav_result measure_result(scav::MeasureStatus st) {
  switch (st) {
    case scav::MeasureStatus::Ok: return SCAV_OK;
    case scav::MeasureStatus::MissingGlyph: return SCAV_E_NO_GLYPH;
    case scav::MeasureStatus::BadUtf8:
    case scav::MeasureStatus::Newline:
    case scav::MeasureStatus::BadSize: return SCAV_E_INVALID_ARG;
  }
  return SCAV_E_INVALID_ARG;
}

// A span against a flat byte pool, under the rule every other span accessor
// follows: zero length reads back NULL and zero, past the end is an error.
scav_result span_out(std::vector<scav_byte> const &pool,
                     scav_span ref,
                     scav_byte const **out,
                     uint32_t *out_len) {
  if ((out == nullptr) || (out_len == nullptr)) { return SCAV_E_INVALID_ARG; }
  if ((ref.off > pool.size()) || (ref.len > (pool.size() - ref.off))) {
    return SCAV_E_INVALID_ARG;
  }
  *out = (ref.len == 0U) ? nullptr : (pool.data() + ref.off);
  *out_len = ref.len;
  return SCAV_OK;
}

}  // namespace

extern "C" {

scav_result scav_metrics_create(scav_byte const *ttf, uint32_t len, scav_metrics **out) {
  if (out == nullptr) { return SCAV_E_INVALID_ARG; }
  *out = nullptr;
  auto *handle{ new scav_metrics{} };
  if (!scav::metrics_create(ttf, len, handle->metrics)) {
    delete handle;
    return SCAV_E_FONT;
  }
  *out = handle;
  return SCAV_OK;
}

void scav_metrics_destroy(scav_metrics *metrics) { delete metrics; }

scav_result scav_metrics_identity(scav_metrics const *metrics, uint32_t *out) {
  if ((metrics == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out = metrics->metrics.identity;
  return SCAV_OK;
}

scav_result scav_metrics_units_per_em(scav_metrics const *metrics, uint32_t *out) {
  if ((metrics == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out = metrics->metrics.units_per_em;
  return SCAV_OK;
}

scav_result scav_metrics_glyph_count(scav_metrics const *metrics, uint32_t *out) {
  if ((metrics == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out = metrics->metrics.num_glyphs;
  return SCAV_OK;
}

scav_result scav_measure_text(scav_metrics const *metrics,
                              scav_byte const *utf8_nfc,
                              uint32_t len,
                              int32_t font_size_grid,
                              scav_extent *out) {
  if ((metrics == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  return measure_result(
      scav::measure_text(metrics->metrics, utf8_nfc, len, font_size_grid, *out));
}

scav_result scav_line_height(int32_t font_size_grid,
                             int32_t k_num,
                             int32_t k_den,
                             int32_t *out) {
  if (out == nullptr) { return SCAV_E_INVALID_ARG; }
  *out = scav::line_height(font_size_grid, k_num, k_den);
  return (*out == 0) ? SCAV_E_INVALID_ARG : SCAV_OK;
}

scav_result scav_measure_block(scav_metrics const *metrics,
                               scav_byte const *utf8_nfc,
                               uint32_t len,
                               int32_t font_size_grid,
                               int32_t k_num,
                               int32_t k_den,
                               scav_extent *out) {
  if ((metrics == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  return measure_result(scav::measure_block(
      metrics->metrics, utf8_nfc, len, font_size_grid, k_num, k_den, *out));
}

scav_result scav_drawlist_create(scav_drawlist **out) {
  if (out == nullptr) { return SCAV_E_INVALID_ARG; }
  *out = new scav_drawlist{};
  return SCAV_OK;
}

void scav_drawlist_destroy(scav_drawlist *list) { delete list; }

scav_result scav_drawlist_counts(scav_drawlist const *list,
                                 uint32_t *out_prims,
                                 uint32_t *out_styles,
                                 uint32_t *out_points,
                                 uint32_t *out_clips,
                                 uint32_t *out_text) {
  if (list == nullptr) { return SCAV_E_INVALID_ARG; }
  scav::DrawList const &d{ list->list };
  if (out_prims != nullptr) { *out_prims = static_cast<uint32_t>(d.prims.size()); }
  if (out_styles != nullptr) { *out_styles = static_cast<uint32_t>(d.styles.size()); }
  if (out_points != nullptr) { *out_points = static_cast<uint32_t>(d.points.size()); }
  if (out_clips != nullptr) { *out_clips = static_cast<uint32_t>(d.clips.size()); }
  if (out_text != nullptr) {
    *out_text = static_cast<uint32_t>(d.text.bytes.size());
  }
  return SCAV_OK;
}

scav_result scav_drawlist_prims(scav_drawlist const *list,
                                scav_prim const **out,
                                uint32_t *out_count) {
  if ((list == nullptr) || (out == nullptr) || (out_count == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  *out = list->list.prims.empty() ? nullptr : list->list.prims.data();
  *out_count = static_cast<uint32_t>(list->list.prims.size());
  return SCAV_OK;
}

scav_result scav_drawlist_styles(scav_drawlist const *list,
                                 scav_style const **out,
                                 uint32_t *out_count) {
  if ((list == nullptr) || (out == nullptr) || (out_count == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  *out = list->list.styles.empty() ? nullptr : list->list.styles.data();
  *out_count = static_cast<uint32_t>(list->list.styles.size());
  return SCAV_OK;
}

scav_result scav_drawlist_points(scav_drawlist const *list,
                                 scav_point const **out,
                                 uint32_t *out_count) {
  if ((list == nullptr) || (out == nullptr) || (out_count == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  *out = list->list.points.empty() ? nullptr : list->list.points.data();
  *out_count = static_cast<uint32_t>(list->list.points.size());
  return SCAV_OK;
}

scav_result scav_drawlist_clips(scav_drawlist const *list,
                                scav_rect const **out,
                                uint32_t *out_count) {
  if ((list == nullptr) || (out == nullptr) || (out_count == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  *out = list->list.clips.empty() ? nullptr : list->list.clips.data();
  *out_count = static_cast<uint32_t>(list->list.clips.size());
  return SCAV_OK;
}

scav_result scav_drawlist_str(scav_drawlist const *list,
                              scav_span payload,
                              scav_byte const **out,
                              uint32_t *out_len) {
  if (list == nullptr) { return SCAV_E_INVALID_ARG; }
  return span_out(list->list.text.bytes, payload, out, out_len);
}

scav_result scav_drawlist_validate(scav_drawlist const *list, uint32_t *out_prim) {
  if (list == nullptr) { return SCAV_E_INVALID_ARG; }
  uint32_t bad{ 0 };
  bool const ok{ scav::drawlist_validate(list->list, bad) };
  if (out_prim != nullptr) { *out_prim = bad; }
  return ok ? SCAV_OK : SCAV_E_DRAWLIST;
}

scav_result scav_drawlist_canonicalize(scav_drawlist *list) {
  if (list == nullptr) { return SCAV_E_INVALID_ARG; }
  uint32_t bad{ 0 };
  // Canonicalizing an invalid list would index past an array; refusing here is
  // what lets the sort trust every span it reads.
  if (!scav::drawlist_validate(list->list, bad)) { return SCAV_E_DRAWLIST; }
  scav::drawlist_canonicalize(list->list);
  return SCAV_OK;
}

scav_result scav_drawlist_digest(scav_drawlist const *list,
                                 scav_metrics const *metrics,
                                 uint32_t *out) {
  if ((list == nullptr) || (metrics == nullptr) || (out == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  *out = scav::drawlist_digest(list->list, metrics->metrics);
  return SCAV_OK;
}

scav_result scav_drawlist_append(scav_drawlist *dst, scav_drawlist const *src) {
  if ((dst == nullptr) || (src == nullptr)) { return SCAV_E_INVALID_ARG; }
  scav::drawlist_append(dst->list, src->list);
  return SCAV_OK;
}

scav_result scav_images_create(scav_images **out) {
  if (out == nullptr) { return SCAV_E_INVALID_ARG; }
  *out = new scav_images{};
  return SCAV_OK;
}

void scav_images_destroy(scav_images *images) { delete images; }

scav_result scav_image_register(scav_images *images,
                               char const *id,
                               scav_byte const *bytes,
                               uint32_t len,
                               int32_t w,
                               int32_t h,
                               char const *mime) {
  if ((images == nullptr) || (id == nullptr) || (mime == nullptr) ||
      ((bytes == nullptr) && (len != 0U))) {
    return SCAV_E_INVALID_ARG;
  }
  // Dimensions come from registration and not from decoding, so a zero one is
  // the caller's error rather than something to infer later.
  if ((w <= 0) || (h <= 0) || (len == 0U)) { return SCAV_E_INVALID_ARG; }

  std::string_view const id_view{ id };
  std::string_view const mime_view{ mime };
  if (id_view.empty() || mime_view.empty()) { return SCAV_E_INVALID_ARG; }
  uint32_t found{ 0 };
  if (scav_image_find(images,
                      reinterpret_cast<scav_byte const *>(id_view.data()),
                      static_cast<uint32_t>(id_view.size()),
                      &found) == SCAV_OK) {
    return SCAV_E_STATE;  // an id names one image for the registry's life
  }

  auto const claim = [&](void const *from, size_t n) {
    uint32_t const off{ static_cast<uint32_t>(images->pool.size()) };
    auto const *raw{ static_cast<scav_byte const *>(from) };
    images->pool.insert(images->pool.end(), raw, raw + n);
    return scav::Span{ .off = off, .len = static_cast<uint32_t>(n) };
  };
  scav::Span const id_span{ claim(id_view.data(), id_view.size()) };
  scav::Span const mime_span{ claim(mime_view.data(), mime_view.size()) };
  scav::Span const byte_span{ claim(bytes, len) };
  images->rows.push_back({ .id = { .off = id_span.off, .len = id_span.len },
                           .mime = { .off = mime_span.off, .len = mime_span.len },
                           .bytes = byte_span,
                           .w = w,
                           .h = h });
  return SCAV_OK;
}

scav_result scav_image_count(scav_images const *images, uint32_t *out_count) {
  if ((images == nullptr) || (out_count == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out_count = static_cast<uint32_t>(images->rows.size());
  return SCAV_OK;
}

scav_result scav_image_find(scav_images const *images,
                            scav_byte const *id,
                            uint32_t id_len,
                            uint32_t *out_index) {
  if ((images == nullptr) || (id == nullptr) || (out_index == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  for (uint32_t i = 0; i < images->rows.size(); ++i) {
    scav::StrRef const ref{ images->rows[i].id };
    if ((ref.len == id_len) &&
        (std::memcmp(images->pool.data() + ref.off, id, id_len) == 0)) {
      *out_index = i;
      return SCAV_OK;
    }
  }
  return SCAV_E_INVALID_ARG;
}

scav_result scav_image_extent(scav_images const *images,
                              uint32_t index,
                              scav_extent *out) {
  if ((images == nullptr) || (out == nullptr) || (index >= images->rows.size())) {
    return SCAV_E_INVALID_ARG;
  }
  *out = { .w = images->rows[index].w, .h = images->rows[index].h };
  return SCAV_OK;
}

scav_result scav_palette_standard(scav_style *out, uint32_t cap) {
  if (out == nullptr) { return SCAV_E_INVALID_ARG; }
  scav::Palette const p{ scav::palette_standard() };
  if (cap < p.size()) { return SCAV_E_CAPACITY; }
  std::memcpy(out, p.data(), p.size() * sizeof(scav_style));
  return SCAV_OK;
}

scav_result scav_emit_chart(scav_drawlist *list,
                             scav_chart const *chart,
                             scav_metrics const *metrics,
                             scav_style const *palette,
                             uint32_t palette_len,
                             int32_t depth) {
  if ((list == nullptr) || (chart == nullptr) || (metrics == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  scav::Palette p{ scav::palette_standard() };
  if (palette != nullptr) {
    if (palette_len < SCAV_STYLE_COUNT) { return SCAV_E_INVALID_ARG; }
    p.assign(palette, palette + palette_len);
  }
  return scav::emit_chart(list->list, chart->chart, metrics->metrics, p, depth)
             ? SCAV_OK
             : SCAV_E_STATE;
}

} /* extern "C" */
