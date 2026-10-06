#ifndef SCAV_LAYOUT_GEOM_H_INCLUDED
#define SCAV_LAYOUT_GEOM_H_INCLUDED

// Rect, point and kind predicates shared by the layout stages. `overlaps` and `inside`
// are strict: touching rects do not overlap, and a point on a border is outside.

#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_int.h"
#include "scav_vec.h"

#include <cstdint>
#include <vector>

namespace scav {

// True when the glyph is a disc or diamond inscribed in its box; a route meets it at a
// face midpoint. Normal, Fork and Join fill the box and meet a route anywhere on a face.
constexpr bool kind_inscribed(StateKind kind) {
  return (kind != StateKind::Normal) && (kind != StateKind::Fork) &&
         (kind != StateKind::Join);
}

constexpr bool same(scav_point a, scav_point b) { return (a.x == b.x) && (a.y == b.y); }

// Direction code `3 * x + y`, each axis 0 falling, 1 still, 2 rising; a bend changes it.
constexpr uint32_t direction(scav_point a, scav_point b) {
  auto const axis = [](int32_t from, int32_t to) {
    if (to > from) { return 2U; }
    return (to < from) ? 0U : 1U;
  };
  return (axis(a.x, b.x) * 3U) + axis(a.y, b.y);
}

constexpr bool overlaps(scav_rect const &a, scav_rect const &b) {
  return (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) && (a.y < (b.y + b.h)) &&
         (b.y < (a.y + a.h));
}

// True when axis-aligned segment `a`-`b` lies within `near` of a border line of `r` and
// overlaps that edge for a positive length.
constexpr bool along_border(scav_point a,
                            scav_point b,
                            scav_rect const &r,
                            int32_t near = 0) {
  if ((r.w <= 0) || (r.h <= 0)) { return false; }
  auto const on = [near](int32_t at, int32_t line) {
    return (imax(at, line) - imin(at, line)) <= near;
  };
  if (a.y == b.y) {
    return (on(a.y, r.y) || on(a.y, r.y + r.h)) && (imin(a.x, b.x) < (r.x + r.w)) &&
           (imax(a.x, b.x) > r.x);
  }
  if (a.x == b.x) {
    return (on(a.x, r.x) || on(a.x, r.x + r.w)) && (imin(a.y, b.y) < (r.y + r.h)) &&
           (imax(a.y, b.y) > r.y);
  }
  return false;
}

constexpr bool inside(scav_point p, scav_rect const &r) {
  return (p.x > r.x) && (p.x < (r.x + r.w)) && (p.y > r.y) && (p.y < (r.y + r.h));
}

// A segment's bounding box, zero-thick when axis-aligned: `overlaps` counts a run along a
// border as touching.
constexpr scav_rect span_rect(scav_point a, scav_point b) {
  int32_t const x{ (a.x < b.x) ? a.x : b.x };
  int32_t const y{ (a.y < b.y) ? a.y : b.y };
  return { .x = x,
           .y = y,
           .w = ((a.x < b.x) ? b.x : a.x) - x,
           .h = ((a.y < b.y) ? b.y : a.y) - y };
}

// True when `inner` lies within `outer`, shared borders included.
constexpr bool contains(scav_rect const &outer, scav_rect const &inner) {
  return (inner.x >= outer.x) && (inner.y >= outer.y) &&
         ((inner.x + inner.w) <= (outer.x + outer.w)) &&
         ((inner.y + inner.h) <= (outer.y + outer.h));
}

// The rect `a` and `b` share; zero width or height where they are disjoint.
constexpr scav_rect intersection(scav_rect const &a, scav_rect const &b) {
  int32_t const x{ imax(a.x, b.x) };
  int32_t const y{ imax(a.y, b.y) };
  return { .x = x,
           .y = y,
           .w = imax(imin(a.x + a.w, b.x + b.w) - x, 0),
           .h = imax(imin(a.y + a.h, b.y + b.h) - y, 0) };
}

// `r` grown by `by` on every side: a box's bumper at clearance `by`.
constexpr scav_rect grow(scav_rect const &r, int32_t by) {
  return { .x = r.x - by, .y = r.y - by, .w = r.w + (2 * by), .h = r.h + (2 * by) };
}

// Overlap length of axis-aligned segments `a`-`b` and `c`-`d` when collinear, else 0.
constexpr Wide shared_run(scav_point a, scav_point b, scav_point c, scav_point d) {
  bool const flat{ (a.y == b.y) && (c.y == d.y) && (a.y == c.y) };
  bool const upright{ (a.x == b.x) && (c.x == d.x) && (a.x == c.x) };
  if (!(flat || upright)) { return 0; }
  Wide const alo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
  Wide const ahi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
  Wide const clo{ flat ? imin(c.x, d.x) : imin(c.y, d.y) };
  Wide const chi{ flat ? imax(c.x, d.x) : imax(c.y, d.y) };
  return imax(Wide{ 0 }, imin(ahi, chi) - imax(alo, clo));
}

// The larger of the x and y gaps between `a` and `b`; 0 on an axis where they meet.
constexpr int32_t chebyshev_gap(scav_rect const &a, scav_rect const &b) {
  int32_t const dx{ imax(imax(b.x - (a.x + a.w), a.x - (b.x + b.w)), 0) };
  int32_t const dy{ imax(imax(b.y - (a.y + a.h), a.y - (b.y + b.h)), 0) };
  return imax(dx, dy);
}

// Rects bucketed in a uniform grid over a region; a query visits only the cells it covers
// and finds every rect a linear scan would, zero-width rects included.
struct RectGrid {
  int32_t x0{ 0 }, y0{ 0 };
  Wide cw{ 1 }, ch{ 1 };
  uint32_t nx{ 1 }, ny{ 1 };
  std::vector<uint32_t> off;    // nx * ny + 1, into `item`
  std::vector<uint32_t> item;   // indices of the rects the grid was built over
  std::vector<uint32_t> stamp;  // per rect, the last query that visited it
  uint32_t epoch{ 0 };          // the current query
};

// Maximum cells per grid side; cells grow past `cell_w` and `cell_h` to stay within it.
inline constexpr uint32_t GRID_SIDE{ 64 };

inline uint32_t grid_cell(Wide v, int32_t lo, Wide size, uint32_t n) {
  Wide const at{ floor_div(v - lo, size) };
  if (at < 0) { return 0; }
  return (at >= Wide{ n }) ? (n - 1) : static_cast<uint32_t>(at);
}

// Buckets `rects` over `region` in cells at least `cell_w` by `cell_h`; `cursor` is
// caller-owned scratch.
inline void grid_build(RectGrid &g,
                       scav_rect const &region,
                       std::vector<scav_rect> const &rects,
                       int32_t cell_w,
                       int32_t cell_h,
                       std::vector<uint32_t> &cursor) {
  g.x0 = region.x;
  g.y0 = region.y;
  g.cw = imax(Wide{ imax(cell_w, 1) }, ceil_div(Wide{ region.w } + 1, Wide{ GRID_SIDE }));
  g.ch = imax(Wide{ imax(cell_h, 1) }, ceil_div(Wide{ region.h } + 1, Wide{ GRID_SIDE }));
  g.nx = static_cast<uint32_t>(imax(ceil_div(Wide{ region.w } + 1, g.cw), Wide{ 1 }));
  g.ny = static_cast<uint32_t>(imax(ceil_div(Wide{ region.h } + 1, g.ch), Wide{ 1 }));
  vec_assign(g.off, (static_cast<size_t>(g.nx) * g.ny) + 1, 0);
  auto const spread = [&g, &rects](auto step) {
    for (uint32_t k = 0; k < rects.size(); ++k) {
      scav_rect const &r{ rects[k] };
      uint32_t const c0{ grid_cell(r.x, g.x0, g.cw, g.nx) };
      uint32_t const c1{ grid_cell(Wide{ r.x } + r.w, g.x0, g.cw, g.nx) };
      uint32_t const r0{ grid_cell(r.y, g.y0, g.ch, g.ny) };
      uint32_t const r1{ grid_cell(Wide{ r.y } + r.h, g.y0, g.ch, g.ny) };
      for (uint32_t y = r0; y <= r1; ++y) {
        for (uint32_t x = c0; x <= c1; ++x) {
          step((static_cast<size_t>(y) * g.nx) + x, k);
        }
      }
    }
  };
  spread([&g](size_t cell, uint32_t) { ++g.off[cell + 1]; });
  for (size_t i = 1; i < g.off.size(); ++i) { g.off[i] += g.off[i - 1]; }
  vec_resize(g.item, g.off.back());
  vec_assign(cursor, g.off.begin(), g.off.end() - 1);
  spread([&g, &cursor](size_t cell, uint32_t k) { g.item[cursor[cell]++] = k; });
  vec_assign(g.stamp, rects.size(), 0);
  g.epoch = 0;
}

// Calls `visit` once per rect in the cells of `q` grown by `margin`, every rect within
// `margin` of `q` included, until it returns true; returns whether it did.
template <typename Visit>
bool grid_visit(RectGrid &g, scav_rect const &q, Wide margin, Visit visit) {
  ++g.epoch;
  uint32_t const c0{ grid_cell(Wide{ q.x } - margin, g.x0, g.cw, g.nx) };
  uint32_t const c1{ grid_cell(Wide{ q.x } + q.w + margin, g.x0, g.cw, g.nx) };
  uint32_t const r0{ grid_cell(Wide{ q.y } - margin, g.y0, g.ch, g.ny) };
  uint32_t const r1{ grid_cell(Wide{ q.y } + q.h + margin, g.y0, g.ch, g.ny) };
  for (uint32_t y = r0; y <= r1; ++y) {
    for (uint32_t x = c0; x <= c1; ++x) {
      size_t const cell{ (static_cast<size_t>(y) * g.nx) + x };
      for (uint32_t k = g.off[cell]; k < g.off[cell + 1]; ++k) {
        uint32_t const item{ g.item[k] };
        if (g.stamp[item] == g.epoch) { continue; }
        g.stamp[item] = g.epoch;
        if (visit(item)) { return true; }
      }
    }
  }
  return false;
}

}  // namespace scav

#endif  // SCAV_LAYOUT_GEOM_H_INCLUDED
