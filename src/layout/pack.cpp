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
// edge, which is what a new block at row level has to clear. Wide because a
// row or a column sums over every rect, and the domain bounds one rect at a
// time; the sum stays inside int64 because a vector that long is not
// addressable.
struct Cursor {
  Wide row_y{ 0 }, row_h{ 0 }, row_right{ 0 };
  Wide block_x{ 0 };
  Wide sub_y{ 0 }, sub_h{ 0 }, sub_x{ 0 };
};

// Every distance here is non-negative, so one bound is the whole clamp.
int32_t narrow(Wide v) { return static_cast<int32_t>(imin(v, Wide{ PACK_SATURATED })); }

// Where the area below stops. It only picks a target width that is then
// clamped to the domain, and at 2^48 the floor-sqrt already clears COORD_MAX
// for every ratio a profile allows (dar_num >= 1, dar_den <= 1024), so the cap
// changes no target and keeps `area * dar_num` inside int64.
constexpr Wide AREA_MAX{ Wide{ 1 } << 48 };

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
    area = imin(area + ((Wide{ r.w } + sep) * (Wide{ r.h } + sep)), AREA_MAX);
    widest = imax(widest, Wide{ r.w });
  }
  // The published operation order: multiply, floor-divide, then floor-sqrt. A
  // target under the widest rect is unsatisfiable, so that is the floor.
  Wide const approx{ static_cast<Wide>(
      isqrt(static_cast<uint64_t>(floor_div(area * dar_num, Wide{ dar_den })))) };
  Wide const target{ imin(imax(widest, approx), Wide{ COORD_MAX }) };

  Cursor at;
  out.at[0].x = 0;
  out.at[0].y = 0;
  at.row_h = rects[0].h;
  at.row_right = rects[0].w;
  at.sub_h = rects[0].h;
  at.sub_x = Wide{ rects[0].w } + sep;
  Wide right{ rects[0].w };
  Wide bottom{ rects[0].h };

  for (uint32_t i = 1; i < rects.size(); ++i) {
    Wide const w{ rects[i].w };
    Wide const h{ rects[i].h };

    bool const lower{ at.sub_y > at.row_y };
    Wide const level_x{ at.row_right + sep };
    Wide x{ 0 };
    if (lower && ((level_x + w) <= target)) {
      // Right on the current row level. Preferred over carrying on rightward
      // from a lower subrow, which would leave a notch above that nothing
      // short of growing a neighbour could fill.
      at.block_x = level_x;
      at.sub_y = at.row_y;
      at.sub_h = h;
      x = level_x;
    } else if ((at.sub_x + w) <= target) {
      x = at.sub_x;
      at.sub_h = imax(at.sub_h, h);
    } else if ((at.block_x + w) <= target) {
      // Next subrow, at the block's left edge.
      at.sub_y = at.sub_y + at.sub_h + sep;
      at.sub_h = h;
      x = at.block_x;
    } else {
      at.row_y = at.row_y + at.row_h + sep;
      at.row_h = 0;
      at.row_right = 0;
      at.block_x = 0;
      at.sub_y = at.row_y;
      at.sub_h = h;
    }
    // Each branch above leaves `sub_y` on the subrow it settled the rect into,
    // the row-level two by resetting it to the row.
    out.at[i].x = narrow(x);
    out.at[i].y = narrow(at.sub_y);
    at.sub_x = x + w + sep;
    at.row_right = imax(at.row_right, x + w);
    at.row_h = imax(at.row_h, (at.sub_y - at.row_y) + h);
    right = imax(right, x + w);
    bottom = imax(bottom, at.sub_y + h);
  }

  out.w = narrow(right);
  out.h = narrow(bottom);
  return out;
}

Packing pack_box(std::vector<scav_rect> const &rects, int32_t sep) {
  Packing out;
  out.at = rects;
  Wide x{ 0 };
  for (scav_rect &r : out.at) {
    r.x = narrow(x);
    r.y = 0;
    out.w = narrow(x + r.w);
    out.h = imax(out.h, r.h);
    x += Wide{ r.w } + sep;
  }
  return out;
}

bool pack_better(Packing const &a,
                 Packing const &b,
                 int32_t dar_num,
                 int32_t dar_den,
                 bool aspect_first) {
  // SM = min(dar_num / (dar_den * w), 1 / h), whichever of the two is
  // smaller, kept as a numerator and denominator and never divided. Extents
  // stop just under 2^31 and a profile's ratio at 2^10, so the widest product
  // below is the area at under 2^62 and the rest are 2^51 or less.
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
