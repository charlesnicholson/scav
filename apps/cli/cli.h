#ifndef SCAV_APPS_CLI_CLI_H_INCLUDED
#define SCAV_APPS_CLI_CLI_H_INCLUDED

// Shared by the verbs: loading a network, reporting its diagnostics, and the
// number and byte formatting that must not go through a locale.

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cli {

using namespace scav;

// The verbs agree on three: clean, the gate found something, the input could not
// be read or parsed at all.
constexpr int EXIT_CLEAN{ 0 };
constexpr int EXIT_DIAGNOSED{ 1 };
constexpr int EXIT_UNUSABLE{ 2 };

void write_stream(std::string const &text, std::FILE *to);

// Reads `path` whole. Writes a message to stderr and returns false when it
// cannot.
bool read_source(char const *path, std::vector<scav_byte> &out);

// Rewrites `path` with `text`. Writes a message to stderr on failure.
bool write_source(char const *path, std::string const &text);

void append_u32(std::string &out, uint32_t value);

// Eight lowercase hex digits and a newline, hand-rolled so no locale can reach
// it.
void append_hash(std::string &out, uint32_t value);

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
