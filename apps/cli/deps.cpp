// `scav deps`: the document network as a make/ninja depfile, like `gcc -M`.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace cli {

namespace {

// Escapes a path for make and ninja: space and `#` take a backslash, `$` doubles.
void append_depfile_path(std::string &out, std::string_view path) {
  for (char const ch : path) {
    switch (ch) {
      case ' ':
      case '#': out += '\\'; break;
      case '$': out += '$'; break;
      default: break;
    }
    out += ch;
  }
}

}  // namespace

int run_deps(char const *path, char const *target) {
  Loaded net;
  // Loads without validating.
  load_and_report(path, false, net);
  if (net.code == EXIT_UNUSABLE) { return EXIT_UNUSABLE; }

  std::string out;
  append_depfile_path(out, (target != nullptr) ? target : path);
  out += ':';
  // Documents are in include-graph order, independent of fetch order.
  for (Document const &doc : net.chart.documents) {
    out += ' ';
    append_depfile_path(out, chart_string(net.chart, doc.path));
  }
  out += '\n';
  write_stream(out, stdout);
  return net.code;
}

}  // namespace cli
