#ifndef SCAV_LAYOUT_PACK_H_INCLUDED
#define SCAV_LAYOUT_PACK_H_INCLUDED

// Order-preserving rectangle packing, for the sibling submachines of one
// state. Internal POD in, positions out; no chart and no profile.

#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

// Where a sum over a row or a column stops rather than wrapping, the domain
// bounding one rect at a time and not their total. Far enough past COORD_MAX
// that every caller's rejection fires and that no packing inside the domain
// ever reaches it, so `pack_better` still measures both candidates as they
// are; inside int32 so its products below stay inside int64.
inline constexpr int32_t PACK_SATURATED{ INT32_MAX };

// All four fields come back filled, `x` and `y` relative to the packing's own
// origin: whitespace elimination grows a rect past the extents it arrived
// with, so `w` and `h` are the step's answer and not an echo of the input.
// Every field saturates at PACK_SATURATED. `w` and `h` on the packing itself
// are the extents before that growth, which cannot change them.
struct Packing {
  std::vector<scav_rect> at;
  int32_t w{ 0 }, h{ 0 };
};

// Whether the placement is compacted before it is filled: `On` re-offers each
// rect the three positions it did not take and keeps a move only where the
// packing shrinks on one axis and grows on neither. A chart-global phase-2
// knob, because it is a smaller drawing at the price of a reflowed one, and
// only the whole chart's `Cost` can say which the reader wanted (11.10).
enum class Compaction : uint32_t { Off, On };

// Greedy width approximation from the desired aspect ratio, placement
// restricted to four positions relative to the predecessor, `compaction`, then
// whitespace elimination, which grows every rect to fill its subrow, block and
// row. Reading order survives: rect i is never left of and above rect j for
// i > j, whichever steps ran.
Packing pack_lr(std::vector<scav_rect> const &rects,
                int32_t sep,
                int32_t dar_num,
                int32_t dar_den,
                Compaction compaction);

// One row, every rect side by side and grown to the tallest. The packer that
// wins whenever the rects are all the same height, which is what the profile's
// `trybox` catches.
Packing pack_box(std::vector<scav_rect> const &rects, int32_t sep);

// `a` beats `b` under the scale measure `SM = min(DAR/w, 1/h)`, held as a
// rational and compared by cross-multiplication rather than computed. Ties go
// to the smaller area then the smaller aspect deviation, reversed when
// `aspect_first`.
bool pack_better(Packing const &a,
                 Packing const &b,
                 int32_t dar_num,
                 int32_t dar_den,
                 bool aspect_first);

}  // namespace scav

#endif  // SCAV_LAYOUT_PACK_H_INCLUDED
