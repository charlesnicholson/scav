// The scav executable, whose primary user is a build system: `fmt --check` and
// `check` as PR gates, `deps` against stale diagrams, `dump` for a consumer.

#include "cli.h"

#include <cstddef>
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
  "  dump [--hash|--json] [--layout] <file>  the model; --layout adds geometry\n"
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
    for (int i = 2; i < argc; ++i) {
      std::string_view const arg{ argv[i] };
      bool *flag{ nullptr };
      if (arg == "--hash") {
        flag = &hash;
      } else if (arg == "--json") {
        flag = &json;
      } else if (arg == "--layout") {
        flag = &layout;
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
    if ((path == nullptr) || (hash && (json || layout))) { return usage(); }
    return run_dump(path, hash, json, layout);
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
