#ifndef SCAV_CORE_TESTS_CORPUS_H_INCLUDED
#define SCAV_CORE_TESTS_CORPUS_H_INCLUDED

// Header-only and test-only.

#include <cstdlib>
#include <string>
#include <string_view>

namespace scav::test {

// Whether SCAV_TEST_CORPUS=light: the corpus is brew, dock, estop and led.
inline bool corpus_light() {
  char const *const mode{ std::getenv("SCAV_TEST_CORPUS") };
  return (mode != nullptr) && (std::string_view{ mode } == "light");
}

// Whether the light corpus drops this chart; a gauntlet chart is never dropped.
inline bool corpus_skipped(std::string_view chart) {
  bool const light{ corpus_light() };
  bool const corpus{ chart.ends_with(".scav") &&
                     (chart.find('/') == std::string_view::npos) };
  return light && corpus && (chart != "brew.scav") && (chart != "dock.scav") &&
         (chart != "estop.scav") && (chart != "led.scav");
}

// `golden` less every line naming a skipped chart as one of its words.
inline std::string corpus_golden(std::string_view golden) {
  std::string out;
  while (!golden.empty()) {
    size_t const eol{ golden.find('\n') };
    std::string_view const line{
      golden.substr(0, (eol == std::string_view::npos) ? golden.size() : eol + 1)
    };
    golden.remove_prefix(line.size());
    bool skipped{ false };
    for (std::string_view rest{ line }; !rest.empty() && !skipped;) {
      size_t const end{ rest.find_first_of(" \n") };
      skipped = corpus_skipped(rest.substr(0, end));
      rest.remove_prefix((end == std::string_view::npos) ? rest.size() : end + 1);
    }
    if (!skipped) { out += line; }
  }
  return out;
}

}  // namespace scav::test

#endif
