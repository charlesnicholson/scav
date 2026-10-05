#ifndef SCAV_LAYOUT_ROUTER_ORTHOGONAL_H_INCLUDED
#define SCAV_LAYOUT_ROUTER_ORTHOGONAL_H_INCLUDED

// The orthogonal router's parts, each callable on its own for tests.

#include "layout/router.h"

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

// One frame's orthogonal visibility graph. A vertex is an (x line, y line) crossing; its
// horizontal and vertical plane copies are `2*v` and `2*v+1`, joined by a bend edge.
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

// One search node's state, live only while `stamp` is the scratch's generation.
struct OrthoNodeState {
  uint32_t stamp;
  uint32_t parent;
  Wide best;
};

// One open-list entry, ordered by `f`, then `g`, then node.
struct OrthoFrontierEntry {
  Wide f;
  Wide g;
  uint32_t node;
};

// Radix heap on `f`, popping in (`f`, `g`, node) order. `ties` heaps the entries at
// `last`; `bucket[b]` holds keys whose highest bit differing from `last` is `b`.
struct OrthoRadixHeap {
  std::array<std::vector<OrthoFrontierEntry>, 64> bucket;
  std::vector<OrthoFrontierEntry> ties;
  std::vector<OrthoFrontierEntry> spill;  // scratch for re-placing entries
  uint64_t full{ 0 };                     // bit b set while bucket[b] holds an entry
  uint64_t last{ 0 };                     // the least key: `f`, sign bit flipped
};

// Search scratch; allocates nothing once the grid size settles. Results are independent
// of what it held.
struct OrthoScratch {
  std::vector<OrthoNodeState> state;
  std::vector<uint32_t> path;
  OrthoRadixHeap open;
  uint32_t generation{ 0 };
};

void ortho_open_clear(OrthoRadixHeap &h);
void ortho_open_push(OrthoRadixHeap &h, OrthoFrontierEntry const &e);
// Removes and returns the least entry by `f`, then `g`, then node; `h` is nonempty.
OrthoFrontierEntry ortho_open_pop(OrthoRadixHeap &h);
[[nodiscard]] bool ortho_open_empty(OrthoRadixHeap const &h);

// Cap on grid vertices, the product of the two line counts; past it the frame degrades
// to straight lines.
inline constexpr uint32_t ORTHO_VERTEX_BUDGET{ 1U << 18U };
inline constexpr uint32_t ORTHO_EXPANSION_BUDGET{ 1U << 20U };

// True when the open segment meets the rect's open interior.
bool ortho_blocks_h(scav_rect const &r, int32_t y, int32_t x0, int32_t x1);
bool ortho_blocks_v(scav_rect const &r, int32_t x, int32_t y0, int32_t y1);

// Sorted and deduplicated in place.
void ortho_sort_unique(std::vector<int32_t> &v);

// Index of the last element not greater than `at`; callers pass a value `v` holds.
uint32_t ortho_index_of(std::vector<int32_t> const &v, int32_t at);

// True when an end leaves `r` by its left or right face: `toward` lies at least as far
// outside `r` on x as on y.
bool ortho_escape_horizontal(scav_point toward, scav_rect const &r);

// `at` moved along one axis onto `r`'s border: x when `ortho_escape_horizontal`, else y,
// to the side nearer `toward`, ties to left or top.
scav_point ortho_escape_box(scav_point at, scav_point toward, scav_rect const &r);

// `ortho_attach_box` on a named face: 0 left, 1 right, 2 top, 3 bottom. A non-inscribed
// face of length at most twice its corner inset falls back to `ortho_attach_box`.
scav_point ortho_attach_face(scav_point toward,
                             scav_rect const &r,
                             int32_t clear,
                             bool inscribed,
                             int32_t corner,
                             uint32_t face);

// The seat on the face `ortho_escape_box` picks, at `toward`'s projection held
// `max(clear, corner)` off each corner, at most mid-face; inscribed: the face midpoint.
scav_point ortho_attach_box(scav_point toward,
                            scav_rect const &r,
                            int32_t clear,
                            bool inscribed,
                            int32_t corner);

// One net end, slot `2 * net + end`: its box (out of range for none), and that box's
// inscribed flag and corner arc.
struct Seat {
  uint32_t box, end, slot;
  bool inscribed;
  int32_t arc;
};

// The seat of every net end, in slot order; `inscribed` and `corner` are `RouteInput`'s.
void ortho_seats(std::vector<RouteNet> const &nets,
                 std::vector<uint8_t> const &inscribed,
                 std::vector<int32_t> const &corner,
                 std::vector<Seat> &out);

// The seat passes, run in this order. `at` holds a seat per slot and only box ends move;
// `table` is `ortho_seats`' output.

// Where an inscribed glyph's face midpoint holds arrivals and departures, moves one
// direction to another face's midpoint; `toward`, an aim per slot, picks which and where.
void ortho_reface_attachments(std::vector<scav_rect> const &boxes,
                              std::vector<Seat> const &table,
                              std::vector<scav_point> const &toward,
                              std::vector<scav_point> &at);

// Puts both ends of a corridor-free net between parallel faces of two boxes on one
// coordinate: the face centres' midpoint clamped to the shared run, its low end if `lean`.
void ortho_align_attachments(std::vector<RouteNet> const &nets,
                             std::vector<scav_rect> const &boxes,
                             std::vector<Seat> const &table,
                             std::vector<scav_point> &at);

// Spreads seats sharing a point on one face by `max(clear, pitch)` where the seatable run
// fits it, else `min(clear, len / 3)`; a port leg level with its `toward` stays.
void ortho_spread_attachments(std::vector<scav_rect> const &boxes,
                              std::vector<Seat> const &table,
                              std::vector<scav_point> const &toward,
                              int32_t clear,
                              int32_t pitch,
                              std::vector<scav_point> &at);

// Pushes apart, along their faces, legs on different boxes running alongside closer than
// `pitch`; skips nets sharing a box, and a port leg level with its `toward` stays.
void ortho_separate_attachments(std::vector<RouteNet> const &nets,
                                std::vector<scav_rect> const &boxes,
                                std::vector<Seat> const &table,
                                std::vector<scav_point> const &toward,
                                int32_t clear,
                                int32_t pitch,
                                std::vector<scav_point> &at);

// Seats a self-loop with both ends on one face in that face's widest run free of other
// seats, point ends and occupied spans, `pitch` apart about its middle where that fits.
void ortho_seat_loops(std::vector<RouteNet> const &nets,
                      std::vector<scav_rect> const &boxes,
                      std::vector<Seat> const &table,
                      std::vector<OccupiedSpan> const &occupied,
                      int32_t clear,
                      int32_t pitch,
                      std::vector<scav_point> &at);

// Moves each seat inside an occupied span to its face's nearest free position, else to the
// free position nearest its aim; `stuck` counts per net the seats left inside.
void ortho_clear_occupied(std::vector<RouteNet> const &nets,
                          std::vector<scav_rect> const &boxes,
                          std::vector<Seat> const &table,
                          std::vector<scav_point> const &toward,
                          std::vector<OccupiedSpan> const &occupied,
                          int32_t clear,
                          std::vector<scav_point> &at,
                          std::vector<int32_t> &stuck);

// `ortho_escape_box` off the smallest-area box strictly containing `at`, ties to the
// lower index; `at` unchanged when inside none.
scav_point ortho_escape(scav_point at,
                        scav_point toward,
                        std::vector<scav_rect> const &boxes);

// The smallest-area box whose border `at` lies on, or INVALID.
uint32_t ortho_box_at(scav_point at, std::vector<scav_rect> const &boxes);

// `at`, on `r`'s border, pushed `clear` straight out; unchanged off the border. Corners
// resolve left, right, top.
scav_point ortho_ring(scav_point at, scav_rect const &r, int32_t clear);

// Appends `from` to `to`, dropping repeated points and collinear middles.
void ortho_simplify(std::vector<scav_point> const &from, std::vector<scav_point> &to);

// Lines at the region's sides, obstacle sides grown by `clear`, `fixed` sides and anchors;
// grown obstacles and `fixed` rects block. False past `ORTHO_VERTEX_BUDGET`.
bool ortho_grid(scav_rect const &region,
                std::vector<scav_rect> const &obstacles,
                std::vector<scav_point> const &anchors,
                int32_t clear,
                OrthoGrid &out,
                std::vector<scav_rect> const *fixed = nullptr);

// Writes to `out` up to four rects covering `region` outside `enclosure` shrunk by
// `inset`; none for a zero-sized enclosure.
void ortho_enclosure_walls(scav_rect const &region,
                           scav_rect const &enclosure,
                           int32_t inset,
                           std::vector<scav_rect> &out);

// A* over the plane-split graph keyed (`f`, `g`, node); false when unreachable or over
// `ORTHO_EXPANSION_BUDGET`. `from_plane`/`to_plane` 0 or 1 fix that end's plane.
bool ortho_search(OrthoGrid const &g,
                  uint32_t from,
                  uint32_t to,
                  Wide bend,
                  OrthoScratch &s,
                  std::vector<uint32_t> &out,
                  uint32_t from_plane = INVALID,
                  uint32_t to_plane = INVALID);

// A bend costs one rank separation of length; the clearance a route keeps from a box is
// `box_clearance`.
Wide ortho_bend_penalty(scav_profile const &p);
int32_t ortho_clearance(scav_profile const &p);

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTER_ORTHOGONAL_H_INCLUDED
