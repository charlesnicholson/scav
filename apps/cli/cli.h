#ifndef SCAV_APPS_CLI_CLI_H_INCLUDED
#define SCAV_APPS_CLI_CLI_H_INCLUDED

// What the verbs share, and nothing a library could own: the exit codes, the
// two output streams, and one entry point apiece.

#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
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

struct Loaded {
  Loader loader;
  Chart chart;
  std::vector<Diagnostic> diags;
  int code;
};

// The prologue three verbs share. Core does the loading, the validation and the
// rendering; the two decisions here are which stream and which exit code.
void load_and_report(char const *path, bool validate, Loaded &out);

// What `render` and `dump --layout` hand layout besides the chart: a row of
// 11.10's table, or `INVALID` for the search; the pins to start from; and
// whether to search from them at all. A run's own row and pins with
// `--no-search` lay that drawing out again exactly, so an edited copy of them
// is a counterfactual scored on the shipped objective (11.10g).
struct LayoutArgs {
  uint32_t row{ INVALID };
  SearchPins pins;
  bool no_search{ false };
  bool given{ false };  // any of the flags below appeared
};

enum class ArgRead : uint32_t { NotOurs, Taken, Malformed };

// One of these at `argv[i]`, with `i` advanced past its value:
//   --portfolio-row N    a row in place of the search
//   --rank S:R           state S held at rank R of its frame (11.10a)
//   --cut T:L            leg L of transition T left unchained (11.10b)
//   --reverse T:L        that leg turned round before cycles break (11.10d)
//   --face T:L:E:F       end E (0 departs, 1 arrives) leaves by face F: 0 left,
//                        1 right, 2 top, 3 bottom (11.10e)
//   --no-search          lay out the row and pins given, and move nothing
ArgRead read_layout_arg(int argc, char **argv, int &i, LayoutArgs &out);

// The flags above that lay out a run's drawing again: its row and every pin.
void append_layout_args(std::string &out, uint32_t row, SearchPins const &pins);

int run_dump(char const *path,
             bool hash_only,
             bool as_json,
             bool with_layout,
             bool trace,
             bool trace_search,
             LayoutArgs const &args);
int run_render(char const *path,
               char const *out_path,
               bool embed_font,
               char const *profile_name,
               LayoutArgs const &args);
int run_fmt(std::vector<char const *> const &paths, bool check_only);
int run_deps(char const *path, char const *target);
int run_selftest(char const *against_path);

}  // namespace cli

#endif  // SCAV_APPS_CLI_CLI_H_INCLUDED
