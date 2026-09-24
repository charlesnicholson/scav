// The rect predicates three phases share, where each of them last went wrong.

#include "layout/geom.h"
#include "layout/partition.h"
#include "layout/tests/pod_eq.h"

#include "doctest.h"

#include <cstdint>

namespace scav {
namespace {

constexpr scav_rect rect(int32_t x, int32_t y, int32_t w, int32_t h) {
  return { .x = x, .y = y, .w = w, .h = h };
}

}  // namespace

TEST_CASE("geom: containment is not strict and overlap is") {
  scav_rect const outer{ rect(0, 0, 100, 100) };
  CHECK(contains(outer, outer));  // flush, and still contained
  CHECK(contains(outer, rect(0, 0, 100, 0)));
  CHECK(contains(outer, rect(100, 100, 0, 0)));
  CHECK_FALSE(contains(outer, rect(-1, 0, 100, 100)));
  CHECK_FALSE(contains(outer, rect(0, 0, 101, 100)));
  // The pair `label.cpp` and `nudge.cpp` each had a copy of: touching is
  // contained and is not overlapping.
  CHECK_FALSE(overlaps(outer, rect(100, 0, 100, 100)));
  CHECK(overlaps(outer, rect(99, 0, 100, 100)));
}

TEST_CASE("geom: an intersection is empty rather than negative") {
  CHECK((intersection(rect(0, 0, 100, 100), rect(50, 50, 100, 100)) ==
         rect(50, 50, 50, 50)));
  // Nested intersects to the inner one, which is how the innermost enclosing
  // state is found without ordering by depth (11.9.3).
  CHECK(
      (intersection(rect(0, 0, 100, 100), rect(10, 10, 20, 20)) == rect(10, 10, 20, 20)));
  CHECK(
      (intersection(rect(10, 10, 20, 20), rect(0, 0, 100, 100)) == rect(10, 10, 20, 20)));
  // Disjoint on both axes, on one, and merely touching: never a negative extent.
  CHECK((intersection(rect(0, 0, 10, 10), rect(50, 50, 10, 10)) == rect(50, 50, 0, 0)));
  CHECK((intersection(rect(0, 0, 10, 100), rect(50, 0, 10, 100)) == rect(50, 0, 0, 100)));
  CHECK((intersection(rect(0, 0, 10, 10), rect(10, 0, 10, 10)) == rect(10, 0, 0, 10)));
  // An empty rect contains nothing and is contained by anything holding its
  // corner, so the two predicates agree on the degenerate case.
  CHECK(contains(rect(0, 0, 100, 100),
                 intersection(rect(0, 0, 10, 10), rect(50, 50, 10, 10))));
}

TEST_CASE("geom: a bumper grows both sides and a negative one shrinks") {
  CHECK((grow(rect(10, 20, 30, 40), 5) == rect(5, 15, 40, 50)));
  CHECK((grow(rect(10, 20, 30, 40), 0) == rect(10, 20, 30, 40)));
  CHECK((grow(rect(10, 20, 30, 40), -5) == rect(15, 25, 20, 30)));
  // What the router and nudging both want it for: clearance becomes containment.
  CHECK(overlaps(grow(rect(0, 0, 10, 10), 3), rect(12, 0, 10, 10)));
  CHECK_FALSE(overlaps(grow(rect(0, 0, 10, 10), 3), rect(13, 0, 10, 10)));
}

TEST_CASE("partition: a set's root is its least member") {
  // Three callers read this rather than the union order: a lane's root is its
  // lowest coordinate, a bundle's is its first member, and a frame's components
  // number densely in first-member order.
  Partition p;
  p.reset(6);
  for (uint32_t i = 0; i < 6; ++i) { CHECK(p.leads(i)); }

  CHECK(p.join(4, 2));
  CHECK(p.root(4) == 2);
  CHECK(p.join(2, 5));
  CHECK(p.root(5) == 2);
  // Joined the other way round, and the least still leads.
  CHECK(p.join(3, 1));
  CHECK(p.root(3) == 1);
  CHECK(p.join(5, 3));
  for (uint32_t const i : { 1U, 2U, 3U, 4U, 5U }) { CHECK(p.root(i) == 1); }
  CHECK(p.leads(0));
  CHECK(p.leads(1));

  CHECK_FALSE(p.join(4, 5));  // already one set
  CHECK_FALSE(p.join(3, 3));
}

TEST_CASE("partition: reset clears whatever it was") {
  Partition p;
  p.reset(4);
  p.join(0, 3);
  p.reset(2);
  CHECK(p.of.size() == 2);
  CHECK(p.leads(0));
  CHECK(p.leads(1));
}

}  // namespace scav

namespace scav {

namespace {

bool scan_hits(std::vector<scav_rect> const &rects, scav_rect const &cand) {
  for (scav_rect const &r : rects) {
    if (overlaps(cand, r)) { return true; }
  }
  return false;
}

// A small deterministic generator: the property is over many shapes, and a
// test that reads different numbers each run finds nothing twice.
uint32_t next(uint64_t &state) {
  state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
  return static_cast<uint32_t>(state >> 33U);
}

}  // namespace

TEST_CASE("geom: a rect grid answers exactly what scanning every rect does") {
  // 11.10f's label speed-up rests on this: the grid may only visit fewer rects,
  // never answer differently. Zero-width and zero-height rects are route
  // pieces, which `overlaps` counts only strictly inside the other's span.
  uint64_t seed{ 1 };
  scav_rect const region{ .x = -3000, .y = -2000, .w = 12000, .h = 7000 };
  for (uint32_t trial = 0; trial < 200; ++trial) {
    std::vector<scav_rect> rects;
    uint32_t const n{ next(seed) % 60 };
    for (uint32_t i = 0; i < n; ++i) {
      int32_t const x{ region.x + static_cast<int32_t>(next(seed) % 12000) };
      int32_t const y{ region.y + static_cast<int32_t>(next(seed) % 7000) };
      uint32_t const shape{ next(seed) % 4 };
      int32_t const w{ (shape == 0) ? 0 : static_cast<int32_t>(next(seed) % 2500) };
      int32_t const h{ (shape == 1) ? 0 : static_cast<int32_t>(next(seed) % 1500) };
      rects.push_back({ .x = x, .y = y, .w = w, .h = h });
    }
    RectGrid g;
    int32_t const cw{ 1 + static_cast<int32_t>(next(seed) % 900) };
    int32_t const ch{ 1 + static_cast<int32_t>(next(seed) % 300) };
    grid_build(g, region, rects, cw, ch);
    for (uint32_t q = 0; q < 200; ++q) {
      // Some candidates reach past the region, which the grid clamps into its
      // edge cells; the answer must not change there either.
      scav_rect const cand{ .x = region.x - 800 + static_cast<int32_t>(next(seed) % 13600),
                            .y = region.y - 800 + static_cast<int32_t>(next(seed) % 8600),
                            .w = static_cast<int32_t>(next(seed) % 1200),
                            .h = static_cast<int32_t>(next(seed) % 400) };
      CAPTURE(trial);
      CAPTURE(q);
      CHECK(grid_hits(g, rects, cand) == scan_hits(rects, cand));
    }
  }
}

TEST_CASE("geom: a rect grid caps its cells, however small it is asked to make them") {
  RectGrid g;
  scav_rect const region{ .x = 0, .y = 0, .w = 1'000'000, .h = 1'000'000 };
  grid_build(g, region, { { .x = 10, .y = 10, .w = 5, .h = 5 } }, 1, 1);
  CHECK(g.nx <= GRID_SIDE);
  CHECK(g.ny <= GRID_SIDE);
  CHECK(grid_hits(g,
                  { { .x = 10, .y = 10, .w = 5, .h = 5 } },
                  { .x = 12, .y = 12, .w = 1, .h = 1 }));
  // An empty set of rects hits nothing, and a degenerate region still builds.
  RectGrid none;
  grid_build(none, { .x = 5, .y = 5, .w = 0, .h = 0 }, {}, 10, 10);
  CHECK(!grid_hits(none, {}, { .x = 0, .y = 0, .w = 100, .h = 100 }));
}

}  // namespace scav
