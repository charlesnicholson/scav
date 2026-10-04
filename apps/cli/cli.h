#ifndef SCAV_APPS_CLI_CLI_H_INCLUDED
#define SCAV_APPS_CLI_CLI_H_INCLUDED

// Declarations the CLI verbs share: exit codes, output, the load prologue, layout
// flags, and the verbs' entry points.

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

// Loads `path`, validates when `validate` is set, writes diagnostics to stderr,
// and sets `out.code`.
void load_and_report(char const *path, bool validate, Loaded &out);

// Layout inputs from `render` and `dump --layout`: a portfolio row or `INVALID`
// for the search, the starting pins, and whether to search from them.
struct LayoutArgs {
  char const *profile{ "readable" };
  uint32_t row{ INVALID };
  SearchPins pins;
  bool no_search{ false };
  bool no_text{ false };
  bool given{ false };  // any of the flags below appeared
};

enum class ArgRead : uint32_t { NotOurs, Taken, Malformed };

// One of these at `argv[i]`, with `i` advanced past its value:
//   --profile NAME       a shipped profile in place of `readable`
//   --portfolio-row N    a row in place of the search
//   --rank S:R           state S held at rank R of its frame
//   --cut T:L            leg L of transition T left unchained
//   --reverse T:L        that leg turned round before cycles break
//   --end T:L:E:F        end E (0 departs, 1 arrives) on face F (0 left, 1 right,
//                        2 top, 3 bottom) of its box, or of its port's state
//   --orient F           submachine F's ranks run down the page
//   --fold F:M[:L]       submachine F's run folds as the scale measure picks (M 0),
//                        always (1), or never (2); a nonzero L is the one rank
//                        the fold cuts before
//   --loop S:F:E         state S's loop room on face F of its free interior (as
//                        --end), at end E (0 top or left, 1 bottom or right)
//   --no-search          lay out the row and pins given, and move nothing
//   --no-text            lay out with no space requests, the scale the layout
//                        goldens are stated at
ArgRead read_layout_arg(int argc, char **argv, int &i, LayoutArgs &out);

// Appends the flags that reproduce a run: the non-default profile and `--no-text`
// from `args`, then `row` and every pin.
void append_layout_args(std::string &out,
                        LayoutArgs const &args,
                        uint32_t row,
                        SearchPins const &pins);

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
