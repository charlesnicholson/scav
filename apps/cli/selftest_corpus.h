#ifndef SCAV_APPS_CLI_SELFTEST_CORPUS_H_INCLUDED
#define SCAV_APPS_CLI_SELFTEST_CORPUS_H_INCLUDED

// The corpus charts and the committed hash golden, embedded by apps/cli/CMakeLists.txt.

#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace cli {

// One embedded document, keyed by the filename an include resolves to.
struct CorpusChart {
  std::string_view name;
  scav_byte const *bytes;
  uint32_t len;
};

std::vector<CorpusChart> corpus_charts();

// test_data/golden/layout/corpus_hashes.txt as committed.
std::string_view corpus_golden();

}  // namespace cli

#endif  // SCAV_APPS_CLI_SELFTEST_CORPUS_H_INCLUDED
