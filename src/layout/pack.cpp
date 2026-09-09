// LR-rectpacking: a target width from the desired aspect ratio, every rect
// placed at one of four positions relative to its predecessor, compaction
// where the knob asks for it, then whitespace elimination. A row holds blocks left to
// right, a block holds subrows top to bottom, and a subrow holds rects left to right, so
// reading order is the placement order by construction and the structure the last step
// fills is the structure the placement built.

#include "layout/pack.h"

#include "scav_int.h"
#include "scav_internal.h"

#include <cstdint>
#include <vector>

namespace scav {

// Bracketed with whitespace elimination switchable as well as compaction, so a
// test can measure either against what it started from; nothing shipping
// passes false. Prototyped here because gcc's -Werror=missing-declarations
// refuses a namespace-scope definition with no declaration above it, which is
// what an internal function is under SCAV_TESTING. The prototype a test uses
// is its own; see scav_internal.h.
SCAV_INTERNAL_BEGIN
Packing pack_rows(std::vector<scav_rect> const &rects,
                  int32_t sep,
                  int32_t dar_num,
                  int32_t dar_den,
                  Compaction compaction,
                  bool expanded);
SCAV_INTERNAL_END

namespace {

// The whole of the placement's freedom, and the reason order-preserving and
// gap-avoiding are one constraint: a rect goes right of its predecessor, into
// the next subrow of its block, into a new block at the row's top level, or
// into a new row. Every structure is therefore a cut of the input sequence.
enum class Spot : uint8_t { Right = 0, Subrow = 1, Level = 2, Row = 3 };
constexpr uint8_t SPOTS{ 4 };

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

// A packing's two extents before they are narrowed, so two candidates that
// both ran past the domain still compare as the sums they are.
struct Extent {
  Wide w{ 0 }, h{ 0 };
};

// Every distance here is non-negative, so one bound is the whole clamp.
int32_t narrow(Wide v) { return static_cast<int32_t>(imin(v, Wide{ PACK_SATURATED })); }

// Where the area below stops. It only picks a target width that is then
// clamped to the domain, and at 2^48 the floor-sqrt already clears COORD_MAX
// for every ratio a profile allows (dar_num >= 1, dar_den <= 1024), so the cap
// changes no target and keeps `area * dar_num` inside int64.
constexpr Wide AREA_MAX{ Wide{ 1 } << 48 };

Wide target_width(std::vector<scav_rect> const &rects,
                  int32_t sep,
                  int32_t dar_num,
                  int32_t dar_den) {
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
  return imin(imax(widest, approx), Wide{ COORD_MAX });
}

// The cursor sitting on the first rect, which takes the origin and no choice.
Cursor seeded(scav_rect const &first, int32_t sep) {
  Cursor at;
  at.row_h = first.h;
  at.row_right = first.w;
  at.sub_h = first.h;
  at.sub_x = Wide{ first.w } + sep;
  return at;
}

// Advances the cursor onto `spot` and answers the rect's left edge; its top is
// the cursor's `sub_y` afterwards. Each case leaves `sub_x` on that left edge
// and `sub_y` on the subrow it settled into, which the common tail then reads.
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

// The geometry a spot list means. `at` arrives holding every rect's extents
// and leaves holding its position too, so laying a candidate out costs one
// pass and no allocation.
Extent lay(std::vector<scav_rect> const &rects,
           std::vector<Spot> const &spot,
           int32_t sep,
           std::vector<scav_rect> &at) {
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

// Greedy placement: the first of the four positions that keeps the row inside
// the target. Row level beats carrying on rightward from a lower subrow, which
// would leave a notch above that nothing short of growing a neighbour fills.
void place(std::vector<scav_rect> const &rects,
           int32_t sep,
           Wide target,
           std::vector<Spot> &spot) {
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

// Smaller area, then shorter, then narrower: a total order on two extents,
// which is all the choice below has to separate.
bool tighter(Extent const &a, Extent const &b) {
  Wide const a_area{ a.w * a.h };
  Wide const b_area{ b.w * b.h };
  if (a_area != b_area) { return a_area < b_area; }
  if (a.h != b.h) { return a.h < b.h; }
  return a.w < b.w;
}

// Compaction. Each rect gets one chance, in index order, to take one of the
// other three positions -- back left into its predecessor's subrow, or up into
// the hole an earlier row left the late arrival that could not use it. A move
// is kept only where neither extent grew and one shrank, so occupancy rises,
// the scale measure rises, and a packing inside the domain stays inside it.
void compact(std::vector<scav_rect> const &rects,
             int32_t sep,
             std::vector<Spot> &spot,
             std::vector<scav_rect> &at) {
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
      // The rect's own edges bound the drawing's, so a candidate putting one of
      // them past the current extent cannot shrink either. Asking here rather
      // than laying the whole packing out is what keeps compaction linear on a
      // packing it has nothing to move.
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

// The `i`th of `parts` shares of `extra`. Floor-apportioned from the running
// prefix, so the shares sum to exactly `extra`, no two differ by more than
// one, and the prefix is the shift the `i`th thing owes the ones before it.
Wide prefix(Wide extra, uint32_t i, uint32_t parts) {
  return (parts == 0) ? Wide{ 0 } : floor_div(extra * i, Wide{ parts });
}

// Where a run of consecutive rects at one nesting level ends: the next rect
// whose spot opens something at `level` or coarser. The four spots are ordered
// finest first, so one comparison reads all three levels.
uint32_t run_end(std::vector<Spot> const &spot, uint32_t at, uint32_t end, Spot level) {
  uint32_t next{ at + 1 };
  while ((next < end) && (spot[next] < level)) { ++next; }
  return next;
}

// Whitespace elimination. Every row is grown to the drawing's width, every
// block to its row's height, every subrow to its block's width, and every rect
// to its subrow's height, sharing the leftover along each axis so that the gap
// between two neighbours is still exactly the gap the placement gave them. A
// rect with no extent is left alone and takes no share: growing an empty
// submachine into a visible band would invent contents it does not have.
void expand(std::vector<Spot> const &spot,
            Extent const &whole,
            std::vector<scav_rect> &at) {
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

}  // namespace

SCAV_INTERNAL_BEGIN
Packing pack_rows(std::vector<scav_rect> const &rects,
                  int32_t sep,
                  int32_t dar_num,
                  int32_t dar_den,
                  Compaction compaction,
                  bool expanded) {
  Packing out;
  out.at = rects;
  if (rects.empty()) { return out; }
  std::vector<Spot> spot;
  place(rects, sep, target_width(rects, sep, dar_num, dar_den), spot);
  if (compaction == Compaction::On) { compact(rects, sep, spot, out.at); }
  Extent const e{ lay(rects, spot, sep, out.at) };
  out.w = narrow(e.w);
  out.h = narrow(e.h);
  // A packing past the domain is no candidate (11.2), so there is nothing in
  // it worth filling, and the extents its arithmetic would divide are sums
  // that already left int32.
  if (expanded && (out.w == e.w) && (out.h == e.h)) { expand(spot, e, out.at); }
  return out;
}
SCAV_INTERNAL_END

Packing pack_lr(std::vector<scav_rect> const &rects,
                int32_t sep,
                int32_t dar_num,
                int32_t dar_den,
                Compaction compaction) {
  return pack_rows(rects, sep, dar_num, dar_den, compaction, true);
}

Packing pack_box(std::vector<scav_rect> const &rects, int32_t sep) {
  Packing out;
  out.at = rects;
  if (rects.empty()) { return out; }
  // One row, one block, one subrow, which is the spot list of every rect
  // following its predecessor. Expansion then levels their heights.
  std::vector<Spot> const spot(rects.size(), Spot::Right);
  Extent const e{ lay(rects, spot, sep, out.at) };
  out.w = narrow(e.w);
  out.h = narrow(e.h);
  if ((out.w == e.w) && (out.h == e.h)) { expand(spot, e, out.at); }
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
