#ifndef SCAV_LAYOUT_PACK_H_INCLUDED
#define SCAV_LAYOUT_PACK_H_INCLUDED

// Order-preserving rectangle packing of one state's sibling submachines.

#include "scav/scav_types.h"
#include "scav_pod_vector.h"

#include <cstdint>

namespace scav {

// Row and column sums saturate here: above COORD_MAX and within int32.
inline constexpr int32_t PACK_SATURATED{ INT32_MAX };

// Every field saturates at PACK_SATURATED.
struct Packing {
  PodVector<scav_rect> at;  // origin-relative; sizes after whitespace elimination
  int32_t w{ 0 }, h{ 0 };   // the packing's extents
};

// `On`: before whitespace elimination, each rect tries its three other positions, keeping
// a move that shrinks one extent and grows neither. A chart-global phase-2 knob.
enum class Compaction : uint32_t { Off, On };

// Target width from the aspect ratio, four-position placement, `compaction`, whitespace
// elimination. Rect i is never left of and above rect j < i.
void pack_lr(Packing &out,
             PodVector<scav_rect> const &rects,
             int32_t sep,
             int32_t dar_num,
             int32_t dar_den,
             Compaction compaction);

// One row: every rect side by side, grown to the tallest.
void pack_box(Packing &out, PodVector<scav_rect> const &rects, int32_t sep);

// True when `a` scores higher on `SM = min(DAR/w, 1/h)`, compared as exact rationals.
// Ties go to smaller area then smaller aspect deviation, reversed when `aspect_first`.
bool pack_better(Packing const &a,
                 Packing const &b,
                 int32_t dar_num,
                 int32_t dar_den,
                 bool aspect_first);

}  // namespace scav

#endif  // SCAV_LAYOUT_PACK_H_INCLUDED
