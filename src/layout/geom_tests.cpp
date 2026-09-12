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
  CHECK((intersection(rect(0, 0, 100, 100), rect(50, 50, 100, 100)) == rect(50, 50, 50, 50)));
  // Nested intersects to the inner one, which is how the innermost enclosing
  // state is found without ordering by depth (11.9.3).
  CHECK((intersection(rect(0, 0, 100, 100), rect(10, 10, 20, 20)) == rect(10, 10, 20, 20)));
  CHECK((intersection(rect(10, 10, 20, 20), rect(0, 0, 100, 100)) == rect(10, 10, 20, 20)));
  // Disjoint on both axes, on one, and merely touching: never a negative extent.
  CHECK((intersection(rect(0, 0, 10, 10), rect(50, 50, 10, 10)) == rect(50, 50, 0, 0)));
  CHECK((intersection(rect(0, 0, 10, 100), rect(50, 0, 10, 100)) == rect(50, 0, 0, 100)));
  CHECK((intersection(rect(0, 0, 10, 10), rect(10, 0, 10, 10)) == rect(10, 0, 0, 10)));
  // An empty rect contains nothing and is contained by anything holding its
  // corner, so the two predicates agree on the degenerate case.
  CHECK(contains(rect(0, 0, 100, 100), intersection(rect(0, 0, 10, 10), rect(50, 50, 10, 10))));
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
