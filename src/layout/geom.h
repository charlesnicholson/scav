#ifndef SCAV_LAYOUT_GEOM_H_INCLUDED
#define SCAV_LAYOUT_GEOM_H_INCLUDED

// The rect, point and kind predicates the phases share. Both containment tests
// are strict: rects touching do not overlap, and a point on a border is not
// inside.

#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_int.h"

#include <cstdint>
#include <vector>

namespace scav {

// Whether this kind's glyph is drawn inside its box rather than filling it: a
// disc for the pseudostates that carry a mark, a diamond on the face midpoints
// for a choice. A rounded rect and a bar fill the box layout gave them, so an
// axis-aligned route meets those anywhere along a face and the rest only at a
// face's midpoint.
constexpr bool kind_inscribed(StateKind kind) {
  return (kind != StateKind::Normal) && (kind != StateKind::Fork) &&
         (kind != StateKind::Join);
}

constexpr bool same(scav_point a, scav_point b) { return (a.x == b.x) && (a.y == b.y); }

constexpr bool overlaps(scav_rect const &a, scav_rect const &b) {
  return (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) && (a.y < (b.y + b.h)) &&
         (b.y < (a.y + a.h));
}

// Whether the axis-aligned segment `a`-`b` runs along one of `r`'s own edges
// for some length: on the border line, and overlapping the edge strictly, so
// a leg that only meets a corner or attaches at a point is not a run.
constexpr bool along_border(scav_point a, scav_point b, scav_rect const &r) {
  if ((r.w <= 0) || (r.h <= 0)) { return false; }
  if (a.y == b.y) {
    return ((a.y == r.y) || (a.y == (r.y + r.h))) && (imin(a.x, b.x) < (r.x + r.w)) &&
           (imax(a.x, b.x) > r.x);
  }
  if (a.x == b.x) {
    return ((a.x == r.x) || (a.x == (r.x + r.w))) && (imin(a.y, b.y) < (r.y + r.h)) &&
           (imax(a.y, b.y) > r.y);
  }
  return false;
}

constexpr bool inside(scav_point p, scav_rect const &r) {
  return (p.x > r.x) && (p.x < (r.x + r.w)) && (p.y > r.y) && (p.y < (r.y + r.h));
}

// A segment's bounding box: zero thickness when axis-aligned, so `overlaps`
// reads a run along a border as touching rather than overlapping.
constexpr scav_rect span_rect(scav_point a, scav_point b) {
  int32_t const x{ (a.x < b.x) ? a.x : b.x };
  int32_t const y{ (a.y < b.y) ? a.y : b.y };
  return { .x = x,
           .y = y,
           .w = ((a.x < b.x) ? b.x : a.x) - x,
           .h = ((a.y < b.y) ? b.y : a.y) - y };
}

// `inner` lies wholly within `outer`. Not strict, unlike `overlaps` and
// `inside`: a rect flush with its container is contained by it.
constexpr bool contains(scav_rect const &outer, scav_rect const &inner) {
  return (inner.x >= outer.x) && (inner.y >= outer.y) &&
         ((inner.x + inner.w) <= (outer.x + outer.w)) &&
         ((inner.y + inner.h) <= (outer.y + outer.h));
}

// The rect both hold, empty where they do not meet. Nested rects intersect to
// the inner one.
constexpr scav_rect intersection(scav_rect const &a, scav_rect const &b) {
  int32_t const x{ imax(a.x, b.x) };
  int32_t const y{ imax(a.y, b.y) };
  return { .x = x,
           .y = y,
           .w = imax(imin(a.x + a.w, b.x + b.w) - x, 0),
           .h = imax(imin(a.y + a.h, b.y + b.h) - y, 0) };
}

// The bumper: a box grown by the clearance a route must keep from it, so
// "no closer than `by`" becomes a containment test.
constexpr scav_rect grow(scav_rect const &r, int32_t by) {
  return { .x = r.x - by, .y = r.y - by, .w = r.w + (2 * by), .h = r.h + (2 * by) };
}

// The Chebyshev gap between two rects: the larger of the two axes'
// separations, and zero on the axis they overlap or touch on.
constexpr int32_t chebyshev_gap(scav_rect const &a, scav_rect const &b) {
  int32_t const dx{ imax(imax(b.x - (a.x + a.w), a.x - (b.x + b.w)), 0) };
  int32_t const dy{ imax(imax(b.y - (a.y + a.h), a.y - (b.y + b.h)), 0) };
  return imax(dx, dy);
}

// Rects bucketed into a uniform grid over a region, so asking whether a rect
// overlaps any of them visits only the cells it covers (11.10f). A rect and a
// query both take their cells from inclusive ranges, and the cell of a
// coordinate is monotone in it, so two rects that overlap share a cell -- a
// zero-width route piece included -- and the answer is a linear scan's.
struct RectGrid {
  int32_t x0{ 0 }, y0{ 0 };
  Wide cw{ 1 }, ch{ 1 };
  uint32_t nx{ 1 }, ny{ 1 };
  std::vector<uint32_t> off;   // nx * ny + 1, into `item`
  std::vector<uint32_t> item;  // -> the rects the grid was built over
};

// At most this many cells a side, so a long route's region and a small box do
// not allocate a cell per box height.
inline constexpr uint32_t GRID_SIDE{ 64 };

inline uint32_t grid_cell(Wide v, int32_t lo, Wide size, uint32_t n) {
  Wide const at{ floor_div(v - lo, size) };
  if (at < 0) { return 0; }
  return (at >= n) ? (n - 1) : static_cast<uint32_t>(at);
}

inline void grid_build(RectGrid &g,
                       scav_rect const &region,
                       std::vector<scav_rect> const &rects,
                       int32_t cell_w,
                       int32_t cell_h) {
  g.x0 = region.x;
  g.y0 = region.y;
  g.cw = imax(Wide{ imax(cell_w, 1) }, ceil_div(Wide{ region.w } + 1, Wide{ GRID_SIDE }));
  g.ch = imax(Wide{ imax(cell_h, 1) }, ceil_div(Wide{ region.h } + 1, Wide{ GRID_SIDE }));
  g.nx = static_cast<uint32_t>(imax(ceil_div(Wide{ region.w } + 1, g.cw), Wide{ 1 }));
  g.ny = static_cast<uint32_t>(imax(ceil_div(Wide{ region.h } + 1, g.ch), Wide{ 1 }));
  g.off.assign((static_cast<size_t>(g.nx) * g.ny) + 1, 0);
  auto const span =
      [&g](scav_rect const &r, uint32_t &c0, uint32_t &c1, uint32_t &r0, uint32_t &r1) {
        c0 = grid_cell(r.x, g.x0, g.cw, g.nx);
        c1 = grid_cell(Wide{ r.x } + r.w, g.x0, g.cw, g.nx);
        r0 = grid_cell(r.y, g.y0, g.ch, g.ny);
        r1 = grid_cell(Wide{ r.y } + r.h, g.y0, g.ch, g.ny);
      };
  for (scav_rect const &r : rects) {
    uint32_t c0{ 0 }, c1{ 0 }, r0{ 0 }, r1{ 0 };
    span(r, c0, c1, r0, r1);
    for (uint32_t y = r0; y <= r1; ++y) {
      for (uint32_t x = c0; x <= c1; ++x) {
        ++g.off[(static_cast<size_t>(y) * g.nx) + x + 1];
      }
    }
  }
  for (size_t i = 1; i < g.off.size(); ++i) { g.off[i] += g.off[i - 1]; }
  g.item.assign(g.off.back(), 0);
  std::vector<uint32_t> fill(g.off.begin(), g.off.end() - 1);
  for (uint32_t k = 0; k < rects.size(); ++k) {
    uint32_t c0{ 0 }, c1{ 0 }, r0{ 0 }, r1{ 0 };
    span(rects[k], c0, c1, r0, r1);
    for (uint32_t y = r0; y <= r1; ++y) {
      for (uint32_t x = c0; x <= c1; ++x) {
        g.item[fill[(static_cast<size_t>(y) * g.nx) + x]++] = k;
      }
    }
  }
}

inline bool grid_hits(RectGrid const &g,
                      std::vector<scav_rect> const &rects,
                      scav_rect const &cand) {
  uint32_t const c0{ grid_cell(cand.x, g.x0, g.cw, g.nx) };
  uint32_t const c1{ grid_cell(Wide{ cand.x } + cand.w, g.x0, g.cw, g.nx) };
  uint32_t const r0{ grid_cell(cand.y, g.y0, g.ch, g.ny) };
  uint32_t const r1{ grid_cell(Wide{ cand.y } + cand.h, g.y0, g.ch, g.ny) };
  for (uint32_t y = r0; y <= r1; ++y) {
    for (uint32_t x = c0; x <= c1; ++x) {
      size_t const cell{ (static_cast<size_t>(y) * g.nx) + x };
      for (uint32_t k = g.off[cell]; k < g.off[cell + 1]; ++k) {
        if (overlaps(cand, rects[g.item[k]])) { return true; }
      }
    }
  }
  return false;
}

}  // namespace scav

#endif  // SCAV_LAYOUT_GEOM_H_INCLUDED
