#ifndef SCAV_STABLE_SORT_H_INCLUDED
#define SCAV_STABLE_SORT_H_INCLUDED

// scav's own stable sort, for any sort whose result reaches output. Bottom-up
// merge over one scratch buffer, templated so the comparator inlines.

#include <cstddef>
#include <vector>

namespace scav {

// Stable insertion sort over a raw range. The merge below allocates a scratch
// buffer per call, and a layout runs hundreds of thousands of sorts of two or
// three elements, where that allocation is the whole of the cost.
template <typename T, typename Less>
void scav_insertion_sort(T *first, T *last, Less less) {
  if (first == last) { return; }
  for (T *i = first + 1; i < last; ++i) {
    T const key{ *i };
    T *j{ i };
    // Move only past strictly greater keys, which is the stability guarantee.
    while ((j > first) && less(key, *(j - 1))) {
      *j = *(j - 1);
      --j;
    }
    *j = key;
  }
}

// Where a merge pass would cost more in scratch than it saves in comparisons.
inline constexpr size_t SCAV_SORT_SMALL{ 32 };

template <typename T, typename Less>
void scav_stable_sort(std::vector<T> &v, Less less) {
  size_t const n{ v.size() };
  if (n < 2) { return; }
  if (n <= SCAV_SORT_SMALL) {
    scav_insertion_sort(v.data(), v.data() + n, less);
    return;
  }

  std::vector<T> scratch(n);
  T *src{ v.data() };
  T *dst{ scratch.data() };

  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t const mid{ ((lo + width) < n) ? (lo + width) : n };
      size_t const hi{ ((lo + (2 * width)) < n) ? (lo + (2 * width)) : n };
      size_t a{ lo };
      size_t b{ mid };
      size_t o{ lo };
      while ((a < mid) && (b < hi)) {
        // Take from the right run only on strict less: equal keys keep the
        // left run's order, which is the stability guarantee.
        if (less(src[b], src[a])) {
          dst[o++] = src[b++];
        } else {
          dst[o++] = src[a++];
        }
      }
      while (a < mid) { dst[o++] = src[a++]; }
      while (b < hi) { dst[o++] = src[b++]; }
    }
    T *const swap{ src };
    src = dst;
    dst = swap;
  }

  // An odd pass count leaves the result in the scratch buffer.
  if (src != v.data()) {
    for (size_t i = 0; i < n; ++i) { v[i] = src[i]; }
  }
}

}  // namespace scav

#endif  // SCAV_STABLE_SORT_H_INCLUDED
