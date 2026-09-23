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

// `--portfolio-row N`: a row of 11.10's table in place of the search, so
// `render` and `dump` can both be pointed at one candidate and the drawing and
// the geometry come from the same one. False on anything that is not a row.
// `INVALID` is the search, which is what every verb does unasked.
bool portfolio_row(char const *text, uint32_t &out);

// `TRANS:LEG` appended to `out`: a segment phase 1 leaves unchained, so the
// drawing the objective declined can be looked at (11.10b). False on anything
// that is not a pair of ordinals.
bool chain_cut(char const *text, std::vector<ChainCut> &out);

// `TRANS:LEG` appended: a segment phase 1 turns around before it breaks cycles,
// so the drawing a different reversal gives can be looked at (11.10d).
bool reverse_pin(char const *text, std::vector<ReversePin> &out);

// `TRANS:LEG:END:FACE` appended -- end 0 departure, 1 arrival; face 0 left, 1
// right, 2 top, 3 bottom (11.10e).
bool face_pin(char const *text, std::vector<FacePin> &out);

int run_dump(char const *path,
             bool hash_only,
             bool as_json,
             bool with_layout,
             uint32_t row,
             bool trace,
             bool trace_search,
             std::vector<ChainCut> const &cuts,
             std::vector<ReversePin> const &reverses,
             std::vector<FacePin> const &faces);
int run_render(char const *path,
               char const *out_path,
               bool embed_font,
               char const *profile_name,
               uint32_t row,
               std::vector<ChainCut> const &cuts,
               std::vector<ReversePin> const &reverses,
             std::vector<FacePin> const &faces);
int run_fmt(std::vector<char const *> const &paths, bool check_only);
int run_deps(char const *path, char const *target);
int run_selftest(char const *against_path);

}  // namespace cli

#endif  // SCAV_APPS_CLI_CLI_H_INCLUDED
