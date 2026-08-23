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
  if ((images == nullptr) || (id == nullptr) || (mime == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  std::string_view const id_view{ id };
  if (scav::image_find(images->images, id_view) != scav::INVALID) {
    return SCAV_E_STATE;  // an id names one image for the registry's life
  }
  return scav::image_register(images->images, id_view, bytes, len, w, h, mime)
             ? SCAV_OK
             : SCAV_E_INVALID_ARG;
}

scav_result scav_image_count(scav_images const *images, uint32_t *out_count) {
  if ((images == nullptr) || (out_count == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out_count = static_cast<uint32_t>(images->images.rows.size());
  return SCAV_OK;
}

scav_result scav_image_find(scav_images const *images,
                            scav_byte const *id,
                            uint32_t id_len,
                            uint32_t *out_index) {
  if ((images == nullptr) || (id == nullptr) || (out_index == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  uint32_t const row{ scav::image_find(
      images->images,
      { reinterpret_cast<char const *>(id), id_len }) };
  if (row == scav::INVALID) { return SCAV_E_INVALID_ARG; }
  *out_index = row;
  return SCAV_OK;
}

scav_result scav_image_extent(scav_images const *images,
                              uint32_t index,
                              scav_extent *out) {
  if ((images == nullptr) || (out == nullptr) ||
      (index >= images->images.rows.size())) {
    return SCAV_E_INVALID_ARG;
  }
  *out = { .w = images->images.rows[index].w, .h = images->images.rows[index].h };
  return SCAV_OK;
}

scav_result scav_measure_chart(scav_chart const *chart,
                               scav_metrics const *metrics,
                               scav_profile const *profile,
                               scav_box_space *box_state,
                               uint32_t cap_box_state,
                               scav_box_space *box_sub,
                               uint32_t cap_box_sub,
                               scav_path_clear *path_clear,
                               uint32_t cap_path_clear,
                               scav_path_box *path_box,
                               uint32_t cap_path_box,
                               uint32_t *out_counts) {
  if ((chart == nullptr) || (metrics == nullptr) || (profile == nullptr) ||
      (out_counts == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  scav::Spaces spaces;
  if (!scav::measure_chart(chart->chart, metrics->metrics, *profile, spaces)) {
    return SCAV_E_STATE;
  }

  auto const count = [](auto const &v) { return static_cast<uint32_t>(v.size()); };
  out_counts[0] = count(spaces.box_state);
  out_counts[1] = count(spaces.box_sub);
  out_counts[2] = count(spaces.path_clear);
  out_counts[3] = count(spaces.path_box);
  if ((cap_box_state == 0) && (cap_box_sub == 0) && (cap_path_clear == 0) &&
      (cap_path_box == 0)) {
    return SCAV_OK; /* count query */
  }
  if ((cap_box_state < out_counts[0]) || (cap_box_sub < out_counts[1]) ||
      (cap_path_clear < out_counts[2]) || (cap_path_box < out_counts[3])) {
    return SCAV_E_CAPACITY;
  }

  auto const copy = [](auto *out, auto const &rows) {
    if (!rows.empty()) {
      if (out == nullptr) { return false; }
      std::memcpy(out, rows.data(), rows.size() * sizeof(rows[0]));
    }
    return true;
  };
  if (!copy(box_state, spaces.box_state) || !copy(box_sub, spaces.box_sub) ||
      !copy(path_clear, spaces.path_clear) || !copy(path_box, spaces.path_box)) {
    return SCAV_E_INVALID_ARG;
  }
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
                            scav_spaces const *spaces,
                            scav_placed const *placed,
                            uint32_t placed_count,
                            int32_t depth) {
  if ((list == nullptr) || (chart == nullptr) || (metrics == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  scav::Palette p{ scav::palette_standard() };
  if (palette != nullptr) {
    if (palette_len < SCAV_STYLE_COUNT) { return SCAV_E_INVALID_ARG; }
    p.assign(palette, palette + palette_len);
  }
  scav_spaces const none{};
  return scav::emit_chart(list->list,
                          chart->chart,
                          metrics->metrics,
                          p,
                          (spaces != nullptr) ? *spaces : none,
                          placed,
                          placed_count,
                          depth)
             ? SCAV_OK
             : SCAV_E_STATE;
}

} /* extern "C" */
