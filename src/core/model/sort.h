#ifndef SCAV_CORE_MODEL_SORT_H_INCLUDED
#define SCAV_CORE_MODEL_SORT_H_INCLUDED

// Any sort whose result reaches output is ours: std::sort is unstable and its
// tie-breaking differs between standard libraries. Bottom-up merge, so no
// recursion; functor rather than function pointer, so the comparison inlines.

#include <cstddef>
#include <utility>
#include <vector>

namespace scav {

// `less` must be a strict weak ordering. Equal elements keep their input order,
// which is what makes a comparator ending in an input-derived key a total one.
template <typename T, typename Less>
void stable_sort_by(std::vector<T> &v, Less less) {
  size_t const n{ v.size() };
  if (n < 2) { return; }

  std::vector<T> scratch(v.size());
  T *src{ v.data() };
  T *dst{ scratch.data() };

  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t const mid{ (lo + width < n) ? (lo + width) : n };
      size_t const hi{ (lo + (2 * width) < n) ? (lo + (2 * width)) : n };
      size_t a{ lo };
      size_t b{ mid };
      for (size_t out = lo; out < hi; ++out) {
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
    for (size_t i = 0; i < n; ++i) { v[i] = std::move(src[i]); }
  }
}

}  // namespace scav

#endif  // SCAV_CORE_MODEL_SORT_H_INCLUDED
