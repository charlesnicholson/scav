// Doctest's implementation and `main`, linked into every test executable.

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstdlib>
#include <string_view>

int main(int argc, char **argv) {
  doctest::Context context;
  char const *const set{ std::getenv("SCAV_TEST_TIER") };
  std::string_view const tier{ (set != nullptr) ? set : "fast" };
  bool const exhaustive{ tier == "exhaustive" };
  if (!exhaustive && (tier != "full")) { context.addFilter("test-suite-exclude", "full"); }
  if (!exhaustive) { context.addFilter("test-suite-exclude", "exhaustive"); }
  context.applyCommandLine(argc, argv);
  return context.run();
}
