// Packing tests: hand-worked targets and positions on fixed rect lists, then the
// invariants every packing keeps over generated shapes.

#include "layout/pack.h"
#include "layout/tests/pod_eq.h"

#include "scav_int.h"
#include "scav_rnd.h"

#include "doctest.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace scav {

// `pack_lr` with compaction and whitespace elimination each selectable; defined in
// `pack.cpp` under SCAV_INTERNAL.
Packing pack_rows(std::vector<scav_rect> const &rects,
                  int32_t sep,
                  int32_t dar_num,
                  int32_t dar_den,
                  Compaction compaction,
                  bool expanded);

}  // namespace scav

namespace {

using namespace scav;

Packing pack_lr(std::vector<scav_rect> const &rects,
                int32_t sep,
                int32_t dar_num,
                int32_t dar_den,
                Compaction compaction) {
  Packing out;
  scav::pack_lr(out, rects, sep, dar_num, dar_den, compaction);
  return out;
}

Packing pack_box(std::vector<scav_rect> const &rects, int32_t sep) {
  Packing out;
  scav::pack_box(out, rects, sep);
  return out;
}

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

// The gap between two rects on one axis, zero where they overlap on it.
int32_t apart(int32_t a_lo, int32_t a_len, int32_t b_lo, int32_t b_len) {
  return imax(imax(b_lo - (a_lo + a_len), a_lo - (b_lo + b_len)), 0);
}

// Every rect lies inside the extents, no two overlap or come closer than `sep`, and
// no later rect is both left of and at or above an earlier one.
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

// Compaction keeps the filled area and shrinks or keeps each extent.
void check_compaction(std::vector<scav_rect> const &rects,
                      int32_t sep,
                      int32_t dar_num,
                      int32_t dar_den) {
  Packing const placed{ pack_rows(rects, sep, dar_num, dar_den, Compaction::Off, false) };
  Packing const packed{ pack_rows(rects, sep, dar_num, dar_den, Compaction::On, false) };
  check_sane(placed, sep);
  check_sane(packed, sep);
  CHECK(packed.w <= placed.w);
  CHECK(packed.h <= placed.h);
  CHECK(filled(packed) == filled(placed));
  CHECK((int64_t{ packed.w } * packed.h) <= (int64_t{ placed.w } * placed.h));
  // A packing inside the domain stays inside it after compaction.
  if ((placed.w <= COORD_MAX) && (placed.h <= COORD_MAX)) {
    CHECK(packed.w <= COORD_MAX);
    CHECK(packed.h <= COORD_MAX);
  }
}

// Whitespace elimination keeps both extents; each rect grows or stays and moves
// only right or down, and a zero-extent rect keeps its size.
void check_expansion(std::vector<scav_rect> const &rects,
                     int32_t sep,
                     int32_t dar_num,
                     int32_t dar_den,
                     Compaction compaction) {
  Packing const placed{ pack_rows(rects, sep, dar_num, dar_den, compaction, false) };
  Packing const grown{ pack_rows(rects, sep, dar_num, dar_den, compaction, true) };
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
  // A packing inside the domain stays inside it after whitespace elimination.
  if ((placed.w <= COORD_MAX) && (placed.h <= COORD_MAX)) {
    CHECK(grown.w <= COORD_MAX);
    CHECK(grown.h <= COORD_MAX);
  }
}

}  // namespace

TEST_CASE("pack: nothing to pack") {
  Packing const p{ pack_lr({}, 10, 16, 10, Compaction::Off) };
  CHECK(p.at.empty());
  CHECK(p.w == 0);
  CHECK(p.h == 0);
  Packing const b{ pack_box({}, 10) };
  CHECK(b.at.empty());
  CHECK(b.w == 0);
}

TEST_CASE("pack: one rect sits at the origin and is the whole extent") {
  Packing const p{ pack_lr(boxes({ { 300, 120 } }), 10, 16, 10, Compaction::Off) };
  REQUIRE(p.at.size() == 1);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.w == 300);
  CHECK(p.h == 120);
}

TEST_CASE("pack: four squares come out square, gaps paid for") {
  // Inflated area is 4 * 110 * 110 = 48400 at DAR 1:1, so the target is
  // isqrt(48400) = 220 and two 100-wide rects fit in a row with 10 between.
  Packing const p{ pack_lr(
      boxes({ { 100, 100 }, { 100, 100 }, { 100, 100 }, { 100, 100 } }),
      10,
      1,
      1,
      Compaction::Off) };
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
  // At separation 0 the target is isqrt(40000) = 200, the width of two rects.
  Packing const p{ pack_lr(
      boxes({ { 100, 100 }, { 100, 100 }, { 100, 100 }, { 100, 100 } }),
      0,
      1,
      1,
      Compaction::Off) };
  CHECK(p.w == 200);
  CHECK(p.h == 200);
}

TEST_CASE("pack: a rect after a wrap goes back to the row's top level") {
  // Target is isqrt(16800 * 16 / 10) = 163. The second rect (210 > 163) wraps to a
  // subrow; the third fits at row level (100 + 10 + 50 = 160 <= 163) and goes there.
  Packing const p{
    pack_lr(boxes({ { 100, 50 }, { 100, 50 }, { 50, 50 } }), 10, 16, 10, Compaction::Off)
  };
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
  // The target floors at the widest rect, 900, so each rect sits under the last.
  Packing const p{
    pack_lr(boxes({ { 900, 40 }, { 900, 40 }, { 900, 40 } }), 10, 16, 10, Compaction::Off)
  };
  REQUIRE(p.at.size() == 3);
  CHECK(p.w == 900);
  for (uint32_t i = 0; i < 3; ++i) { CHECK(p.at[i].x == 0); }
  CHECK(p.at[0].y < p.at[1].y);
  CHECK(p.at[1].y < p.at[2].y);
  check_sane(p, 10);
}

TEST_CASE("pack: a rect that fits neither its subrow nor its block opens a row") {
  // Target is isqrt(15600 * 16 / 10) = 157. The third rect starts a block at x=120;
  // the fourth fits neither beside it (160 + 40) nor at its left edge (120 + 40).
  Packing const p{ pack_lr(boxes({ { 40, 50 }, { 110, 50 }, { 30, 50 }, { 40, 50 } }),
                           10,
                           16,
                           10,
                           Compaction::Off) };
  REQUIRE(p.at.size() == 4);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 0);
  CHECK(p.at[1].y == 60);
  CHECK(p.at[2].x == 120);
  CHECK(p.at[2].y == 0);
  // The new row starts below the first row's lower subrow.
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
  // At 10:16 the preference reverses.
  CHECK(pack_better(tall, wide, 10, 16, false));
}

TEST_CASE("pack: the scale measure already prefers the smaller of two similar shapes") {
  Packing const small{ .at = {}, .w = 160, .h = 100 };
  Packing const large{ .at = {}, .w = 320, .h = 200 };
  CHECK(pack_better(small, large, 16, 10, false));
}

TEST_CASE("pack: equal scale falls to the tiebreak the profile picked") {
  // Both are height-bound at 100 and scale equally. The tiebreak picks the smaller
  // area, 100x100, or with `aspect_first` the 16:10 shape, 160x100.
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
    Packing const p{ pack_lr(boxes(wh), 13, 16, 10, Compaction::Off) };
    REQUIRE(p.at.size() == wh.size());
    check_sane(p, 13);
    CHECK(same(p.at, pack_lr(boxes(wh), 13, 16, 10, Compaction::Off).at));
    check_sane(pack_box(boxes(wh), 13), 13);
    check_expansion(boxes(wh), 13, 16, 10, Compaction::Off);
  }
}

TEST_CASE("pack: a column of maximal rects saturates rather than wrapping") {
  // 5000 COORD_MAX squares, one per row: the column sums to 2.6 billion and `h`
  // saturates.
  std::vector<scav_rect> const tall(5000,
                                    { .x = 0, .y = 0, .w = COORD_MAX, .h = COORD_MAX });

  Packing const p{ pack_lr(tall, 0, 16, 10, Compaction::Off) };
  REQUIRE(p.at.size() == tall.size());
  CHECK(p.w == COORD_MAX);
  CHECK(p.h == PACK_SATURATED);
  CHECK(p.h > COORD_MAX);  // the test callers apply
  bool ordered{ true };
  for (uint32_t i = 1; i < p.at.size(); ++i) {
    ordered = ordered && (p.at[i].y >= p.at[i - 1].y) && (p.at[i].x >= 0);
  }
  CHECK(ordered);

  // `pack_box` sums widths and saturates the same way.
  Packing const row{ pack_box(tall, 0) };
  CHECK(row.w == PACK_SATURATED);
  CHECK(row.h == COORD_MAX);
}

TEST_CASE("pack: one oversized child sets the target and nothing shares its row") {
  // The widest rect floors the target at 1000; the other two share the subrow under it.
  std::vector<scav_rect> const in{ boxes({ { 1000, 100 }, { 100, 100 }, { 100, 100 } }) };
  Packing const p{ pack_rows(in, 10, 16, 10, Compaction::Off, false) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 0);
  CHECK(p.at[1].y == 110);
  CHECK(p.at[2].x == 110);
  CHECK(p.at[2].y == 110);
  CHECK(p.w == 1000);
  CHECK(p.h == 210);
  check_expansion(in, 10, 16, 10, Compaction::Off);
}

TEST_CASE("pack: equal heights are where the row packer wins") {
  // Two regions of equal height: the target fits one, so `pack_lr` stacks them, and
  // `pack_box`'s single row scores better.
  std::vector<scav_rect> const in{ boxes({ { 1888, 1594 }, { 1773, 1594 } }) };
  Packing const p{ pack_lr(in, 192, 16, 10, Compaction::Off) };
  CHECK(p.w == 1888);
  CHECK(p.h == 3380);
  Packing const row{ pack_box(in, 192) };
  CHECK(row.w == 3853);
  CHECK(row.h == 1594);
  CHECK(pack_better(row, p, 16, 10, false));
  check_expansion(in, 192, 16, 10, Compaction::Off);
}

TEST_CASE("pack: both last steps keep every property over a seeded spread") {
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
    check_compaction(in, sep, 16, 10);
    check_expansion(in, sep, 16, 10, Compaction::Off);
    check_expansion(in, sep, 16, 10, Compaction::On);
    // Two runs of the same input give the same positions.
    CHECK(same(pack_lr(in, sep, 16, 10, Compaction::Off).at,
               pack_lr(in, sep, 16, 10, Compaction::Off).at));
  }
}

TEST_CASE("pack: a saturated column is left as it is") {
  // 64 COORD_MAX squares in one column, past the domain: whitespace elimination
  // keeps placement's positions.
  std::vector<scav_rect> const tall(64,
                                    { .x = 0, .y = 0, .w = COORD_MAX, .h = COORD_MAX });
  Packing const placed{ pack_rows(tall, 0, 16, 10, Compaction::Off, false) };
  Packing const p{ pack_lr(tall, 0, 16, 10, Compaction::Off) };
  CHECK(p.w == COORD_MAX);
  CHECK(p.h == (64 * COORD_MAX));
  CHECK(same(p.at, placed.at));
}

TEST_CASE("pack: whitespace elimination fills a column to the drawing's width") {
  // A column of three subrows, one rect each: the narrower two grow to the widest's
  // 2695.
  std::vector<scav_rect> const in{ boxes(
      { { 2695, 653 }, { 1434, 653 }, { 1895, 653 } }) };
  Packing const p{ pack_lr(in, 288, 16, 10, Compaction::Off) };
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
  // Filled area is the box less the two 288 gaps.
  CHECK(filled(p) == (int64_t{ p.w } * (p.h - (2 * 288))));
  check_expansion(in, 288, 16, 10, Compaction::Off);
}

TEST_CASE("pack: whitespace elimination fills the notch a row-level rect left") {
  // The third rect sits at row level and grows from 50 x 50 to 50 x 110, down to the
  // row's floor.
  std::vector<scav_rect> const in{ boxes({ { 100, 50 }, { 100, 50 }, { 50, 50 } }) };
  Packing const p{ pack_lr(in, 10, 16, 10, Compaction::Off) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[2].x == 110);
  CHECK(p.at[2].y == 0);
  CHECK(p.at[2].w == 50);
  CHECK(p.at[2].h == 110);
  CHECK(p.at[0].h == 50);
  CHECK(p.at[1].h == 50);
  CHECK(p.w == 160);
  CHECK(p.h == 110);
  check_expansion(in, 10, 16, 10, Compaction::Off);
}

TEST_CASE("pack: whitespace elimination shares a row's slack between its blocks") {
  // A 310-wide row in a 400-wide drawing, in two blocks: each block gets 45 of the
  // 90 spare, keeping the 10 gap and ending the row at the drawing's right edge.
  std::vector<scav_rect> const in{ boxes(
      { { 200, 100 }, { 200, 100 }, { 100, 100 }, { 400, 100 } }) };
  Packing const placed{ pack_rows(in, 10, 16, 10, Compaction::Off, false) };
  CHECK(placed.at[2].x == 210);
  CHECK(placed.w == 400);
  CHECK(placed.h == 320);

  Packing const p{ pack_lr(in, 10, 16, 10, Compaction::Off) };
  REQUIRE(p.at.size() == 4);
  CHECK(p.at[0] == scav_rect{ .x = 0, .y = 0, .w = 245, .h = 100 });
  CHECK(p.at[1] == scav_rect{ .x = 0, .y = 110, .w = 245, .h = 100 });
  CHECK(p.at[2] == scav_rect{ .x = 255, .y = 0, .w = 145, .h = 210 });
  CHECK(p.at[3] == scav_rect{ .x = 0, .y = 220, .w = 400, .h = 100 });
  CHECK(p.w == 400);
  CHECK(p.h == 320);
  // The box less its gaps: two inside the first row, one between the rows.
  CHECK(filled(p) == 119450);
  check_expansion(in, 10, 16, 10, Compaction::Off);
}
TEST_CASE("pack: the row packer levels its rects' heights") {
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
  std::vector<scav_rect> const in{ boxes({ { 400, 300 }, { 0, 0 }, { 200, 100 } }) };
  Packing const p{ pack_lr(in, 10, 16, 10, Compaction::Off) };
  REQUIRE(p.at.size() == 3);
  CHECK(p.at[1].w == 0);
  CHECK(p.at[1].h == 0);
  check_expansion(in, 10, 16, 10, Compaction::Off);
}

TEST_CASE("pack: compaction takes the late arrival back into the hole above it") {
  // Target is isqrt(240400 * 16 / 10) = 620: placement puts the third rect at row
  // level and the fourth in a new row.
  std::vector<scav_rect> const in{ boxes(
      { { 500, 100 }, { 200, 400 }, { 100, 100 }, { 200, 400 } }) };

  Packing const placed{ pack_rows(in, 10, 16, 10, Compaction::Off, false) };
  REQUIRE(placed.at.size() == 4);
  CHECK(placed.at[0].x == 0);
  CHECK(placed.at[0].y == 0);
  CHECK(placed.at[1].x == 0);
  CHECK(placed.at[1].y == 110);
  CHECK(placed.at[2].x == 510);
  CHECK(placed.at[2].y == 0);
  CHECK(placed.at[3].x == 0);
  CHECK(placed.at[3].y == 520);
  CHECK(placed.w == 610);
  CHECK(placed.h == 920);

  Packing const p{ pack_rows(in, 10, 16, 10, Compaction::On, false) };
  REQUIRE(p.at.size() == 4);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 0);
  CHECK(p.at[1].y == 110);
  CHECK(p.at[2].x == 210);
  CHECK(p.at[2].y == 110);
  CHECK(p.at[3].x == 210);
  CHECK(p.at[3].y == 220);
  CHECK(p.w == 500);
  CHECK(p.h == 620);
  // Occupancy rises from 39.20% to 70.97%.
  CHECK((filled(p) * (int64_t{ placed.w } * placed.h)) >
        (filled(placed) * (int64_t{ p.w } * p.h)));
  check_compaction(in, 10, 16, 10);
  check_expansion(in, 10, 16, 10, Compaction::On);
}

TEST_CASE("pack: compaction pulls a row-level rect back beside its predecessor") {
  // Placement puts the third rect at row level (1696 + 288 + 1088 = 3072 <= 3186);
  // compaction moves it beside the second, 262 to the left.
  std::vector<scav_rect> const in{ boxes(
      { { 1696, 941 }, { 1434, 1229 }, { 1088, 653 } }) };

  Packing const placed{ pack_rows(in, 288, 16, 10, Compaction::Off, false) };
  CHECK(placed.at[2].x == 1984);
  CHECK(placed.w == 3072);
  CHECK(placed.h == 2458);

  Packing const p{ pack_rows(in, 288, 16, 10, Compaction::On, false) };
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 0);
  CHECK(p.at[1].y == 1229);
  CHECK(p.at[2].x == 1722);
  CHECK(p.at[2].y == 1229);
  CHECK(p.w == 2810);
  CHECK(p.h == 2458);
  check_compaction(in, 288, 16, 10);
  check_expansion(in, 288, 16, 10, Compaction::On);
}

TEST_CASE("pack: vac's root frame is a packing compaction cannot improve") {
  std::vector<scav_rect> const in{ boxes(
      { { 3066, 2535 }, { 2842, 1050 }, { 10785, 6299 }, { 1696, 1594 } }) };

  Packing const p{ pack_rows(in, 288, 16, 10, Compaction::On, false) };
  REQUIRE(p.at.size() == 4);
  CHECK(p.at[0].x == 0);
  CHECK(p.at[0].y == 0);
  CHECK(p.at[1].x == 3354);
  CHECK(p.at[1].y == 0);
  CHECK(p.at[2].x == 0);
  CHECK(p.at[2].y == 2823);
  CHECK(p.at[3].x == 0);
  CHECK(p.at[3].y == 9410);
  CHECK(p.w == 10785);
  CHECK(p.h == 11004);
  CHECK(same(p.at, pack_rows(in, 288, 16, 10, Compaction::Off, false).at));
  check_compaction(in, 288, 16, 10);
}

TEST_CASE("pack: compaction leaves a saturated column saturated") {
  // 64 COORD_MAX squares in one column: compaction keeps placement's positions.
  std::vector<scav_rect> const tall(64,
                                    { .x = 0, .y = 0, .w = COORD_MAX, .h = COORD_MAX });
  Packing const placed{ pack_rows(tall, 0, 16, 10, Compaction::Off, false) };
  Packing const p{ pack_lr(tall, 0, 16, 10, Compaction::On) };
  CHECK(p.w == COORD_MAX);
  CHECK(p.h == (64 * COORD_MAX));
  CHECK(same(p.at, placed.at));
}
