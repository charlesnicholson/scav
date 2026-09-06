#ifndef SCAV_LAYOUT_ROUTER_ORTHOGONAL_H_INCLUDED
#define SCAV_LAYOUT_ROUTER_ORTHOGONAL_H_INCLUDED

// The orthogonal router's parts, each callable on its own so a test builds the
// graph it wants and asks the search one question (11).

#include "layout/router.h"

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"

#include <cstdint>
#include <vector>

namespace scav {

// One frame's orthogonal visibility graph. A vertex is an (x line, y line)
// crossing; its plane copies are `2*v` and `2*v+1`, joined by the bend (11.5).
struct OrthoGrid {
  std::vector<int32_t> xs, ys;
  std::vector<uint8_t> pass_h;  // ny rows of (nx-1): (ix,iy) -> (ix+1,iy)
  std::vector<uint8_t> pass_v;  // (ny-1) rows of nx: (ix,iy) -> (ix,iy+1)

  [[nodiscard]] uint32_t nx() const { return static_cast<uint32_t>(xs.size()); }
  [[nodiscard]] uint32_t ny() const { return static_cast<uint32_t>(ys.size()); }
  [[nodiscard]] uint32_t vertex(uint32_t ix, uint32_t iy) const {
    return (iy * nx()) + ix;
  }
  [[nodiscard]] scav_point point(uint32_t v) const {
    return { .x = xs[v % nx()], .y = ys[v / nx()] };
  }
};

// Reused across searches, so one allocates nothing once the grid settles.
// Carrying it over must not change an answer, which is its own test.
struct OrthoScratch {
  std::vector<Wide> best;
  std::vector<uint32_t> parent, stamp;
  std::vector<uint32_t> path;
  std::vector<Wide> heap_f, heap_g;
  std::vector<uint32_t> heap_node;
  uint32_t generation{ 0 };
};

// The grid is the product of its two line sets. Past this the frame degrades
// to straight lines rather than stalling; 11.5's sparse graph lifts it.
inline constexpr uint32_t ORTHO_VERTEX_BUDGET{ 1U << 18U };
inline constexpr uint32_t ORTHO_EXPANSION_BUDGET{ 1U << 20U };

// The open segment enters the rect's interior; an endpoint on a border does not,
// which is how a route leaves one box and reaches another.
bool ortho_blocks_h(scav_rect const &r, int32_t y, int32_t x0, int32_t x1);
bool ortho_blocks_v(scav_rect const &r, int32_t x, int32_t y0, int32_t y1);

// Sorted and deduplicated in place.
void ortho_sort_unique(std::vector<int32_t> &v);

// Last element not greater than `at`. Callers pass a value the vector holds.
uint32_t ortho_index_of(std::vector<int32_t> const &v, int32_t at);

// Whether an end leaves `r` by a left or right face rather than a top or bottom
// one, by how far `toward` lies outside the box on each axis (11.5).
bool ortho_escape_horizontal(scav_point toward, scav_rect const &r);

// `at` onto `r`'s border, along the one axis whose exit lands nearest
// `toward`. One axis, so the stub it leaves stays axis-aligned.
scav_point ortho_escape_box(scav_point at, scav_point toward, scav_rect const &r);

// The point on `r`'s border a route to `toward` attaches at: the same face, and
// `toward`'s own projection onto it rather than the box's centre, held `clear`
// off the face's two corners.
// `inscribed` is `RouteInput`'s: the face's midpoint and nothing else, because
// that is where an axis-aligned route meets a disc or a diamond.
scav_point ortho_attach_box(scav_point toward,
                            scav_rect const &r,
                            int32_t clear,
                            bool inscribed);

// `at` holds `2 * nets.size()` points below, src then dst per net, and only the
// ends naming a box are read or written. The three run in this order.

// An inscribed glyph's face midpoint that would hold an arrival and a departure
// together, moved a face apart. A disc or a diamond has four faces and one
// point on each, so a mixed midpoint is seatable without sliding an end off the
// glyph, which is what the spread below may not do to one. `toward` is what each
// seat was aimed at -- `ortho_attach_box`'s own argument, so `2 * nets.size()`
// points again -- and it picks which of the other axis's two faces they take.
void ortho_reface_attachments(std::vector<RouteNet> const &nets,
                              std::vector<scav_rect> const &boxes,
                              std::vector<uint8_t> const &inscribed,
                              std::vector<scav_point> const &toward,
                              std::vector<scav_point> &at);

// The two ends of one net, seated on one coordinate where that makes the net one
// straight segment: each was the other box's *centre* projected onto its own
// face, so two parallel faces a shared run apart still produce two coordinates
// and a jog between them. Skipped where phase 1 asked for a corridor.
void ortho_align_attachments(std::vector<RouteNet> const &nets,
                             std::vector<scav_rect> const &boxes,
                             std::vector<uint8_t> const &inscribed,
                             int32_t clear,
                             std::vector<scav_point> &at);

// The attachments one face still lands on one point -- two states each other's
// target project onto the same place -- pushed `clear` apart along that face and
// clamped back onto it.
void ortho_spread_attachments(std::vector<RouteNet> const &nets,
                              std::vector<scav_rect> const &boxes,
                              std::vector<uint8_t> const &inscribed,
                              int32_t clear,
                              std::vector<scav_point> &at);

// The same, off whichever box `at` is strictly inside: innermost by area, then
// by list order. Unchanged when it is inside none.
scav_point ortho_escape(scav_point at,
                        scav_point toward,
                        std::vector<scav_rect> const &boxes);

// The innermost box whose border `at` lies on, or INVALID.
uint32_t ortho_box_at(scav_point at, std::vector<scav_rect> const &boxes);

// `at`, on `r`'s border, pushed `clear` straight out; searching between rings is
// what makes the leg touching a box square. Corners resolve left, right, top.
scav_point ortho_ring(scav_point at, scav_rect const &r, int32_t clear);

// Appends, dropping repeated points and collinear middles, so a polyline
// carries only the turns it makes.
void ortho_simplify(std::vector<scav_point> const &from, std::vector<scav_point> &to);

// Lines from the region's sides, obstacles grown by `clear`, and every anchor.
// Blocks against the grown rect; `clear` of 0 is 11.5's re-seat.
bool ortho_grid(scav_rect const &region,
                std::vector<scav_rect> const &obstacles,
                std::vector<scav_point> const &anchors,
                int32_t clear,
                OrthoGrid &out);

// A* over the plane-split graph. False when unreachable or past the expansion
// budget. The key `(f, g, node)` is total, so equal-cost paths break the same.
bool ortho_search(OrthoGrid const &g,
                  uint32_t from,
                  uint32_t to,
                  Wide bend,
                  OrthoScratch &s,
                  std::vector<uint32_t> &out);

// A bend is worth one rank separation of length; the clearance is a third of
// the node separation. Named here so a test states them rather than derives.
Wide ortho_bend_penalty(scav_profile const &p);
int32_t ortho_clearance(scav_profile const &p);

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTER_ORTHOGONAL_H_INCLUDED
