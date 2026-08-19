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
  "  dump [--hash|--json] <file>  the model: entity rows, not syntax\n"
};

int usage() {
  write_stream(std::string{ USAGE }, stderr);
  return EXIT_UNUSABLE;
}

// One optional flag from a fixed set, then exactly one path. Returns false when
// the tail is not shaped that way.
bool one_flagged_path(int argc,
                      char **argv,
                      std::string_view flag_a,
                      std::string_view flag_b,
                      bool &a,
                      bool &b,
                      char const *&path) {
  a = false;
  b = false;
  path = nullptr;
  for (int i = 2; i < argc; ++i) {
    std::string_view const arg{ argv[i] };
    if (!flag_a.empty() && (arg == flag_a)) {
      if (a) { return false; }
      a = true;
    } else if (!flag_b.empty() && (arg == flag_b)) {
      if (b) { return false; }
      b = true;
    } else if ((path == nullptr) && !arg.starts_with("-")) {
      path = argv[i];
    } else {
      return false;
    }
  }
  return (path != nullptr) && !(a && b);
}

int dispatch(int argc, char **argv) {
  std::string_view const verb{ argv[1] };
  bool flag_a{ false };
  bool flag_b{ false };
  char const *path{ nullptr };

  if (verb == "dump") {
    if (!one_flagged_path(argc, argv, "--hash", "--json", flag_a, flag_b, path)) {
      return usage();
    }
    return run_dump(path, flag_a, flag_b);
  }

  if (verb == "check") {
    if (!one_flagged_path(argc, argv, "", "", flag_a, flag_b, path)) { return usage(); }
    Network net;
    load_network(path, true, net);
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
