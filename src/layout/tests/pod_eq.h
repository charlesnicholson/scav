#ifndef SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED
#define SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED

// Field-wise `operator==` for the C structs, for tests.

#include "scav/scav_layout_c.h"

#include <cstdint>

namespace scav {

constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

constexpr bool operator==(scav_point const &a, scav_point const &b) {
  return (a.x == b.x) && (a.y == b.y);
}

constexpr bool operator==(scav_span const &a, scav_span const &b) {
  return (a.off == b.off) && (a.len == b.len);
}

constexpr bool operator==(scav_port_slot const &a, scav_port_slot const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.side == b.side) &&
         (a.boundary_depth == b.boundary_depth);
}

// Element-wise equality of two vectors of C structs, by the operators above.
template <typename V>
bool same_rows(V const &a, V const &b) {
  if (a.size() != b.size()) { return false; }
  for (uint32_t i = 0; i < a.size(); ++i) {
    if (!(a[i] == b[i])) { return false; }
  }
  return true;
}

}  // namespace scav

#endif  // SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED
