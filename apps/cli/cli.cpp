// Output streams and exit codes, which are a process's to choose. Everything
// below them -- loading, validating, rendering a diagnostic -- is core's.

#include "cli.h"

#include "scav/scav_core.h"

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
