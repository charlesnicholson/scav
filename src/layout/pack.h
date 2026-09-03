#ifndef SCAV_LAYOUT_PACK_H_INCLUDED
#define SCAV_LAYOUT_PACK_H_INCLUDED

// Order-preserving rectangle packing, for the sibling submachines of one
// state. Internal POD in, positions out; no chart and no profile.

#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

// `w` and `h` are read from the input rects; `x` and `y` come back filled,
// relative to the packing's own origin.
struct Packing {
  std::vector<scav_rect> at;
  int32_t w{ 0 }, h{ 0 };
};

// Greedy width approximation from the desired aspect ratio, then placement
// restricted to four positions relative to the predecessor. Reading order
// survives: rect i is never left of and above rect j for i > j.
Packing pack_lr(std::vector<scav_rect> const &rects,
                int32_t sep,
                int32_t dar_num,
                int32_t dar_den);

// One row, every rect side by side. The packer that wins whenever the rects
// are all the same height, which is what the profile's `trybox` catches.
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
