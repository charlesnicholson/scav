// The document network as a make/ninja depfile, so a build re-renders chart A
// when a document it includes changes. This is `gcc -M`.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace cli {

namespace {

// make and ninja read the same escapes here: a space is `\ `, a hash starts a
// comment, and a dollar opens a variable in both dialects.
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
  Network net;
  // Structural validity is `check`'s question: a network that resolved is enough
  // to name its files.
  load_network(path, false, net);
  if (net.code == EXIT_UNUSABLE) { return EXIT_UNUSABLE; }

  std::string out;
  append_depfile_path(out, (target != nullptr) ? target : path);
  out += ':';
  // Document order comes from the include graph rather than from arrival, so the
  // line is the same however the documents were fetched.
  for (Document const &doc : net.chart.documents) {
    out += ' ';
    append_depfile_path(out, chart_string(net.chart, doc.path));
  }
  out += '\n';
  write_stream(out, stdout);
  return net.code;
}

}  // namespace cli
