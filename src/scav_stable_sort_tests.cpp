// The vendored stable sort. std::sort and std::stable_sort are permitted in
// tests, which is exactly where they earn their keep: as the oracle.

#include "scav_stable_sort.h"

#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <utility>
#include <vector>

namespace {

using scav::scav_stable_sort;

// splitmix64's finalizer, position-addressed: reproducible from the seed alone.
uint64_t rnd(uint64_t seed, uint64_t index) {
  uint64_t x{ seed + (index * 0x9E37'79B9'7F4A'7C15ULL) };
  x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31U);
}

struct Row {
  uint32_t key;
  uint32_t seq;  // insertion order, which stability must preserve per key
};

bool by_key(Row a, Row b) { return a.key < b.key; }

}  // namespace

TEST_CASE("sort: sizes that stress the merge boundaries") {
  // 0 and 1 are the no-op path; 2 and 3 the smallest merges; the rest cover
  // odd tails, exact powers of two, and one size past each.
  for (uint32_t const n : { 0U, 1U, 2U, 3U, 7U, 8U, 9U, 64U, 65U, 1000U }) {
    std::vector<uint32_t> v;
    v.reserve(n);
    for (uint32_t i = 0; i < n; ++i) { v.push_back(static_cast<uint32_t>(rnd(1, i))); }
    std::vector<uint32_t> expect{ v };
    std::ranges::sort(expect);
    scav_stable_sort(v, [](uint32_t a, uint32_t b) { return a < b; });
    CHECK_MESSAGE(v == expect, "n = " << n);
  }
}

TEST_CASE("sort: already sorted, reversed, and all-equal inputs") {
  std::vector<uint32_t> sorted;
  std::vector<uint32_t> reversed;
  std::vector<uint32_t> equal;
  sorted.reserve(100);
  reversed.reserve(100);
  equal.reserve(100);
  for (uint32_t i = 0; i < 100; ++i) {
    sorted.push_back(i);
    reversed.push_back(99 - i);
    equal.push_back(7);
  }
  std::vector<uint32_t> const expect{ sorted };
  auto const less{ [](uint32_t a, uint32_t b) { return a < b; } };
  scav_stable_sort(sorted, less);
  scav_stable_sort(reversed, less);
  scav_stable_sort(equal, less);
  CHECK(sorted == expect);
  CHECK(reversed == expect);
  CHECK(equal == std::vector<uint32_t>(100, 7));
}

TEST_CASE("sort: equal keys keep insertion order") {
  // Few distinct keys and many rows, so every merge sees ties.
  std::vector<Row> rows;
  rows.reserve(512);
  for (uint32_t i = 0; i < 512; ++i) {
    rows.push_back({ static_cast<uint32_t>(rnd(2, i) % 5U), i });
  }
  std::vector<Row> oracle{ rows };
  std::ranges::stable_sort(oracle, by_key);
  scav_stable_sort(rows, by_key);
  REQUIRE(rows.size() == oracle.size());
  for (uint32_t i = 0; i < rows.size(); ++i) {
    CHECK(rows[i].key == oracle[i].key);
    CHECK(rows[i].seq == oracle[i].seq);
  }
  // Restated without the oracle: within one key, seq strictly climbs.
  for (uint32_t i = 1; i < rows.size(); ++i) {
    if (rows[i - 1].key == rows[i].key) { CHECK(rows[i - 1].seq < rows[i].seq); }
  }
}

TEST_CASE("sort: the comparator inlines as a functor, not a function pointer") {
  // Compile-shape test: a capturing lambda works, which qsort's shape forbids.
  uint32_t comparisons{ 0 };
  std::vector<uint32_t> v{ 3, 1, 2 };
  scav_stable_sort(v, [&comparisons](uint32_t a, uint32_t b) {
    ++comparisons;
    return a < b;
  });
  CHECK(v == std::vector<uint32_t>{ 1, 2, 3 });
  CHECK(comparisons > 0);
}
