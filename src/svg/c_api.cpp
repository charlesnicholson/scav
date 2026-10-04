// C API over scav_svg.h, following the span accessors' out-param protocol.

#include "scav/scav_svg_c.h"

#include "draw/handles.h"
#include "scav/scav_core_c.h"
#include "scav/scav_draw.h"
#include "scav/scav_svg.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <cstring>
#include <string>

static_assert(sizeof(scav_svg_options) ==
              sizeof(scav_byte const *) + (2 * sizeof(int32_t)));

extern "C" {

scav_result scav_svg_write(scav_drawlist const *list,
                           scav_metrics const *metrics,
                           scav_images const *images,
                           scav_svg_options const *options,
                           uint32_t options_size,
                           scav_byte *out,
                           uint32_t cap,
                           uint32_t *out_count) {
  if (options_size != sizeof(scav_svg_options)) { return SCAV_E_ABI; }
  if ((list == nullptr) || (metrics == nullptr) || (out_count == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  scav::Images const none{};
  scav::SvgOptions opts{};
  if (options != nullptr) {
    if ((options->embed_font == nullptr) && (options->embed_font_len != 0)) {
      return SCAV_E_INVALID_ARG;
    }
    opts = { .embed_font = options->embed_font,
             .embed_font_len = options->embed_font_len,
             .margin = options->margin };
  }

  std::string doc;
  uint32_t bad{ 0 };
  scav::SvgStatus const st{ scav::svg_write(list->list,
                                            metrics->metrics,
                                            (images != nullptr) ? images->images : none,
                                            opts,
                                            doc,
                                            bad) };
  if (st == scav::SvgStatus::FontMismatch) { return SCAV_E_FONT; }
  if (st != scav::SvgStatus::Ok) { return SCAV_E_DRAWLIST; }

  *out_count = static_cast<uint32_t>(doc.size());
  if (cap == 0) { return SCAV_OK; /* count query */ }
  if (cap < doc.size()) { return SCAV_E_CAPACITY; }
  if (out == nullptr) { return SCAV_E_INVALID_ARG; }
  // Copies without a NUL terminator; `out_count` already holds the length.
  // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
  std::memcpy(out, doc.data(), doc.size());
  return SCAV_OK;
}

scav_result scav_svg_bounds(scav_drawlist const *list, scav_rect *out, uint32_t out_size) {
  if (out_size != sizeof(scav_rect)) { return SCAV_E_ABI; }
  if ((list == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out = scav::svg_bounds(list->list);
  return SCAV_OK;
}

} /* extern "C" */
