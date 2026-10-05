#ifndef SCAV_DRAW_HANDLES_H_INCLUDED
#define SCAV_DRAW_HANDLES_H_INCLUDED

// The three handles only libscavdraw's C surface operates on: complete here, opaque
// elsewhere. `scav_chart` is shared and lives at the src root.

#include "scav/scav_draw.h"
#include "scav/scav_types.h"

struct scav_metrics {
  scav::Metrics metrics;
};

struct scav_drawlist {
  scav::DrawList list;
};

struct scav_images {
  scav::Images images;
};

#endif  // SCAV_DRAW_HANDLES_H_INCLUDED
