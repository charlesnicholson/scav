#ifndef SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED
#define SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED

// The C structs carry no operators and tests compare them field-wise. Here
// rather than in `geom.h` so the library's own vocabulary stays without them,
// and here rather than in each test file so five copies cannot disagree.

#include "scav/scav_layout_c.h"

namespace scav {

constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

constexpr bool operator==(scav_point const &a, scav_point const &b) {
  return (a.x == b.x) && (a.y == b.y);
}

}  // namespace scav

#endif  // SCAV_LAYOUT_TESTS_POD_EQ_H_INCLUDED
