#ifndef SCAV_CORE_MODEL_SORT_H_INCLUDED
#define SCAV_CORE_MODEL_SORT_H_INCLUDED

// PRD 6: any sort whose result reaches output is scav's own, because std::sort
// is unstable and its tie-breaking differs between standard libraries. This is
// the vendored stable merge sort -- bottom-up, so it needs no recursion and no
// stack of its own beyond one scratch buffer.
//
// PRD 4's preferred idiom: parameterized on a functor rather than on a function
// pointer, so the comparison inlines. qsort's shape pays an indirect call per
// comparison and blocks inlining entirely.

#include <cstdint>
#include <utility>
#include <vector>

namespace scav {

// `less(a, b)` must be a strict weak ordering. Equal elements keep their input
// order, which is what makes a comparator that ends in an input-derived key a
// total order in practice.
template <typename T, typename Less>
void stable_sort_by(std::vector<T> &v, Less less) {
  uint32_t const n{ static_cast<uint32_t>(v.size()) };
  if (n < 2) { return; }

  std::vector<T> scratch(v.size());
  T *src{ v.data() };
  T *dst{ scratch.data() };

  for (uint32_t width = 1; width < n; width *= 2) {
    for (uint32_t lo = 0; lo < n; lo += 2 * width) {
      uint32_t const mid{ (lo + width < n) ? (lo + width) : n };
      uint32_t const hi{ (lo + (2 * width) < n) ? (lo + (2 * width)) : n };
      uint32_t a{ lo };
      uint32_t b{ mid };
      for (uint32_t out = lo; out < hi; ++out) {
        // `less(src[b], src[a])` rather than `!less(src[a], src[b])`: taking the
        // left run whenever the two compare equal is what makes this stable.
        bool const take_right{ (a >= mid) || ((b < hi) && less(src[b], src[a])) };
        dst[out] = std::move(take_right ? src[b++] : src[a++]);
      }
    }
    std::swap(src, dst);
  }

  // An odd number of passes leaves the result in the scratch buffer.
  if (src != v.data()) {
    for (uint32_t i = 0; i < n; ++i) { v[i] = std::move(src[i]); }
  }
}

}  // namespace scav

#endif  // SCAV_CORE_MODEL_SORT_H_INCLUDED
