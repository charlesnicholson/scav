// Doctest's implementation and `main`, linked into every test executable.

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstdlib>
#include <string_view>

int main(int argc, char **argv) {
  doctest::Context context;
  char const *const tier{ std::getenv("SCAV_TEST_TIER") };
  if ((tier == nullptr) || (std::string_view{ tier } != "full")) {
    context.addFilter("test-suite-exclude",
                      "full");  // the fast tier, unless asked for full
  }
  context.applyCommandLine(argc, argv);
  return context.run();
}
