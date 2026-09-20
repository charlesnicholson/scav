// Output streams and exit codes, which are a process's to choose. Everything
// below them -- loading, validating, rendering a diagnostic -- is core's.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace cli {

void write_stream(std::string const &text, std::FILE *to) {
  std::ignore = std::fwrite(text.data(), 1, text.size(), to);
}

void write_error(std::string_view what, std::string_view path) {
  std::string err{ "scav: " };
  err += what;
  err += " '";
  err += path;
  err += "'\n";
  write_stream(err, stderr);
}

bool portfolio_row(char const *text, uint32_t &out) {
  std::string_view const arg{ text };
  uint32_t at{ 0 };
  std::from_chars_result const got{
    std::from_chars(arg.data(), arg.data() + arg.size(), at)
  };
  if ((got.ec != std::errc{}) || (got.ptr != (arg.data() + arg.size()))) { return false; }
  if (at >= LAYOUT_SEARCH_ROWS) { return false; }
  out = at;
  return true;
}

// `TRANS:LEG`, appended. Names a segment for phase 1 to leave unchained, so a
// reader can see the drawing the objective declined (11.10b), the way
// `--portfolio-row` shows the tuple it declined. False on anything else.
bool chain_cut(char const *text, std::vector<ChainCut> &out) {
  std::string_view const arg{ text };
  size_t const colon{ arg.find(':') };
  if ((colon == std::string_view::npos) || (colon == 0) ||
      (colon + 1 == arg.size())) {
    return false;
  }
  uint32_t trans{ 0 };
  uint32_t leg{ 0 };
  std::from_chars_result const a{
    std::from_chars(arg.data(), arg.data() + colon, trans)
  };
  std::from_chars_result const b{
    std::from_chars(arg.data() + colon + 1, arg.data() + arg.size(), leg)
  };
  if ((a.ec != std::errc{}) || (a.ptr != (arg.data() + colon))) { return false; }
  if ((b.ec != std::errc{}) || (b.ptr != (arg.data() + arg.size()))) { return false; }
  out.push_back({ .trans = TransId{ trans }, .leg = leg });
  return true;
}

void load_and_report(char const *path, bool validate, Loaded &out) {
  std::string failed;
  bool const loaded{ load_file(path, out.loader, out.chart, out.diags, failed) };
  if (!failed.empty()) {
    write_error("cannot read", failed);
    out.code = EXIT_UNUSABLE;
    return;
  }

  std::string err;
  // A load that never reached a chart leaves nothing to print, and its findings
  // index the loader's buffers rather than a chart's.
  if (out.chart.documents.empty()) {
    for (Diagnostic const &d : out.diags) { diag_append(err, out.loader, d, path); }
    write_stream(err, stderr);
    out.code = EXIT_UNUSABLE;
    return;
  }

  bool clean{ loaded };
  if (validate) { clean = validate_chart(out.chart, out.diags) && clean; }
  for (Diagnostic const &d : out.diags) { diag_append(err, out.chart, d, path); }
  write_stream(err, stderr);
  out.code = clean ? EXIT_CLEAN : EXIT_DIAGNOSED;
}

}  // namespace cli
