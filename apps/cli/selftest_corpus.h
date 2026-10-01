#ifndef SCAV_APPS_CLI_SELFTEST_CORPUS_H_INCLUDED
#define SCAV_APPS_CLI_SELFTEST_CORPUS_H_INCLUDED

// The corpus charts and the committed hash golden, embedded gzipped by
// apps/cli/CMakeLists.txt and inflated per call.

#include "scav/scav_types.h"

#include <string>
#include <string_view>
#include <vector>

namespace cli {

// One embedded document, keyed by the filename an include resolves to. `bytes`
// is empty when the embedded copy does not inflate.
struct CorpusChart {
  std::string_view name;
  std::vector<scav_byte> bytes;
};

std::vector<CorpusChart> corpus_charts();

// test_data/golden/layout/corpus_hashes.txt as committed; empty if it does not inflate.
std::string corpus_golden();

}  // namespace cli

#endif  // SCAV_APPS_CLI_SELFTEST_CORPUS_H_INCLUDED
