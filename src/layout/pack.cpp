// LR-rectpacking: target width, four-spot placement, compaction, whitespace elimination.
// Rows hold blocks left to right; blocks hold subrows top to bottom; subrows hold rects.

#include "layout/pack.h"

#include "scav_int.h"
#include "scav_internal.h"
#include "scav_vector.h"

#include <cstdint>

namespace scav {

// Test entry: `pack_lr` with whitespace elimination switchable.
SCAV_INTERNAL_BEGIN
Packing pack_rows(Vector<scav_rect> const &rects,
                  int32_t sep,
                  int32_t dar_num,
                  int32_t dar_den,
                  Compaction compaction,
                  bool expanded);
SCAV_INTERNAL_END

namespace {

// Where a rect goes relative to its predecessor: right of it, next subrow of its block,
// new block at the row's top level, or new row. Ordered finest first.
enum class Spot : uint8_t { Right = 0, Subrow = 1, Level = 2, Row = 3 };
constexpr uint8_t SPOTS{ 4 };

// Placement state after the latest rect; `row_right` is the row's rightmost edge.
struct Cursor {
  Wide row_y{ 0 }, row_h{ 0 }, row_right{ 0 };
  Wide block_x{ 0 };
  Wide sub_y{ 0 }, sub_h{ 0 }, sub_x{ 0 };
};

// A packing's extents before narrowing to int32.
struct Extent {
  Wide w{ 0 }, h{ 0 };
};

// Per-thread spot list, reassigned in place by each (non-nested) packing.
Vector<Spot> &spot_scratch() {
  thread_local Vector<Spot> s;
  return s;
}

// Clamps a non-negative `v` to PACK_SATURATED.
int32_t narrow(Wide v) { return static_cast<int32_t>(imin(v, Wide{ PACK_SATURATED })); }

// Area cap: at any profile ratio its target width exceeds COORD_MAX, and
// `area * dar_num` fits in int64.
constexpr Wide AREA_MAX{ Wide{ 1 } << 48 };

Wide target_width(Vector<scav_rect> const &rects,
                  int32_t sep,
                  int32_t dar_num,
                  int32_t dar_den) {
  // Area with `sep` added to each rect's width and height, capped at AREA_MAX.
  Wide area{ 0 };
  Wide widest{ 0 };
  for (scav_rect const &r : rects) {
    area = imin(area + ((Wide{ r.w } + sep) * (Wide{ r.h } + sep)), AREA_MAX);
    widest = imax(widest, Wide{ r.w });
  }
  // The published operation order: multiply, floor-divide, then floor-sqrt.
  Wide const approx{ static_cast<Wide>(
      isqrt(static_cast<uint64_t>(floor_div(area * dar_num, Wide{ dar_den })))) };
  return imin(imax(widest, approx), Wide{ COORD_MAX });
}

// The cursor after the first rect, placed at the origin.
Cursor seeded(scav_rect const &first, int32_t sep) {
  Cursor at;
  at.row_h = first.h;
  at.row_right = first.w;
  at.sub_h = first.h;
  at.sub_x = Wide{ first.w } + sep;
  return at;
}

// Advances the cursor to `spot` and returns the rect's left edge; its top is the
// cursor's `sub_y` afterwards.
Wide seat(Cursor &at, Spot spot, Wide w, Wide h, int32_t sep) {
  switch (spot) {
    case Spot::Right: at.sub_h = imax(at.sub_h, h); break;
    case Spot::Subrow:
      at.sub_y = at.sub_y + at.sub_h + sep;
      at.sub_h = h;
      at.sub_x = at.block_x;
      break;
    case Spot::Level:
      at.block_x = at.row_right + sep;
      at.sub_y = at.row_y;
      at.sub_h = h;
      at.sub_x = at.block_x;
      break;
    case Spot::Row:
      at.row_y = at.row_y + at.row_h + sep;
      at.row_h = 0;
      at.row_right = 0;
      at.block_x = 0;
      at.sub_y = at.row_y;
      at.sub_h = h;
      at.sub_x = 0;
      break;
  }
  Wide const x{ at.sub_x };
  at.sub_x = x + w + sep;
  at.row_right = imax(at.row_right, x + w);
  at.row_h = imax(at.row_h, (at.sub_y - at.row_y) + h);
  return x;
}

// Writes each rect's position into `at`, whose sizes are already set, and returns the
// packing's extents.
Extent lay(Vector<scav_rect> const &rects,
           Vector<Spot> const &spot,
           int32_t sep,
           Vector<scav_rect> &at) {
  at[0].x = 0;
  at[0].y = 0;
  Cursor cur{ seeded(rects[0], sep) };
  Extent e{ .w = Wide{ rects[0].w }, .h = Wide{ rects[0].h } };
  for (uint32_t i = 1; i < rects.size(); ++i) {
    Wide const x{ seat(cur, spot[i], rects[i].w, rects[i].h, sep) };
    at[i].x = narrow(x);
    at[i].y = narrow(cur.sub_y);
    e.w = imax(e.w, x + rects[i].w);
    e.h = imax(e.h, cur.sub_y + rects[i].h);
  }
  return e;
}

// Each rect takes the first spot that fits within `target`: Level (only from a lower
// subrow), Right, Subrow, else Row.
void place(Vector<scav_rect> const &rects, int32_t sep, Wide target, Vector<Spot> &spot) {
  spot.assign(rects.size(), Spot::Row);
  Cursor at{ seeded(rects[0], sep) };
  for (uint32_t i = 1; i < rects.size(); ++i) {
    Wide const w{ rects[i].w };
    bool const lower{ at.sub_y > at.row_y };
    if (lower && ((at.row_right + sep + w) <= target)) {
      spot[i] = Spot::Level;
    } else if ((at.sub_x + w) <= target) {
      spot[i] = Spot::Right;
    } else if ((at.block_x + w) <= target) {
      spot[i] = Spot::Subrow;
    } else {
      spot[i] = Spot::Row;
    }
    (void)seat(at, spot[i], w, rects[i].h, sep);
  }
}

// True when `a` is smaller by area, then height, then width.
bool tighter(Extent const &a, Extent const &b) {
  Wide const a_area{ a.w * a.h };
  Wide const b_area{ b.w * b.h };
  if (a_area != b_area) { return a_area < b_area; }
  if (a.h != b.h) { return a.h < b.h; }
  return a.w < b.w;
}

// Each rect, in index order, tries its three other spots and takes the tightest that
// shrinks one extent and grows neither.
void compact(Vector<scav_rect> const &rects,
             int32_t sep,
             Vector<Spot> &spot,
             Vector<scav_rect> &at) {
  Extent cur{ lay(rects, spot, sep, at) };
  Cursor before{ seeded(rects[0], sep) };
  for (uint32_t i = 1; i < rects.size(); ++i) {
    Wide const w{ rects[i].w };
    Wide const h{ rects[i].h };
    Spot chosen{ spot[i] };
    Extent chosen_e{ cur };
    for (uint8_t s = 0; s < SPOTS; ++s) {
      Spot const cand{ static_cast<Spot>(s) };
      if (cand == spot[i]) { continue; }
      // Skips a spot putting the rect past the current extent before laying the packing
      // out.
      Cursor probe{ before };
      Wide const x{ seat(probe, cand, w, h, sep) };
      if (((x + w) > cur.w) || ((probe.sub_y + h) > cur.h)) { continue; }
      Spot const was{ spot[i] };
      spot[i] = cand;
      Extent const e{ lay(rects, spot, sep, at) };
      spot[i] = was;
      if ((e.w <= cur.w) && (e.h <= cur.h) && tighter(e, chosen_e)) {
        chosen = cand;
        chosen_e = e;
      }
    }
    spot[i] = chosen;
    cur = chosen_e;
    (void)seat(before, chosen, w, h, sep);
  }
}

// Sum of the first `i` of `parts` floor-apportioned shares of `extra`; all `parts`
// shares sum to `extra` and differ by at most one.
Wide prefix(Wide extra, uint32_t i, uint32_t parts) {
  return (parts == 0) ? Wide{ 0 } : floor_div(extra * i, Wide{ parts });
}

// First index after `at` whose spot is `level` or coarser, else `end`.
uint32_t run_end(Vector<Spot> const &spot, uint32_t at, uint32_t end, Spot level) {
  uint32_t next{ at + 1 };
  while ((next < end) && (spot[next] < level)) { ++next; }
  return next;
}

// Grows rows, blocks, subrows and rects to fill their parents, keeping every gap; a
// rect with zero width or height keeps its size and takes no share.
void expand(Vector<Spot> const &spot, Extent const &whole, Vector<scav_rect> &at) {
  uint32_t const n{ static_cast<uint32_t>(at.size()) };
  for (uint32_t row = 0; row < n;) {
    uint32_t const row_last{ run_end(spot, row, n, Spot::Row) };
    Wide const row_y{ at[row].y };
    Wide row_w{ 0 };
    Wide row_bottom{ 0 };
    uint32_t blocks{ 1 };
    for (uint32_t i = row; i < row_last; ++i) {
      row_w = imax(row_w, Wide{ at[i].x } + at[i].w);
      row_bottom = imax(row_bottom, Wide{ at[i].y } + at[i].h);
      blocks += ((i > row) && (spot[i] == Spot::Level)) ? 1U : 0U;
    }
    Wide const row_extra{ whole.w - row_w };
    Wide const row_h{ row_bottom - row_y };

    uint32_t block_ord{ 0 };
    for (uint32_t block = row; block < row_last;) {
      uint32_t const block_last{ run_end(spot, block, row_last, Spot::Level) };
      Wide const block_x{ at[block].x };
      Wide block_w{ 0 };
      Wide block_bottom{ 0 };
      uint32_t subrows{ 1 };
      for (uint32_t i = block; i < block_last; ++i) {
        block_w = imax(block_w, (Wide{ at[i].x } + at[i].w) - block_x);
        block_bottom = imax(block_bottom, Wide{ at[i].y } + at[i].h);
        subrows += ((i > block) && (spot[i] == Spot::Subrow)) ? 1U : 0U;
      }
      Wide const dx{ prefix(row_extra, block_ord, blocks) };
      Wide const target_w{ block_w + prefix(row_extra, block_ord + 1, blocks) - dx };
      Wide const block_extra{ row_h - (block_bottom - row_y) };

      uint32_t sub_ord{ 0 };
      for (uint32_t sub = block; sub < block_last;) {
        uint32_t const sub_last{ run_end(spot, sub, block_last, Spot::Subrow) };
        Wide sub_w{ 0 };
        Wide sub_h{ 0 };
        uint32_t growing{ 0 };
        for (uint32_t i = sub; i < sub_last; ++i) {
          sub_w = imax(sub_w, (Wide{ at[i].x } + at[i].w) - block_x);
          sub_h = imax(sub_h, Wide{ at[i].h });
          growing += ((at[i].w > 0) && (at[i].h > 0)) ? 1U : 0U;
        }
        Wide const dy{ prefix(block_extra, sub_ord, subrows) };
        Wide const grown_h{ sub_h + prefix(block_extra, sub_ord + 1, subrows) - dy };
        Wide const sub_extra{ target_w - sub_w };

        uint32_t grown{ 0 };
        for (uint32_t i = sub; i < sub_last; ++i) {
          Wide const gdx{ prefix(sub_extra, grown, growing) };
          at[i].x = narrow((Wide{ at[i].x } + dx) + gdx);
          at[i].y = narrow(Wide{ at[i].y } + dy);
          if ((at[i].w > 0) && (at[i].h > 0)) {
            at[i].w =
                narrow((Wide{ at[i].w } + prefix(sub_extra, grown + 1, growing)) - gdx);
            at[i].h = narrow(grown_h);
            ++grown;
          }
        }
        ++sub_ord;
        sub = sub_last;
      }
      ++block_ord;
      block = block_last;
    }
    row = row_last;
  }
}

// `pack_rows`, written into `out` in place.
void fill_rows(Packing &out,
               Vector<scav_rect> const &rects,
               int32_t sep,
               int32_t dar_num,
               int32_t dar_den,
               Compaction compaction,
               bool expanded) {
  out.at.assign(rects.begin(), rects.end());
  out.w = 0;
  out.h = 0;
  if (rects.empty()) { return; }
  Vector<Spot> &spot{ spot_scratch() };
  place(rects, sep, target_width(rects, sep, dar_num, dar_den), spot);
  if (compaction == Compaction::On) { compact(rects, sep, spot, out.at); }
  Extent const e{ lay(rects, spot, sep, out.at) };
  out.w = narrow(e.w);
  out.h = narrow(e.h);
  // Expands only when neither extent saturated.
  if (expanded && (out.w == e.w) && (out.h == e.h)) { expand(spot, e, out.at); }
}

}  // namespace

SCAV_INTERNAL_BEGIN
[[maybe_unused]] Packing pack_rows(Vector<scav_rect> const &rects,
                                   int32_t sep,
                                   int32_t dar_num,
                                   int32_t dar_den,
                                   Compaction compaction,
                                   bool expanded) {
  Packing out;
  fill_rows(out, rects, sep, dar_num, dar_den, compaction, expanded);
  return out;
}
SCAV_INTERNAL_END

void pack_lr(Packing &out,
             Vector<scav_rect> const &rects,
             int32_t sep,
             int32_t dar_num,
             int32_t dar_den,
             Compaction compaction) {
  fill_rows(out, rects, sep, dar_num, dar_den, compaction, true);
}

void pack_box(Packing &out, Vector<scav_rect> const &rects, int32_t sep) {
  out.at.assign(rects.begin(), rects.end());
  out.w = 0;
  out.h = 0;
  if (rects.empty()) { return; }
  // Every rect Right of its predecessor in one subrow; expansion levels the heights.
  Vector<Spot> &spot{ spot_scratch() };
  spot.assign(rects.size(), Spot::Right);
  Extent const e{ lay(rects, spot, sep, out.at) };
  out.w = narrow(e.w);
  out.h = narrow(e.h);
  if ((out.w == e.w) && (out.h == e.h)) { expand(spot, e, out.at); }
}

bool pack_better(Packing const &a,
                 Packing const &b,
                 int32_t dar_num,
                 int32_t dar_den,
                 bool aspect_first) {
  // SM = min(dar_num / (dar_den * w), 1 / h) as a numerator and denominator; every
  // product below stays under 2^62.
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
  return b_smaller;  // a's SM is larger
}

}  // namespace scav
