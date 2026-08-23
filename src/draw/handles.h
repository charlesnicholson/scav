#ifndef SCAV_DRAW_HANDLES_H_INCLUDED
#define SCAV_DRAW_HANDLES_H_INCLUDED

// The three handles only libscavdraw's C surface operates on. Complete here and
// opaque everywhere else, so no C++ member can reach a caller's translation
// unit. `scav_chart` is shared and lives at the src root instead.

#include "scav/scav_draw.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

struct scav_metrics {
  scav::Metrics metrics;
};

struct scav_drawlist {
  scav::DrawList list;
};

// One pool behind all three spans, so a registry is two allocations however
// many images it holds and a registered id survives the vector growing.
struct scav_images {
  struct Row {
    scav::StrRef id, mime;
    scav::Span bytes;
    int32_t w, h;
  };
  std::vector<Row> rows;
  std::vector<scav_byte> pool;
};

#endif  // SCAV_DRAW_HANDLES_H_INCLUDED
