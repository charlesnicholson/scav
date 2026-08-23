// Interior subdivision, author-supplied line breaks, and the arrowhead's
// integer geometry.

#include "scav/scav_draw.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_int.h"

#include "doctest.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

constexpr ElemRef NONE{ .kind = ElemKind::None, .ordinal = INVALID };

bool same(scav_rect a, scav_rect b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

}  // namespace

TEST_CASE("helpers: a vertical stack partitions its rect top down") {
  scav_rect const r{ .x = 10, .y = 20, .w = 100, .h = 60 };
  int32_t const heights[3]{ 10, 20, 30 };
  scav_rect out[3]{};
  stack_v(r, heights, 3, out);
  CHECK(same(out[0], scav_rect{ .x = 10, .y = 20, .w = 100, .h = 10 }));
  CHECK(same(out[1], scav_rect{ .x = 10, .y = 30, .w = 100, .h = 20 }));
  CHECK(same(out[2], scav_rect{ .x = 10, .y = 50, .w = 100, .h = 30 }));
}

TEST_CASE("helpers: a stack asking for more than it has is clipped, not overrun") {
  scav_rect const r{ .x = 0, .y = 0, .w = 10, .h = 10 };
  int32_t const heights[3]{ 8, 8, 8 };
  scav_rect out[3]{};
  stack_v(r, heights, 3, out);
  CHECK(out[0].h == 8);
  CHECK(out[1].h == 2);  // what was left
  CHECK(out[2].h == 0);
  CHECK(out[2].y == 10);
  // A negative request is zero, never a rect that inverts.
  int32_t const negative[2]{ -5, 4 };
  stack_v(r, negative, 2, out);
  CHECK(out[0].h == 0);
  CHECK(same(out[1], scav_rect{ .x = 0, .y = 0, .w = 10, .h = 4 }));
}

TEST_CASE("helpers: a horizontal row partitions its rect left to right") {
  scav_rect const r{ .x = 5, .y = 5, .w = 30, .h = 8 };
  int32_t const widths[2]{ 10, 25 };
  scav_rect out[2]{};
  row_h(r, widths, 2, out);
  CHECK(same(out[0], scav_rect{ .x = 5, .y = 5, .w = 10, .h = 8 }));
  CHECK(same(out[1], scav_rect{ .x = 15, .y = 5, .w = 20, .h = 8 }));  // clipped
}

TEST_CASE("helpers: a null array is a no-op rather than a crash") {
  scav_rect out[1]{ { .x = 7, .y = 7, .w = 7, .h = 7 } };
  scav_rect const r{ .x = 0, .y = 0, .w = 1, .h = 1 };
  stack_v(r, nullptr, 1, out);
  row_h(r, nullptr, 1, out);
  int32_t const one[1]{ 1 };
  stack_v(r, one, 1, nullptr);
  row_h(r, one, 1, nullptr);
  CHECK(same(out[0], scav_rect{ .x = 7, .y = 7, .w = 7, .h = 7 }));
}

TEST_CASE("helpers: every anchor cell places its content in the right corner") {
  scav_rect const r{ .x = 0, .y = 0, .w = 100, .h = 50 };
  CHECK(same(align(r, 10, 6, Anchor::TopLeft), scav_rect{ .x = 0, .y = 0, .w = 10, .h = 6 }));
  CHECK(same(align(r, 10, 6, Anchor::TopCentre), scav_rect{ .x = 45, .y = 0, .w = 10, .h = 6 }));
  CHECK(same(align(r, 10, 6, Anchor::TopRight), scav_rect{ .x = 90, .y = 0, .w = 10, .h = 6 }));
  CHECK(same(align(r, 10, 6, Anchor::MidLeft), scav_rect{ .x = 0, .y = 22, .w = 10, .h = 6 }));
  CHECK(same(align(r, 10, 6, Anchor::MidCentre), scav_rect{ .x = 45, .y = 22, .w = 10, .h = 6 }));
  CHECK(same(align(r, 10, 6, Anchor::BottomRight), scav_rect{ .x = 90, .y = 44, .w = 10, .h = 6 }));
  CHECK(same(align(r, 10, 6, Anchor::BottomCentre), scav_rect{ .x = 45, .y = 44, .w = 10, .h = 6 }));
  CHECK(same(align(r, 10, 6, Anchor::MidRight), scav_rect{ .x = 90, .y = 22, .w = 10, .h = 6 }));
  CHECK(same(align(r, 10, 6, Anchor::BottomLeft), scav_rect{ .x = 0, .y = 44, .w = 10, .h = 6 }));
}

TEST_CASE("helpers: content wider than its rect centres by floor, not toward zero") {
  scav_rect const r{ .x = 0, .y = 0, .w = 10, .h = 10 };
  // Slack is -5, and floor_div takes it to -3 rather than -2: overhang is
  // symmetric about the rect either way, and `/` would bias it one direction.
  CHECK(same(align(r, 15, 15, Anchor::MidCentre), scav_rect{ .x = -3, .y = -3, .w = 15, .h = 15 }));
}

TEST_CASE("helpers: the lines are the ones the author wrote") {
  CHECK(text_lines("one").size() == 1);
  CHECK(text_lines("one")[0] == "one");

  std::vector<std::string_view> const three{ text_lines("a\nbb\nccc") };
  REQUIRE(three.size() == 3);
  CHECK(three[0] == "a");
  CHECK(three[2] == "ccc");

  // An empty string is one empty line; a trailing newline adds none.
  REQUIRE(text_lines("").size() == 1);
  CHECK(text_lines("")[0].empty());
  CHECK(text_lines("a\n").size() == 1);
  // An interior blank line is one the author meant.
  REQUIRE(text_lines("a\n\nb").size() == 3);
  CHECK(text_lines("a\n\nb")[1].empty());
}

TEST_CASE("helpers: an arrowhead is a closed triangle behind its own tip") {
  DrawList d;
  uint32_t const s{ drawlist_style(
      d, { .stroke_rgba = 0, .fill_rgba = 0, .stroke_w = 1, .dash = 0,
           .font_size_grid = 0 }) };
  // Pointing straight down, so the barbs land either side of the shaft.
  push_arrowhead(d, 0, s, { .x = 100, .y = 200 }, { .x = 100, .y = 100 }, 20, NONE);
  REQUIRE(d.prims.size() == 1);
  CHECK(d.prims[0].kind == SCAV_PRIM_PATH);
  REQUIRE(d.prims[0].points.len == 3);
  scav_point const *pts{ d.points.data() + d.prims[0].points.off };
  CHECK(pts[0].x == 100);
  CHECK(pts[0].y == 200);
  CHECK(pts[1].y == 180);  // twenty back along the shaft
  CHECK(pts[2].y == 180);
  CHECK(pts[1].x == 90);   // ten either side
  CHECK(pts[2].x == 110);

  uint32_t bad{ 0 };
  CHECK(drawlist_validate(d, bad));
}

TEST_CASE("helpers: a degenerate arrowhead emits nothing rather than dividing by zero") {
  DrawList d;
  uint32_t const s{ drawlist_style(
      d, { .stroke_rgba = 0, .fill_rgba = 0, .stroke_w = 1, .dash = 0,
           .font_size_grid = 0 }) };
  scav_point const same{ .x = 5, .y = 5 };
  push_arrowhead(d, 0, s, same, same, 10, NONE);   // no direction to point in
  push_arrowhead(d, 0, s, same, { .x = 0, .y = 0 }, 0, NONE);   // no size
  push_arrowhead(d, 0, s, same, { .x = 0, .y = 0 }, -4, NONE);  // negative size
  CHECK(d.prims.empty());
}

TEST_CASE("helpers: an arrowhead points along any diagonal it is given") {
  DrawList d;
  uint32_t const s{ drawlist_style(
      d, { .stroke_rgba = 0, .fill_rgba = 0, .stroke_w = 1, .dash = 0,
           .font_size_grid = 0 }) };
  // Eight directions, each of which must put the tip where it was told and the
  // barbs somewhere behind it.
  constexpr scav_point FROM[8]{ { .x = 0, .y = 0 },  { .x = 200, .y = 0 },
                                { .x = 200, .y = 200 }, { .x = 0, .y = 200 },
                                { .x = 100, .y = 0 },   { .x = 200, .y = 100 },
                                { .x = 100, .y = 200 }, { .x = 0, .y = 100 } };
  scav_point const tip{ .x = 100, .y = 100 };
  for (scav_point const &from : FROM) {
    d.prims.clear();
    d.points.clear();
    push_arrowhead(d, 0, s, tip, from, 16, NONE);
    REQUIRE(d.prims.size() == 1);
    scav_point const *pts{ d.points.data() + d.prims[0].points.off };
    CHECK(pts[0].x == tip.x);
    CHECK(pts[0].y == tip.y);
    // A barb sits `size` back along the shaft and half that across it, so on a
    // diagonal each axis carries a share of both.
    for (uint32_t i = 1; i < 3; ++i) {
      CHECK(imax(pts[i].x - tip.x, tip.x - pts[i].x) <= 24);
      CHECK(imax(pts[i].y - tip.y, tip.y - pts[i].y) <= 24);
    }
    // And they are not the same point, so the triangle has area.
    CHECK(((pts[1].x != pts[2].x) || (pts[1].y != pts[2].y)));
  }
}
