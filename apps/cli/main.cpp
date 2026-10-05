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
  "  dump [--hash|--json] [--layout [LAYOUT...]] [--trace "
  "[--trace-search|--trace-outline]|--search-stats] "
  "<file>  the model; --layout adds geometry and the flags it rests on, --trace its "
  "decisions, --search-stats the search's counts and CPU\n"
  "  render [-o FILE] [--embed-font] [LAYOUT...] <file>"
  "   chart -> SVG\n"
  "  selftest [--against FILE]   recompute the layout hashes on this toolchain "
  "and diff against the goldens\n"
  "\n"
  "  LAYOUT: --profile NAME, --portfolio-row N, --rank S:R, --cut T:L, "
  "--reverse T:L, --end T:L:E:F, --orient F, --fold F:M[:L], --loop S:F:E, "
  "--no-search, "
  "--no-text\n"
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
    bool trace_outline{ false };
    bool search_stats{ false };
    LayoutArgs args;
    for (int i = 2; i < argc; ++i) {
      ArgRead const read{ read_layout_arg(argc, argv, i, args) };
      if (read == ArgRead::Malformed) { return usage(); }
      if (read == ArgRead::Taken) { continue; }
      std::string_view const arg{ argv[i] };
      bool *flag{ nullptr };
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
      } else if (arg == "--trace-outline") {
        flag = &trace_outline;
      } else if (arg == "--search-stats") {
        flag = &search_stats;
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
    // `--layout` gates layout flags, `--trace` and `--search-stats`; `--trace` gates its
    // two scopes. Exclusive: the two scopes, stats and trace, hash and json or layout.
    if ((path == nullptr) || (hash && (json || layout)) ||
        ((trace_search || trace_outline) && !trace) || (trace_search && trace_outline) ||
        (search_stats && trace) || ((args.given || trace || search_stats) && !layout)) {
      return usage();
    }
    TraceScope scope{ TraceScope::Shipped };
    if (trace_search) { scope = TraceScope::Search; }
    if (trace_outline) { scope = TraceScope::Outline; }
    if (search_stats) { scope = TraceScope::Stats; }
    return run_dump(path, hash, json, layout, trace || search_stats, scope, args);
  }

  if (verb == "render") {
    char const *out{ nullptr };
    bool embed{ false };
    LayoutArgs args;
    for (int i = 2; i < argc; ++i) {
      ArgRead const read{ read_layout_arg(argc, argv, i, args) };
      if (read == ArgRead::Malformed) { return usage(); }
      if (read == ArgRead::Taken) { continue; }
      std::string_view const arg{ argv[i] };
      if (arg == "-o") {
        if (((i + 1) >= argc) || (out != nullptr)) { return usage(); }
        out = argv[++i];
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
    return run_render(path, out, embed, args.profile, args);
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
