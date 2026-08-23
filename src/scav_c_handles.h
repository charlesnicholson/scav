#ifndef SCAV_C_HANDLES_H_INCLUDED
#define SCAV_C_HANDLES_H_INCLUDED

// The C handles, complete: every library's C surface operates on the same
// handles, so the definitions live at the src root. Nothing installs them.

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
  std::vector<scav::Diagnostic> diags;  // latest operation's; cleared at entry
};

#endif  // SCAV_C_HANDLES_H_INCLUDED
