#ifndef SCAV_LAYOUT_PARTITION_H_INCLUDED
#define SCAV_LAYOUT_PARTITION_H_INCLUDED

// Disjoint sets over `[0, n)`, which three phases build and none of them needs
// a different one of.

#include "scav_int.h"

#include <cstdint>
#include <vector>

namespace scav {

// Path-halving find, unioned onto the lower index. **A set's root is its least
// member**, which is the property the callers read rather than an accident of
// the union order: a lane's root is its lowest coordinate because members are
// sorted by it, a bundle's is its first member, and a frame's components number
// densely in first-member order.
struct Partition {
  std::vector<uint32_t> of;

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

  // False where the two were already one set.
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
