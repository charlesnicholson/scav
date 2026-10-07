// `scav render`: chart to SVG. Measures with the bundled font, lays out, builds,
// renders.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_draw.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_svg.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#else
#  include <unistd.h>
#endif

namespace cli {

namespace {

// This process's executable, or empty.
std::string executable_path() {
  std::string path(4096, '\0');
#ifdef _WIN32
  DWORD const n{
    GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()))
  };
  path.resize((n < path.size()) ? n : 0U);
#elif defined(__APPLE__)
  auto size{ static_cast<uint32_t>(path.size()) };
  bool const got{ _NSGetExecutablePath(path.data(), &size) == 0 };
  path.resize(got ? std::strlen(path.c_str()) : 0U);
#else
  ssize_t const n{ readlink("/proc/self/exe", path.data(), path.size()) };
  bool const got{ (n > 0) && (static_cast<size_t>(n) < path.size()) };
  path.resize(got ? static_cast<size_t>(n) : 0U);
#endif
  return path;
}

// $SCAV_FONT alone when set; otherwise the installed copy relative to the
// executable, then the source tree's for a build-tree run.
bool read_bundled_font(std::vector<scav_byte> &out) {
  if (char const *const forced{ std::getenv("SCAV_FONT") }; forced != nullptr) {
    return read_file(forced, out);
  }
  std::string installed{ executable_path() };
  if (size_t const slash{ installed.find_last_of("/\\") }; slash != std::string::npos) {
    installed.resize(slash + 1U);
    installed += SCAV_FONT_FROM_BIN;
    if (read_file(installed.c_str(), out)) { return true; }
  }
  return read_file(SCAV_FONT_IN_SOURCE, out);
}

char const *why(SvgStatus status) {
  switch (status) {
    case SvgStatus::Ok: return "";
    case SvgStatus::InvalidDrawList: return "the builder produced an invalid drawlist";
    case SvgStatus::UnsupportedPrim:
      return "the drawlist holds a primitive this "
             "backend does not render";
    case SvgStatus::UnknownImage: return "the drawlist names an unregistered image";
    case SvgStatus::MissingGlyph: return "the bundled font has no glyph for some text";
    case SvgStatus::ExtentOverflow: return "the diagram does not fit an integer viewBox";
    case SvgStatus::FontMismatch: return "the font to embed is not the bundled font";
  }
  return "unknown";
}

}  // namespace

int run_render(char const *path,
               char const *out_path,
               bool embed_font,
               char const *profile_name,
               LayoutArgs const &args) {
  Loaded net;
  load_and_report(path, true, net);
  if (net.code == EXIT_UNUSABLE) { return EXIT_UNUSABLE; }

  scav_layout_opts opts{};
  if (!profile_named(profile_name, opts.profile)) {
    write_error("no such profile", profile_name);
    return EXIT_UNUSABLE;
  }
  apply_layout_args(args, opts.profile);

  Metrics metrics;
  Spaces spaces;
  if (!metrics_create(nullptr, 0, metrics) ||
      (!args.no_text && !measure_chart(net.chart, metrics, opts.profile, spaces))) {
    write_error("cannot measure the chart with the bundled font", path);
    return EXIT_UNUSABLE;
  }

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  bool const laid{ layout_run(net.chart,
                              as_spaces(spaces),
                              opts,
                              placed,
                              diags,
                              nullptr,
                              nullptr,
                              args.row,
                              nullptr,
                              nullptr,
                              &args.pins) };
  if (!diags.empty()) {
    std::string err;
    for (Diagnostic const &d : diags) { diag_append(err, net.chart, d, path); }
    write_stream(err, stderr);
  }
  if (!laid) { return EXIT_DIAGNOSED; }

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

  std::vector<scav_byte> ttf;
  if (embed_font && (!read_bundled_font(ttf) || ttf.empty())) {
    write_error("cannot read the bundled font to embed", SCAV_FONT_FILE);
    return EXIT_UNUSABLE;
  }
  std::string doc;
  uint32_t bad{ 0 };
  SvgOptions const svg{ .embed_font = embed_font ? ttf.data() : nullptr,
                        .embed_font_len = static_cast<uint32_t>(ttf.size()),
                        .margin = opts.profile.pad };
  SvgStatus const status{ svg_write(list, metrics, {}, svg, doc, bad) };
  if (status != SvgStatus::Ok) {
    write_error(why(status), path);
    return EXIT_UNUSABLE;
  }

  if (out_path == nullptr) {
    if (!write_stream(doc, stdout)) {
      write_error("cannot write", "stdout");
      return EXIT_UNUSABLE;
    }
    return net.code;
  }
  if (!write_file(out_path, reinterpret_cast<scav_byte const *>(doc.data()), doc.size())) {
    write_error("cannot write", out_path);
    return EXIT_UNUSABLE;
  }
  return net.code;
}

}  // namespace cli
