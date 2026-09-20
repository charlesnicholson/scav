#ifndef SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED
#define SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED

// The C structs carry no operators and tests compare them field-wise. Here
// rather than in `geom.h` so the library's own vocabulary stays without them,
// and here rather than in each test file so five copies cannot disagree.

#include "scav/scav_layout_c.h"

#include <cstdint>
#include <vector>

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

// Element-wise, because the operators above are `scav`'s and the structs are
// the C ABI's: argument-dependent lookup from inside `std` looks in the global
// namespace and does not find them, so `vector == vector` will not compile.
template <typename T>
bool same_rows(std::vector<T> const &a, std::vector<T> const &b) {
  if (a.size() != b.size()) { return false; }
  for (uint32_t i = 0; i < a.size(); ++i) {
    if (!(a[i] == b[i])) { return false; }
  }
  return true;
}

}  // namespace scav

#endif  // SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED
