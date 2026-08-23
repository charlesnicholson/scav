// `scav render`: chart to SVG, which is the first user-visible thing scav does.
// Measure with the bundled font, lay out, build, render.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_draw.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_svg.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cli {

namespace {

char const *why(SvgStatus status) {
  switch (status) {
    case SvgStatus::Ok: return "";
    case SvgStatus::InvalidDrawList: return "the builder produced an invalid drawlist";
    case SvgStatus::UnsupportedPrim: return "the drawlist holds a primitive this "
                                            "backend does not render";
    case SvgStatus::UnknownImage: return "the drawlist names an unregistered image";
    case SvgStatus::MissingGlyph: return "the bundled font has no glyph for some text";
    case SvgStatus::ExtentOverflow: return "the diagram does not fit an integer viewBox";
  }
  return "unknown";
}

}  // namespace

int run_render(char const *path,
               char const *out_path,
               bool embed_font,
               char const *profile_name) {
  Loaded net;
  load_and_report(path, true, net);
  if (net.code == EXIT_UNUSABLE) { return EXIT_UNUSABLE; }

  scav_layout_opts opts{};
  if (!profile_named(profile_name, opts.profile)) {
    write_error("no such profile", profile_name);
    return EXIT_UNUSABLE;
  }

  Metrics metrics;
  Spaces spaces;
  if (!metrics_create(nullptr, 0, metrics) ||
      !measure_chart(net.chart, metrics, opts.profile, spaces)) {
    write_error("cannot measure the chart with the bundled font", path);
    return EXIT_UNUSABLE;
  }

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  if (!layout_run(net.chart, as_spaces(spaces), opts, placed, diags)) {
    std::string err;
    for (Diagnostic const &d : diags) { diag_append(err, net.chart, d, path); }
    write_stream(err, stderr);
    return EXIT_DIAGNOSED;
  }

  DrawList list;
  if (!emit_chart(list,
                  net.chart,
                  metrics,
                  palette_standard(),
                  as_spaces(spaces),
                  placed.data(),
                  static_cast<uint32_t>(placed.size()),
                  0)) {
    write_error("the reference builder found no geometry", path);
    return EXIT_UNUSABLE;
  }
  drawlist_canonicalize(list);

  std::string doc;
  uint32_t bad{ 0 };
  SvgOptions const svg{ .embed_font = embed_font, .margin = opts.profile.pad };
  SvgStatus const status{ svg_write(list, metrics, {}, svg, doc, bad) };
  if (status != SvgStatus::Ok) {
    write_error(why(status), path);
    return EXIT_UNUSABLE;
  }

  if (out_path == nullptr) {
    write_stream(doc, stdout);
    return net.code;
  }
  if (!write_file(out_path,
                  reinterpret_cast<scav_byte const *>(doc.data()),
                  doc.size())) {
    write_error("cannot write", out_path);
    return EXIT_UNUSABLE;
  }
  return net.code;
}

}  // namespace cli
