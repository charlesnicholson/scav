// LR-rectpacking's shape without its later refinements: a target width from
// the desired aspect ratio, then every rect placed at one of four positions
// relative to its predecessor. A row holds blocks left to right, a block
// holds subrows top to bottom, and a subrow holds rects left to right, so
// reading order is the placement order by construction.

#include "layout/pack.h"

#include "scav_int.h"

#include <cstdint>
#include <vector>

namespace scav {

namespace {

// Where the packing has got to. `row_right` is the whole row's rightmost
// edge, which is what a new block at row level has to clear.
struct Cursor {
  int32_t row_y{ 0 }, row_h{ 0 }, row_right{ 0 };
  int32_t block_x{ 0 };
  int32_t sub_y{ 0 }, sub_h{ 0 }, sub_x{ 0 };
};

}  // namespace

Packing pack_lr(std::vector<scav_rect> const &rects,
                int32_t sep,
                int32_t dar_num,
                int32_t dar_den) {
  Packing out;
  out.at = rects;
  if (rects.empty()) { return out; }

  // Area with one gap folded into each rect, because a target that pays for
  // no separation is one two rects can never share: at 100 wide and 10 apart,
  // the bare-area target of 200 leaves them stacked in a column.
  Wide area{ 0 };
  Wide widest{ 0 };
  for (scav_rect const &r : rects) {
    area += static_cast<Wide>(r.w + sep) * (r.h + sep);
    widest = imax(widest, Wide{ r.w });
  }
  // The published operation order: multiply, floor-divide, then floor-sqrt. A
  // target under the widest rect is unsatisfiable, so that is the floor.
  Wide const approx{ static_cast<Wide>(
      isqrt(static_cast<uint64_t>(floor_div(area * dar_num, Wide{ dar_den })))) };
  int32_t const target{ static_cast<int32_t>(
      imin(imax(widest, approx), Wide{ COORD_MAX })) };

  Cursor at;
  out.at[0].x = 0;
  out.at[0].y = 0;
  at.row_h = rects[0].h;
  at.row_right = rects[0].w;
  at.sub_h = rects[0].h;
  at.sub_x = rects[0].w + sep;

  for (uint32_t i = 1; i < rects.size(); ++i) {
    int32_t const w{ rects[i].w };
    int32_t const h{ rects[i].h };

    bool const lower{ at.sub_y > at.row_y };
    int32_t const level_x{ at.row_right + sep };
    if (lower && ((level_x + w) <= target)) {
      // Right on the current row level. Preferred over carrying on rightward
      // from a lower subrow, which would leave a notch above that nothing
      // short of growing a neighbour could fill.
      at.block_x = level_x;
      at.sub_y = at.row_y;
      at.sub_h = h;
      out.at[i].x = level_x;
      out.at[i].y = at.row_y;
    } else if ((at.sub_x + w) <= target) {
      out.at[i].x = at.sub_x;
      out.at[i].y = at.sub_y;
      at.sub_h = imax(at.sub_h, h);
    } else if ((at.block_x + w) <= target) {
      // Next subrow, at the block's left edge.
      at.sub_y = at.sub_y + at.sub_h + sep;
      at.sub_h = h;
      out.at[i].x = at.block_x;
      out.at[i].y = at.sub_y;
    } else {
      at.row_y = at.row_y + at.row_h + sep;
      at.row_h = 0;
      at.row_right = 0;
      at.block_x = 0;
      at.sub_y = at.row_y;
      at.sub_h = h;
      out.at[i].x = 0;
      out.at[i].y = at.row_y;
    }
    at.sub_x = out.at[i].x + w + sep;
    at.row_right = imax(at.row_right, out.at[i].x + w);
    at.row_h = imax(at.row_h, (out.at[i].y - at.row_y) + h);
  }

  for (scav_rect const &r : out.at) {
    out.w = imax(out.w, r.x + r.w);
    out.h = imax(out.h, r.y + r.h);
  }
  return out;
}

Packing pack_box(std::vector<scav_rect> const &rects, int32_t sep) {
  Packing out;
  out.at = rects;
  int32_t x{ 0 };
  for (scav_rect &r : out.at) {
    r.x = x;
    r.y = 0;
    x += r.w + sep;
    out.w = r.x + r.w;
    out.h = imax(out.h, r.h);
  }
  return out;
}

bool pack_better(Packing const &a,
                 Packing const &b,
                 int32_t dar_num,
                 int32_t dar_den,
                 bool aspect_first) {
  // SM = min(dar_num / (dar_den * w), 1 / h), whichever of the two is
  // smaller, kept as a numerator and denominator and never divided.
  auto const measure = [&](Packing const &p, int64_t &num, int64_t &den) {
    int64_t const w{ imax(int64_t{ p.w }, int64_t{ 1 }) };
    int64_t const h{ imax(int64_t{ p.h }, int64_t{ 1 }) };
    if ((int64_t{ dar_num } * h) < (int64_t{ dar_den } * w)) {
      num = dar_num;
      den = int64_t{ dar_den } * w;
    } else {
      num = 1;
      den = h;
    }
  };
  int64_t lhs_num{ 0 };
  int64_t lhs_den{ 1 };
  int64_t rhs_num{ 0 };
  int64_t rhs_den{ 1 };
  measure(a, lhs_num, lhs_den);
  measure(b, rhs_num, rhs_den);
  bool const a_smaller{ ratio_less(lhs_num, lhs_den, rhs_num, rhs_den) };
  bool const b_smaller{ ratio_less(rhs_num, rhs_den, lhs_num, lhs_den) };
  if (!a_smaller && !b_smaller) {
    int64_t const a_area{ int64_t{ a.w } * a.h };
    int64_t const b_area{ int64_t{ b.w } * b.h };
    int64_t const a_dev{ int64_t{ a.w } * dar_den - int64_t{ a.h } * dar_num };
    int64_t const b_dev{ int64_t{ b.w } * dar_den - int64_t{ b.h } * dar_num };
    int64_t const a_abs{ (a_dev < 0) ? -a_dev : a_dev };
    int64_t const b_abs{ (b_dev < 0) ? -b_dev : b_dev };
    if (aspect_first) { return (a_abs != b_abs) ? (a_abs < b_abs) : (a_area < b_area); }
    return (a_area != b_area) ? (a_area < b_area) : (a_abs < b_abs);
  }
  return b_smaller;  // a scales larger, which is the better packing
}

}  // namespace scav
