// Packing against hand-written rect lists: the target width worked by hand,
// each of the four positions reached deliberately, then the properties every
// packing has to keep whatever the shape.

#include "layout/pack.h"

#include "scav_int.h"
#include "scav_rnd.h"

#include "doctest.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace scav {

// The packing with its last step switchable, which `pack.cpp` brackets with
// SCAV_INTERNAL so these tests can weigh it against the placement it started
// from.
Packing pack_rows(std::vector<scav_rect> const &rects,
                  int32_t sep,
                  int32_t dar_num,
                  int32_t dar_den,
                  bool expanded);

}  // namespace scav

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

// The C structs carry no operators; the tests compare them field-wise.
constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
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

// The clear distance between two rects on one axis, zero where they overlap
// on it. One of the two is at least `sep` in every packing.
int32_t apart(int32_t a_lo, int32_t a_len, int32_t b_lo, int32_t b_len) {
  return imax(imax(b_lo - (a_lo + a_len), a_lo - (b_lo + b_len)), 0);
}

// No two rects share a point or come closer than `sep`, and no later rect is
// both above and left of an earlier one, which is what "order preserving" has
// to mean for a reader.
void check_sane(Packing const &p, int32_t sep) {
  for (uint32_t i = 0; i < p.at.size(); ++i) {
    CHECK((p.at[i].x + p.at[i].w) <= p.w);
    CHECK((p.at[i].y + p.at[i].h) <= p.h);
    for (uint32_t j = i + 1; j < p.at.size(); ++j) {
      CHECK_FALSE(overlaps(p.at[i], p.at[j]));
      bool const behind{ (p.at[j].x < p.at[i].x) && (p.at[j].y <= p.at[i].y) };
      CHECK_FALSE(behind);
      int32_t const dx{ apart(p.at[i].x, p.at[i].w, p.at[j].x, p.at[j].w) };
      int32_t const dy{ apart(p.at[i].y, p.at[i].h, p.at[j].y, p.at[j].h) };
      CHECK(imax(dx, dy) >= sep);
    }
  }
}

int64_t filled(Packing const &p) {
  int64_t used{ 0 };
  for (scav_rect const &r : p.at) { used += int64_t{ r.w } * r.h; }
  return used;
}

// Whitespace elimination moves neither extent, which is what keeps it out of
// `pack_better`'s way: every rect grows or stays, none moves back or up, and a
// rect with no extent is left as it is.
void check_expansion(std::vector<scav_rect> const &rects,
                     int32_t sep,
                     int32_t dar_num,
                     int32_t dar_den) {
  Packing const placed{ pack_rows(rects, sep, dar_num, dar_den, false) };
  Packing const grown{ pack_lr(rects, sep, dar_num, dar_den) };
  check_sane(grown, sep);
  check_sane(placed, sep);
  CHECK(grown.w == placed.w);
  CHECK(grown.h == placed.h);
  REQUIRE(grown.at.size() == placed.at.size());
  for (uint32_t i = 0; i < grown.at.size(); ++i) {
    CAPTURE(i);
    CHECK(grown.at[i].x >= placed.at[i].x);
    CHECK(grown.at[i].y >= placed.at[i].y);
    CHECK(grown.at[i].w >= placed.at[i].w);
    CHECK(grown.at[i].h >= placed.at[i].h);
    if ((placed.at[i].w == 0) || (placed.at[i].h == 0)) {
      CHECK(grown.at[i].w == placed.at[i].w);
      CHECK(grown.at[i].h == placed.at[i].h);
    }
  }
  CHECK(filled(grown) >= filled(placed));
  // Inside the domain before the step stays inside it after, which is what
  // lets `pack_best` keep asking the same question of one candidate (11.2).
  if ((placed.w <= COORD_MAX) && (placed.h <= COORD_MAX)) {
    CHECK(grown.w <= COORD_MAX);
    CHECK(grown.h <= COORD_MAX);
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
  check_sane(p, 10);
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
  check_sane(p, 10);
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
  check_sane(p, 10);
}

TEST_CASE("pack: a rect that fits neither its subrow nor its block opens a row") {
  // Inflated area 3000 + 7200 + 2400 + 3000 = 15600, times 16 over 10 is 24960,
  // so the target is isqrt(24960) = 157. The third rect goes back to row level
  // and starts a block at x=120; the fourth fits neither beside it (160 + 40)
  // nor at that block's left edge (120 + 40), so it wraps to a new row.
  Packing const p{
    pack_lr(boxes({ { 40, 50 }, { 110, 50 }, { 30, 50 }, { 40, 50 } }), 10, 16, 10)
  };
  REQUIRE(p.at.size() == 4);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 0);
  CHECK(p.at[1].y == 60);
  CHECK(p.at[2].x == 120);
  CHECK(p.at[2].y == 0);
  // The new row clears the whole of the one above it, block and subrows alike.
  CHECK(p.at[3].x == 0);
  CHECK(p.at[3].y == 120);
  CHECK(p.w == 150);
  CHECK(p.h == 170);
  check_sane(p, 10);
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
  check_sane(p, 7);
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
    check_sane(p, 13);
    CHECK(same(p.at, pack_lr(boxes(wh), 13, 16, 10).at));
    check_sane(pack_box(boxes(wh), 13), 13);
    check_expansion(boxes(wh), 13, 16, 10);
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

TEST_CASE("pack: one oversized child sets the target and nothing shares its row") {
  // The widest rect floors the target, so the other two can only stack under it.
  std::vector<scav_rect> const in{ boxes({ { 1000, 100 }, { 100, 100 }, { 100, 100 } }) };
  Packing const p{ pack_rows(in, 10, 16, 10, false) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 0);
  CHECK(p.at[1].y == 110);
  CHECK(p.at[2].x == 110);
  CHECK(p.at[2].y == 110);
  CHECK(p.w == 1000);
  CHECK(p.h == 210);
  check_expansion(in, 10, 16, 10);
}

TEST_CASE("pack: equal heights are where the row packer wins") {
  // `crowd`'s two concurrent regions under real text: same height, and the
  // target holds only one of them, so `pack_lr` stacks them. The side-by-side
  // candidate is the row packer's, and `trybox` is what finds it.
  std::vector<scav_rect> const in{ boxes({ { 1888, 1594 }, { 1773, 1594 } }) };
  Packing const p{ pack_lr(in, 192, 16, 10) };
  CHECK(p.w == 1888);
  CHECK(p.h == 3380);
  Packing const row{ pack_box(in, 192) };
  CHECK(row.w == 3853);
  CHECK(row.h == 1594);
  CHECK(pack_better(row, p, 16, 10, false));
  check_expansion(in, 192, 16, 10);
}

TEST_CASE("pack: whitespace elimination keeps every property over a seeded spread") {
  // Random rect lists from the position-addressed generator, so the cases are
  // the same on every platform. Each is held to order, separation, no overlap,
  // the extents and the domain, before and after the step.
  for (uint32_t item = 0; item < 400; ++item) {
    CAPTURE(item);
    uint32_t const n{ 1 + static_cast<uint32_t>(rnd(9091, 0, item, 0) % 9) };
    int32_t const sep{ static_cast<int32_t>(rnd(9091, 1, item, 0) % 200) };
    std::vector<scav_rect> in;
    in.reserve(n);
    for (uint32_t k = 0; k < n; ++k) {
      in.push_back({ .x = 0,
                     .y = 0,
                     .w = static_cast<int32_t>(rnd(9091, 2, item, k) % 4000),
                     .h = static_cast<int32_t>(rnd(9091, 3, item, k) % 4000) });
    }
    check_expansion(in, sep, 16, 10);
    // A second run of the same input is the same bytes, the step included.
    CHECK(same(pack_lr(in, sep, 16, 10).at, pack_lr(in, sep, 16, 10).at));
  }
}

TEST_CASE("pack: a saturated column is left as it is") {
  // The column the domain admits one rect at a time. A packing past the domain
  // is no candidate, so nothing in it is worth filling and the step declines.
  std::vector<scav_rect> const tall(64,
                                    { .x = 0, .y = 0, .w = COORD_MAX, .h = COORD_MAX });
  Packing const placed{ pack_rows(tall, 0, 16, 10, false) };
  Packing const p{ pack_lr(tall, 0, 16, 10) };
  CHECK(p.w == COORD_MAX);
  CHECK(p.h == (64 * COORD_MAX));
  CHECK(same(p.at, placed.at));
}

TEST_CASE("pack: whitespace elimination fills a column to the drawing's width") {
  // The commonest shape phase 2 packs: a column of rows one rect wide, the
  // widest setting the drawing. Every narrower rect grows to that width, so
  // the only space left inside the box is the gaps between the rows.
  std::vector<scav_rect> const in{ boxes(
      { { 2695, 653 }, { 1434, 653 }, { 1895, 653 } }) };
  Packing const p{ pack_lr(in, 288, 16, 10) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.w == 2695);
  CHECK(p.h == 2535);
  for (uint32_t i = 0; i < 3; ++i) {
    CAPTURE(i);
    CHECK(p.at[i].x == 0);
    CHECK(p.at[i].w == 2695);
    CHECK(p.at[i].h == 653);
  }
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].y == 941);
  CHECK(p.at[2].y == 1882);
  // The box less the two gaps, exactly: no whitespace of any other kind left.
  CHECK(filled(p) == (int64_t{ p.w } * (p.h - (2 * 288))));
  check_expansion(in, 288, 16, 10);
}

TEST_CASE("pack: whitespace elimination fills the notch a row-level rect left") {
  // The case the placement comment names: the third rect takes row level and
  // leaves a notch under it that nothing short of growing a neighbour fills.
  // This is that growth -- 50 x 50 becomes 50 x 110, down to the row's floor.
  std::vector<scav_rect> const in{ boxes({ { 100, 50 }, { 100, 50 }, { 50, 50 } }) };
  Packing const p{ pack_lr(in, 10, 16, 10) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[2].x == 110);
  CHECK(p.at[2].y == 0);
  CHECK(p.at[2].w == 50);
  CHECK(p.at[2].h == 110);
  CHECK(p.at[0].h == 50);
  CHECK(p.at[1].h == 50);
  CHECK(p.w == 160);
  CHECK(p.h == 110);
  check_expansion(in, 10, 16, 10);
}

TEST_CASE("pack: whitespace elimination shares a row's slack between its blocks") {
  // A row 310 wide inside a 400-wide drawing, in two blocks. The 90 spare is
  // halved between them and the second block shifts right by the first's
  // share, so the gap between the two is still the 10 the placement gave them
  // and the row's right edge lands exactly on the drawing's.
  std::vector<scav_rect> const in{ boxes(
      { { 200, 100 }, { 200, 100 }, { 100, 100 }, { 400, 100 } }) };
  Packing const placed{ pack_rows(in, 10, 16, 10, false) };
  CHECK(placed.at[2].x == 210);
  CHECK(placed.w == 400);
  CHECK(placed.h == 320);

  Packing const p{ pack_lr(in, 10, 16, 10) };
  REQUIRE(p.at.size() == 4);
  CHECK(p.at[0] == scav_rect{ .x = 0, .y = 0, .w = 245, .h = 100 });
  CHECK(p.at[1] == scav_rect{ .x = 0, .y = 110, .w = 245, .h = 100 });
  CHECK(p.at[2] == scav_rect{ .x = 255, .y = 0, .w = 145, .h = 210 });
  CHECK(p.at[3] == scav_rect{ .x = 0, .y = 220, .w = 400, .h = 100 });
  CHECK(p.w == 400);
  CHECK(p.h == 320);
  // The box less its gaps: two inside the first row, one between the rows.
  CHECK(filled(p) == 119450);
  check_expansion(in, 10, 16, 10);
}
TEST_CASE("pack: the row packer levels its rects' heights") {
  // One row, so the only whitespace is under the short ones, and every rect
  // comes back as tall as the tallest.
  Packing const p{ pack_box(boxes({ { 40, 10 }, { 900, 300 }, { 5, 5 } }), 7) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[0] == scav_rect{ .x = 0, .y = 0, .w = 40, .h = 300 });
  CHECK(p.at[1] == scav_rect{ .x = 47, .y = 0, .w = 900, .h = 300 });
  CHECK(p.at[2] == scav_rect{ .x = 954, .y = 0, .w = 5, .h = 300 });
  CHECK(p.w == 959);
  CHECK(p.h == 300);
  check_sane(p, 7);
}

TEST_CASE("pack: a rect with no extent is not grown into one") {
  // A live submachine that sized to nothing packs as a zero rect, and filling
  // the row around it must not give it a band a reader would look for
  // contents in.
  std::vector<scav_rect> const in{ boxes({ { 400, 300 }, { 0, 0 }, { 200, 100 } }) };
  Packing const p{ pack_lr(in, 10, 16, 10) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[1].w == 0);
  CHECK(p.at[1].h == 0);
  check_expansion(in, 10, 16, 10);
}
