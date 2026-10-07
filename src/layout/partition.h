#ifndef SCAV_LAYOUT_PARTITION_H_INCLUDED
#define SCAV_LAYOUT_PARTITION_H_INCLUDED

// Disjoint sets over `[0, n)`.

#include "scav_int.h"
#include "scav_pod_vector.h"

#include <cstdint>

namespace scav {

// Union-find: `root` halves paths and `join` unions onto the lower index, so a set's root
// is its least member.
struct Partition {
  PodVector<uint32_t> of;

  void reset(size_t n) {
    of.resize(n);
    for (uint32_t i = 0; i < of.size(); ++i) { of[i] = i; }
  }

  uint32_t root(uint32_t x) {
    while (of[x] != x) {
      of[x] = of[of[x]];
      x = of[x];
    }
    return x;
  }

  bool leads(uint32_t x) { return root(x) == x; }

  // False when `a` and `b` were already in one set.
  bool join(uint32_t a, uint32_t b) {
    uint32_t const one{ root(a) };
    uint32_t const two{ root(b) };
    if (one == two) { return false; }
    of[imax(one, two)] = imin(one, two);
    return true;
  }
};

}  // namespace scav

#endif  // SCAV_LAYOUT_PARTITION_H_INCLUDED
