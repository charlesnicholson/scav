#ifndef SCAV_CORE_C_API_INTERNAL_H_INCLUDED
#define SCAV_CORE_C_API_INTERNAL_H_INCLUDED

// The C handles, complete. A later library's C surface (layout, draw) operates
// on the same handles, so the definitions live where -Isrc reaches rather than
// inside one translation unit. Private by location: nothing outside src/ can
// reach this path, and nothing here is installed.

#include "scav/scav_core.h"
#include "scav/scav_core_c.h"

#include <cstdint>
#include <vector>

// Complete here and opaque everywhere else, so no C++ member can reach a
// caller's translation unit.
struct scav_load {
  scav::Loader loader;
  std::vector<scav::Diagnostic> diags;  // the loader's, plus finish's
  std::vector<scav_pending> pending;
  uint32_t finished;
};

struct scav_chart {
  scav::Chart chart;
  // Findings from the latest operation on this chart -- validation, layout.
  // Cleared at each operation's entry. The loader keeps its own: a failed
  // load leaves no chart to carry them.
  std::vector<scav::Diagnostic> diags;
};

#endif  // SCAV_CORE_C_API_INTERNAL_H_INCLUDED
