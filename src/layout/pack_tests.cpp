// Packing against hand-written rect lists: the target width worked by hand,
// each of the four positions reached deliberately, then the properties every
// packing has to keep whatever the shape.

#include "layout/pack.h"

#include "doctest.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace scav;

std::vector<scav_rect> boxes(std::vector<std::pair<int32_t, int32_t>> const &wh) {
  std::vector<scav_rect> out;
  out.reserve(wh.size());
  for (std::pair<int32_t, int32_t> const &e : wh) {
    out.push_back({ .x = 0, .y = 0, .w = e.first, .h = e.second });
  }
  return out;
}

bool same(std::vector<scav_rect> const &a, std::vector<scav_rect> const &b) {
  if (a.size() != b.size()) { return false; }
  for (uint32_t i = 0; i < a.size(); ++i) {
    if ((a[i].x != b[i].x) || (a[i].y != b[i].y) || (a[i].w != b[i].w) ||
        (a[i].h != b[i].h)) {
      return false;
    }
  }
  return true;
}

bool overlaps(scav_rect const &a, scav_rect const &b) {
  return (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) && (a.y < (b.y + b.h)) &&
         (b.y < (a.y + a.h));
}

// No two rects share a point, and no later rect is both above and left of an
// earlier one, which is what "order preserving" has to mean for a reader.
void check_sane(Packing const &p) {
  for (uint32_t i = 0; i < p.at.size(); ++i) {
    CHECK((p.at[i].x + p.at[i].w) <= p.w);
    CHECK((p.at[i].y + p.at[i].h) <= p.h);
    for (uint32_t j = i + 1; j < p.at.size(); ++j) {
      CHECK_FALSE(overlaps(p.at[i], p.at[j]));
      bool const behind{ (p.at[j].x < p.at[i].x) && (p.at[j].y <= p.at[i].y) };
      CHECK_FALSE(behind);
    }
  }
}

}  // namespace

TEST_CASE("pack: nothing to pack") {
  Packing const p{ pack_lr({}, 10, 16, 10) };
  CHECK(p.at.empty());
  CHECK(p.w == 0);
  CHECK(p.h == 0);
  Packing const b{ pack_box({}, 10) };
  CHECK(b.at.empty());
  CHECK(b.w == 0);
}

TEST_CASE("pack: one rect sits at the origin and is the whole extent") {
  Packing const p{ pack_lr(boxes({ { 300, 120 } }), 10, 16, 10) };
  REQUIRE(p.at.size() == 1);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.w == 300);
  CHECK(p.h == 120);
}

TEST_CASE("pack: four squares come out square, gaps paid for") {
  // Inflated area is 4 * 110 * 60 = 26400 at DAR 1:1, so the target is
  // isqrt(26400) = 162 and two 100-wide rects fit in a row with 10 between.
  Packing const p{
    pack_lr(boxes({ { 100, 100 }, { 100, 100 }, { 100, 100 }, { 100, 100 } }), 10, 1, 1)
  };
  REQUIRE(p.at.size() == 4);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 110);
  CHECK(p.at[1].y == 0);
  CHECK(p.at[2].x == 0);
  CHECK(p.at[2].y == 110);
  CHECK(p.at[3].x == 110);
  CHECK(p.at[3].y == 110);
  CHECK(p.w == 210);
  CHECK(p.h == 210);
  check_sane(p);
}

TEST_CASE("pack: a bare-area target would have stacked those four in a column") {
  // The same input with no separation: the target is the same 200-ish, and
  // the gaps are what the inflation above pays for.
  Packing const p{
    pack_lr(boxes({ { 100, 100 }, { 100, 100 }, { 100, 100 }, { 100, 100 } }), 0, 1, 1)
  };
  CHECK(p.w == 200);
  CHECK(p.h == 200);
}

TEST_CASE("pack: a rect after a wrap goes back to the row's top level") {
  // Inflated area 6600 + 6600 + 3600 = 16800, times 16 over 10 is 26880, so
  // the target is isqrt(26880) = 163. The second rect cannot follow the first
  // (210 > 163) so it wraps to a subrow; the third fits at row level
  // (100 + 10 + 50 = 160 <= 163) and goes there rather than beside the second,
  // which would have left an unfillable notch above it.
  Packing const p{ pack_lr(boxes({ { 100, 50 }, { 100, 50 }, { 50, 50 } }), 10, 16, 10) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 0);
  CHECK(p.at[1].y == 60);
  CHECK(p.at[2].x == 110);
  CHECK(p.at[2].y == 0);
  CHECK(p.w == 160);
  CHECK(p.h == 110);
  check_sane(p);
}

TEST_CASE("pack: a rect wider than the target opens a new row") {
  // Nothing can share a row with the 900-wide rect, so the target is its own
  // width and each of the others takes a row.
  Packing const p{ pack_lr(boxes({ { 900, 40 }, { 900, 40 }, { 900, 40 } }), 10, 16, 10) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.w == 900);
  for (uint32_t i = 0; i < 3; ++i) { CHECK(p.at[i].x == 0); }
  CHECK(p.at[0].y < p.at[1].y);
  CHECK(p.at[1].y < p.at[2].y);
  check_sane(p);
}

TEST_CASE("pack: the box packer is one row whatever the extents") {
  Packing const p{ pack_box(boxes({ { 40, 10 }, { 900, 300 }, { 5, 5 } }), 7) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[1].x == 47);
  CHECK(p.at[2].x == 954);
  for (scav_rect const &r : p.at) { CHECK(r.y == 0); }
  CHECK(p.w == 959);
  CHECK(p.h == 300);
  check_sane(p);
}

TEST_CASE("pack: the scale measure prefers the packing that scales larger") {
  Packing const wide{ .at = {}, .w = 160, .h = 100 };
  Packing const tall{ .at = {}, .w = 100, .h = 160 };
  // At DAR 16:10 the wide one fits a 16:10 viewport at a larger scale.
  CHECK(pack_better(wide, tall, 16, 10, false));
  CHECK_FALSE(pack_better(tall, wide, 16, 10, false));
  // At 10:16 the preference reverses, without either extent changing.
  CHECK(pack_better(tall, wide, 10, 16, false));
}

TEST_CASE("pack: the scale measure already prefers the smaller of two similar shapes") {
  Packing const small{ .at = {}, .w = 160, .h = 100 };
  Packing const large{ .at = {}, .w = 320, .h = 200 };
  CHECK(pack_better(small, large, 16, 10, false));
}

TEST_CASE("pack: equal scale falls to the tiebreak the profile picked") {
  // Both are height-bound at the same height, so both scale to 1/100 and the
  // measure cannot separate them. Only the tiebreak can, and it points the
  // two ways round: 160x100 is exactly 16:10 while 100x100 covers less area.
  Packing const matching{ .at = {}, .w = 160, .h = 100 };
  Packing const smaller{ .at = {}, .w = 100, .h = 100 };
  CHECK(pack_better(smaller, matching, 16, 10, false));
  CHECK_FALSE(pack_better(matching, smaller, 16, 10, false));
  CHECK(pack_better(matching, smaller, 16, 10, true));
  CHECK_FALSE(pack_better(smaller, matching, 16, 10, true));
}

TEST_CASE("pack: sane on a spread of shapes, and twice the same") {
  std::vector<std::vector<std::pair<int32_t, int32_t>>> const cases{
    { { 7, 7 } },
    { { 41, 13 }, { 13, 41 } },
    { { 200, 30 }, { 30, 200 }, { 60, 60 }, { 5, 5 }, { 90, 12 } },
    { { 33, 33 }, { 33, 33 }, { 33, 33 }, { 33, 33 }, { 33, 33 }, { 33, 33 }, { 33, 33 } },
    { { 1, 1 }, { 400, 1 }, { 1, 400 }, { 200, 200 } },
  };
  for (std::vector<std::pair<int32_t, int32_t>> const &wh : cases) {
    Packing const p{ pack_lr(boxes(wh), 13, 16, 10) };
    REQUIRE(p.at.size() == wh.size());
    check_sane(p);
    CHECK(same(p.at, pack_lr(boxes(wh), 13, 16, 10).at));
    check_sane(pack_box(boxes(wh), 13));
  }
}

TEST_CASE("pack: a column of maximal rects saturates rather than wrapping") {
  // Five thousand rects the domain admits one at a time. Only one fits a row,
  // so the column runs to 2.6 billion: an int32 cursor would have wrapped
  // through it four thousand rects in.
  std::vector<scav_rect> const tall(5000,
                                    { .x = 0, .y = 0, .w = COORD_MAX, .h = COORD_MAX });

  Packing const p{ pack_lr(tall, 0, 16, 10) };
  REQUIRE(p.at.size() == tall.size());
  CHECK(p.w == COORD_MAX);
  CHECK(p.h == PACK_SATURATED);
  CHECK(p.h > COORD_MAX);  // the only thing a caller tests
  bool ordered{ true };
  for (uint32_t i = 1; i < p.at.size(); ++i) {
    ordered = ordered && (p.at[i].y >= p.at[i - 1].y) && (p.at[i].x >= 0);
  }
  CHECK(ordered);

  // The row packer sums the other axis, and stops in the same place.
  Packing const row{ pack_box(tall, 0) };
  CHECK(row.w == PACK_SATURATED);
  CHECK(row.h == COORD_MAX);
}
