#ifndef SCAV_APPS_CLI_CLI_H_INCLUDED
#define SCAV_APPS_CLI_CLI_H_INCLUDED

// What the verbs share, and nothing a library could own: the exit codes, the
// two output streams, and one entry point apiece.

#include "scav/scav_core.h"

#include <cstdio>
#include <string>
#include <vector>

namespace cli {

using namespace scav;

// Clean, the gate found something, the input could not be read or parsed at all.
constexpr int EXIT_CLEAN{ 0 };
constexpr int EXIT_DIAGNOSED{ 1 };
constexpr int EXIT_UNUSABLE{ 2 };

void write_stream(std::string const &text, std::FILE *to);

// `scav: ...` on stderr, which is how every verb reports a path it cannot use.
void write_error(std::string_view what, std::string_view path);

// A whole network, diagnostics already on stderr. EXIT_DIAGNOSED when the load
// or validation reported anything, EXIT_UNUSABLE when no chart was built.
struct Network {
  Loader loader;
  Chart chart;
  std::vector<Diagnostic> diags;
  int code;
};

// `validate` runs the structural checks as well as the load.
void load_network(char const *path, bool validate, Network &out);

int run_dump(char const *path, bool hash_only, bool as_json);
int run_fmt(std::vector<char const *> const &paths, bool check_only);
int run_deps(char const *path, char const *target);

}  // namespace cli

#endif  // SCAV_APPS_CLI_CLI_H_INCLUDED
