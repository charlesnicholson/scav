// The scav executable, whose primary user is a build system: `fmt --check` and
// `check` as PR gates, `deps` against stale diagrams, `dump` for a consumer.

#include "cli.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using namespace cli;

constexpr std::string_view USAGE{
  "usage: scav <verb> [options] <chart.scav>\n"
  "\n"
  "  fmt [--check] <file>...      canonical print, in place; --check gates\n"
  "  check <file>                 structural validation, exit 1 on a finding\n"
  "  deps [--target NAME] <file>  the document network as a depfile\n"
  "  dump [--hash|--json] [--layout] [--portfolio-row N] [--trace "
  "[--trace-search]] <file>  the model; --layout adds geometry, --trace its "
  "decisions, --cut T:L leaves one segment unchained\n"
  "  render [-o FILE] [--embed-font] [--profile NAME] [--portfolio-row N] "
  "[--cut T:L] <file>"
  "   chart -> SVG\n"
  "  selftest [--against FILE]   recompute the layout hashes on this toolchain "
  "and diff against the goldens\n"
};

int usage() {
  write_stream(std::string{ USAGE }, stderr);
  return EXIT_UNUSABLE;
}

int dispatch(int argc, char **argv) {
  std::string_view const verb{ argv[1] };
  char const *path{ nullptr };

  if (verb == "dump") {
    bool hash{ false };
    bool json{ false };
    bool layout{ false };
    bool trace{ false };
    bool trace_search{ false };
    uint32_t row{ INVALID };
    std::vector<ChainCut> cuts;
    for (int i = 2; i < argc; ++i) {
      std::string_view const arg{ argv[i] };
      bool *flag{ nullptr };
      if (arg == "--portfolio-row") {
        // The increment is its own statement: clang-tidy's
        // bugprone-inc-dec-in-conditions is right that `++i` inside a compound
        // condition depends on an evaluation order a reader has to reconstruct.
        if (((i + 1) >= argc) || (row != INVALID)) { return usage(); }
        ++i;
        if (!portfolio_row(argv[i], row)) { return usage(); }
        continue;
      }
      if (arg == "--cut") {
        if ((i + 1) >= argc) { return usage(); }
        ++i;
        if (!chain_cut(argv[i], cuts)) { return usage(); }
        continue;
      }
      if (arg == "--hash") {
        flag = &hash;
      } else if (arg == "--json") {
        flag = &json;
      } else if (arg == "--layout") {
        flag = &layout;
      } else if (arg == "--trace") {
        flag = &trace;
      } else if (arg == "--trace-search") {
        flag = &trace_search;
      }
      if (flag != nullptr) {
        if (*flag) { return usage(); }
        *flag = true;
      } else if ((path == nullptr) && !arg.starts_with("-")) {
        path = argv[i];
      } else {
        return usage();
      }
    }
    // A pinned row only reaches layout, so it is the geometry's flag and not
    // the model's: `--hash` and a bare dump have nothing to point at.
    // `--trace` is layout's, like `--portfolio-row`: it prints the decisions
    // one run made and there are none without a run (11.16).
    // `--trace-search` is a mode of `--trace`, not a second flag beside it.
    if ((path == nullptr) || (hash && (json || layout)) || (trace_search && !trace) ||
        (((row != INVALID) || trace || !cuts.empty()) && !layout)) {
      return usage();
    }
    return run_dump(path, hash, json, layout, row, trace, trace_search, cuts);
  }

  if (verb == "render") {
    char const *out{ nullptr };
    char const *profile{ "readable" };
    bool embed{ false };
    uint32_t row{ INVALID };
    std::vector<ChainCut> cuts;
    for (int i = 2; i < argc; ++i) {
      std::string_view const arg{ argv[i] };
      if (arg == "--portfolio-row") {
        // The increment is its own statement: clang-tidy's
        // bugprone-inc-dec-in-conditions is right that `++i` inside a compound
        // condition depends on an evaluation order a reader has to reconstruct.
        if (((i + 1) >= argc) || (row != INVALID)) { return usage(); }
        ++i;
        if (!portfolio_row(argv[i], row)) { return usage(); }
      } else if (arg == "--cut") {
        if ((i + 1) >= argc) { return usage(); }
        ++i;
        if (!chain_cut(argv[i], cuts)) { return usage(); }
      } else if (arg == "-o") {
        if (((i + 1) >= argc) || (out != nullptr)) { return usage(); }
        out = argv[++i];
      } else if (arg == "--profile") {
        if ((i + 1) >= argc) { return usage(); }
        profile = argv[++i];
      } else if (arg == "--embed-font") {
        if (embed) { return usage(); }
        embed = true;
      } else if ((path == nullptr) && !arg.starts_with("-")) {
        path = argv[i];
      } else {
        return usage();
      }
    }
    if (path == nullptr) { return usage(); }
    return run_render(path, out, embed, profile, row, cuts);
  }

  if (verb == "selftest") {
    char const *against{ nullptr };
    for (int i = 2; i < argc; ++i) {
      if ((std::string_view{ argv[i] } != "--against") || ((i + 1) >= argc) ||
          (against != nullptr)) {
        return usage();
      }
      against = argv[++i];
    }
    return run_selftest(against);
  }

  if (verb == "check") {
    if ((argc != 3) || std::string_view{ argv[2] }.starts_with("-")) { return usage(); }
    Loaded net;
    load_and_report(argv[2], true, net);
    return net.code;
  }

  if (verb == "deps") {
    char const *target{ nullptr };
    for (int i = 2; i < argc; ++i) {
      std::string_view const arg{ argv[i] };
      if (arg == "--target") {
        if (((i + 1) >= argc) || (target != nullptr)) { return usage(); }
        target = argv[++i];
      } else if ((path == nullptr) && !arg.starts_with("-")) {
        path = argv[i];
      } else {
        return usage();
      }
    }
    if (path == nullptr) { return usage(); }
    return run_deps(path, target);
  }

  if (verb == "fmt") {
    bool check_only{ false };
    std::vector<char const *> paths;
    for (int i = 2; i < argc; ++i) {
      std::string_view const arg{ argv[i] };
      if (arg == "--check") {
        check_only = true;
      } else if (!arg.starts_with("-")) {
        paths.push_back(argv[i]);
      } else {
        return usage();
      }
    }
    if (paths.empty()) { return usage(); }
    return run_fmt(paths, check_only);
  }

  return usage();
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) { return usage(); }
  return dispatch(argc, argv);
}
