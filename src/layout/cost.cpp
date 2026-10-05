// Scores the rects and polylines the phases produced. Every predicate is degree 2 (sign
// tests of `orient2d`, no constructed point) and fits in int64.

#include "layout/cost.h"

#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_stable_sort.h"
#include "scav_vec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace scav {

// Linkable from tests: the containment walk, the grid, two Tier 0 counts and the three
// sweeps. Tests declare their own prototypes; see scav_internal.h.
SCAV_INTERNAL_BEGIN
Ancestry cost_flatten_ancestry(Chart const &c);
bool cost_ancestor(Chart const &c, Ancestry const &an, StateId ancestor, StateId of);
ChildGrid cost_child_grid(Chart const &c, SizedLayout const &z);
void cost_grid_query(ChildGrid const &g,
                     uint32_t frame,
                     scav_rect const &q,
                     GridQuery &out);
int32_t cost_box_overlaps(Chart const &c, SizedLayout const &z, ChildGrid const &g);
int32_t cost_through_boxes(Chart const &c,
                           SizedLayout const &z,
                           Ancestry const &an,
                           ChildGrid const &g,
                           std::vector<Piece> const &pieces);
int64_t cost_crossings(std::vector<Piece> const &pieces, std::vector<uint32_t> &per_trans);
Wide cost_corridor(Routes const &r, std::vector<Piece> const &pieces);
Wide cost_crowding(std::vector<Piece> const &pieces, int32_t em);
// The fewest axis changes from box `s` through `via` in order to box `t`; a face below 4
// is left square outward at `s` or entered square inward at `t`.
int32_t cost_path_turns(scav_rect const &s,
                        uint32_t s_face,
                        std::vector<scav_point> const &via,
                        scav_rect const &t,
                        uint32_t t_face);
SCAV_INTERNAL_END

namespace {

// Cap on cells per axis; a grid holds at most its square.
constexpr uint32_t GRID_SIDE_MAX{ 64 };

// A frame of at most this many children is scanned and has no grid cells.
constexpr uint32_t SCAN_MAX{ 16 };

Wide orient2d(scav_point a, scav_point b, scav_point c) {
  return ((Wide{ b.x } - a.x) * (Wide{ c.y } - a.y)) -
         ((Wide{ b.y } - a.y) * (Wide{ c.x } - a.x));
}

// True for a proper crossing; false for a shared endpoint, a touch or a collinear overlap.
bool crosses(scav_point a, scav_point b, scav_point c, scav_point d) {
  Wide const d1{ orient2d(a, b, c) };
  Wide const d2{ orient2d(a, b, d) };
  Wide const d3{ orient2d(c, d, a) };
  Wide const d4{ orient2d(c, d, b) };
  if ((d1 == 0) || (d2 == 0) || (d3 == 0) || (d4 == 0)) { return false; }
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// The segment enters the rect's interior: either end inside, or it cuts one
// of the four sides.
bool enters(scav_point a, scav_point b, scav_rect const &r) {
  if (inside(a, r) || inside(b, r)) { return true; }
  scav_point const tl{ .x = r.x, .y = r.y };
  scav_point const tr{ .x = r.x + r.w, .y = r.y };
  scav_point const bl{ .x = r.x, .y = r.y + r.h };
  scav_point const br{ .x = r.x + r.w, .y = r.y + r.h };
  return crosses(a, b, tl, tr) || crosses(a, b, bl, br) || crosses(a, b, tl, bl) ||
         crosses(a, b, tr, br);
}

// Touching or at most `sep` apart on one axis while overlapping on the other.
bool adjacent(scav_rect const &a, scav_rect const &b, int32_t sep) {
  bool const x_over{ (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) };
  bool const y_over{ (a.y < (b.y + b.h)) && (b.y < (a.y + a.h)) };
  int32_t const x_gap{ (a.x < b.x) ? (b.x - (a.x + a.w)) : (a.x - (b.x + b.w)) };
  int32_t const y_gap{ (a.y < b.y) ? (b.y - (a.y + a.h)) : (a.y - (b.y + b.h)) };
  return (y_over && (x_gap <= sep)) || (x_over && (y_gap <= sep));
}

Wide length_of(scav_point a, scav_point b) {
  Wide const dx{ Wide{ b.x } - a.x };
  Wide const dy{ Wide{ b.y } - a.y };
  if (dx == 0) { return (dy < 0) ? -dy : dy; }
  if (dy == 0) { return (dx < 0) ? -dx : dx; }
  return static_cast<Wide>(isqrt(static_cast<uint64_t>((dx * dx) + (dy * dy))));
}

// Which line a piece can share a run along: 0 the horizontal at `at`, 1 the vertical at
// it, 2 for a degenerate or diagonal piece.
uint32_t piece_axis(Piece const &p, int32_t &at) {
  if ((p.a.y == p.b.y) && (p.a.x != p.b.x)) {
    at = p.a.y;
    return 0;
  }
  if ((p.a.x == p.b.x) && (p.a.y != p.b.y)) {
    at = p.a.x;
    return 1;
  }
  return 2;
}

// Two routes' shared tail and head.
struct Trunk {
  uint32_t tail{ 0 };         // identical final points
  bool merged_tail{ false };  // the legs into those points share a run
  uint32_t head{ 0 };         // identical first points
  bool merged_head{ false };  // the legs out of those points share a run
};

constexpr uint32_t TRUNK_TAIL{ 2 };  // `corridor`'s cap on `Trunk::tail`: the final leg

// `a` and `b`'s common tail of at most `cap` points and, where `heads`, their common head.
Trunk trunk_of(std::vector<scav_point> const &pts,
               scav_span a,
               scav_span b,
               uint32_t cap,
               bool heads) {
  Trunk out;
  uint32_t const shortest{ imin(imin(a.len, b.len), cap) };
  while ((out.tail < shortest) &&
         same(pts[(a.off + a.len - 1) - out.tail], pts[(b.off + b.len - 1) - out.tail])) {
    ++out.tail;
  }
  if ((out.tail > 0) && (out.tail < a.len) && (out.tail < b.len)) {
    uint32_t const i{ (a.off + a.len - 1) - out.tail };
    uint32_t const j{ (b.off + b.len - 1) - out.tail };
    out.merged_tail = shared_run(pts[i], pts[i + 1], pts[j], pts[j + 1]) > 0;
  }
  if (!heads) { return out; }
  while ((out.head < imin(a.len, b.len)) &&
         same(pts[a.off + out.head], pts[b.off + out.head])) {
    ++out.head;
  }
  if ((out.head > 0) && (out.head < a.len) && (out.head < b.len)) {
    uint32_t const i{ (a.off + out.head) - 1 };
    uint32_t const j{ (b.off + out.head) - 1 };
    out.merged_head = shared_run(pts[i], pts[i + 1], pts[j], pts[j + 1]) > 0;
  }
  return out;
}

// Whether segment `k` of a `len`-point route lies in `t`: inside the shared tail or head,
// or the leg into or out of it when merged.
bool trunk_piece(Trunk const &t, uint32_t len, uint32_t k) {
  return ((k + t.tail) >= len) || (t.merged_tail && ((k + t.tail + 1) == len)) ||
         ((k + 1) < t.head) || (t.merged_head && ((k + 1) == t.head));
}

// Direction code `3 * x + y`, each axis 0 falling, 1 still, 2 rising; a bend changes it.
uint32_t direction(scav_point a, scav_point b) {
  auto const axis = [](int32_t from, int32_t to) {
    if (to > from) { return 2U; }
    return (to < from) ? 0U : 1U;
  };
  return (axis(a.x, b.x) * 3U) + axis(a.y, b.y);
}

// Cells per axis for `n` children, about one child per cell.
uint32_t grid_side(uint32_t n) {
  return imin(static_cast<uint32_t>(isqrt(n)) + 1U, GRID_SIDE_MAX);
}

// The cell a coordinate falls in, clamped to [0, `side`).
uint32_t cell_of(Wide at, int32_t origin, Wide size, uint32_t side) {
  Wide const i{ floor_div(at - origin, size) };
  return static_cast<uint32_t>(imax(Wide{ 0 }, imin(i, Wide{ side } - 1)));
}

// True when every geometry column covers its entities and every route span lies within
// `r.points`.
bool geometry_complete(Chart const &c, SizedLayout const &z, Routes const &r) {
  if ((z.state.size() < c.states.size()) || (z.before.size() < c.states.size()) ||
      (z.after.size() < c.states.size()) || (z.sub.size() < c.submachines.size()) ||
      (r.route.size() < c.transitions.size())) {
    return false;
  }
  for (scav_span const &span : r.route) {
    if ((Wide{ span.off } + span.len) > static_cast<Wide>(r.points.size())) {
      return false;
    }
  }
  return true;
}

// An axial piece: `axis` and `at` from `piece_axis`, `lo`..`hi` its extent along the line.
struct Lane {
  uint32_t axis;
  int32_t at;
  int32_t lo, hi;
  uint32_t piece;
};

// Ascending LSD radix sort of distinct keys, one counting scatter per byte via `spare`; a
// byte every key shares skips its pass.
void sort_keys(std::vector<uint64_t> &key, std::vector<uint64_t> &spare) {
  size_t const n{ key.size() };
  if (n <= SCAV_SORT_SMALL) {
    scav_insertion_sort(key.data(), key.data() + n, [](uint64_t x, uint64_t y) {
      return x < y;
    });
    return;
  }
  constexpr uint32_t DIGITS{ 8 };
  constexpr uint32_t RADIX{ 256 };
  std::array<std::array<uint32_t, RADIX>, DIGITS> count{};
  for (uint64_t const k : key) {
    for (uint32_t d = 0; d < DIGITS; ++d) { ++count[d][(k >> (8U * d)) & 0xFFU]; }
  }
  vec_resize(spare, n);
  uint64_t *src{ key.data() };
  uint64_t *dst{ spare.data() };
  for (uint32_t d = 0; d < DIGITS; ++d) {
    std::array<uint32_t, RADIX> &at{ count[d] };
    if (at[(src[0] >> (8U * d)) & 0xFFU] == n) { continue; }
    uint32_t sum{ 0 };
    for (uint32_t &slot : at) {
      uint32_t const here{ slot };
      slot = sum;
      sum += here;
    }
    for (size_t i = 0; i < n; ++i) {
      uint64_t const k{ src[i] };
      dst[at[(k >> (8U * d)) & 0xFFU]++] = k;
    }
    std::swap(src, dst);
  }
  if (src != key.data()) { std::memcpy(key.data(), src, n * sizeof(uint64_t)); }
}

// Every axial piece, horizontals first, sorted by (axis, coordinate, piece) packed
// into one integer each. `key` and `spare` are the sort's buffers.
void lanes_of(std::vector<Piece> const &pieces,
              std::vector<uint64_t> &key,
              std::vector<uint64_t> &spare,
              std::vector<Lane> &lanes) {
  constexpr uint32_t PIECE_BITS{ 31 };
  key.clear();
  for (uint32_t i = 0; i < pieces.size(); ++i) {
    int32_t at{ 0 };
    uint64_t const axis{ piece_axis(pieces[i], at) };
    if (axis >= 2) { continue; }
    uint64_t const line{ static_cast<uint32_t>(at) ^ 0x8000'0000U };
    vec_push_back(key, (axis << 63U) | (line << PIECE_BITS) | i);
  }
  sort_keys(key, spare);
  lanes.clear();
  for (uint64_t const k : key) {
    uint32_t const i{ static_cast<uint32_t>(k & ((uint64_t{ 1 } << PIECE_BITS) - 1)) };
    Piece const &pc{ pieces[i] };
    uint32_t const axis{ static_cast<uint32_t>(k >> 63U) };
    int32_t const a{ (axis == 0) ? pc.a.x : pc.a.y };
    int32_t const b{ (axis == 0) ? pc.b.x : pc.b.y };
    vec_push_back(lanes,
                  { .axis = axis,
                    .at = (axis == 0) ? pc.a.y : pc.a.x,
                    .lo = imin(a, b),
                    .hi = imax(a, b),
                    .piece = i });
  }
}

[[maybe_unused]] std::vector<Lane> lanes_of(std::vector<Piece> const &pieces) {
  std::vector<uint64_t> key;
  std::vector<uint64_t> spare;
  std::vector<Lane> lanes;
  lanes_of(pieces, key, spare, lanes);
  return lanes;
}

// Each vertical against the band of horizontals strictly inside its span; a diagonal goes
// against everything.
int64_t crossings_over(std::vector<Piece> const &pieces,
                       std::vector<Lane> const &lanes,
                       std::vector<uint32_t> &per_trans,
                       std::vector<uint8_t> &is_loose) {
  uint32_t flat{ 0 };
  while ((flat < lanes.size()) && (lanes[flat].axis == 0)) { ++flat; }
  vec_assign(is_loose, pieces.size(), 1);
  for (Lane const &lane : lanes) { is_loose[lane.piece] = 0; }

  int64_t total{ 0 };
  auto const charge = [&](uint32_t i, uint32_t j) {
    ++total;
    ++per_trans[pieces[i].trans];
    ++per_trans[pieces[j].trans];
  };

  for (uint32_t u = flat; u < lanes.size(); ++u) {
    Lane const &lv{ lanes[u] };
    // First horizontal with `at > lo`; the band is open at both ends.
    uint32_t low{ 0 };
    uint32_t high{ flat };
    while (low < high) {
      uint32_t const mid{ low + ((high - low) >> 1U) };
      if (lanes[mid].at <= lv.lo) {
        low = mid + 1;
      } else {
        high = mid;
      }
    }
    // In the band, a crossing is the vertical's x strictly inside the horizontal's extent.
    for (uint32_t at = low; (at < flat) && (lanes[at].at < lv.hi); ++at) {
      Lane const &lh{ lanes[at] };
      if (pieces[lh.piece].trans == pieces[lv.piece].trans) { continue; }
      if ((lh.lo < lv.at) && (lv.at < lh.hi)) { charge(lv.piece, lh.piece); }
    }
  }

  for (uint32_t d = 0; d < pieces.size(); ++d) {
    if (is_loose[d] == 0) { continue; }
    for (uint32_t j = 0; j < pieces.size(); ++j) {
      // A loose pair is charged once, from its lower index; `j == d` is skipped.
      if ((is_loose[j] != 0) && (j <= d)) { continue; }
      if (pieces[d].trans == pieces[j].trans) { continue; }
      if (crosses(pieces[d].a, pieces[d].b, pieces[j].a, pieces[j].b)) { charge(d, j); }
    }
  }
  return total;
}

bool on_rect(scav_point at, scav_rect const &r) {
  return (at.x >= r.x) && (at.x <= (r.x + r.w)) && (at.y >= r.y) && (at.y <= (r.y + r.h));
}

// Whether `at` lies on state `st`'s border or on the inner edge of a band its request
// rules.
bool on_drawn_edge(SizedLayout const &z,
                   scav_spaces const &s,
                   uint32_t st,
                   scav_point at) {
  scav_rect const box{ z.state[st] };
  if (on_rect(at, box) && ((at.x == box.x) || (at.x == (box.x + box.w)) ||
                           (at.y == box.y) || (at.y == (box.y + box.h)))) {
    return true;
  }
  uint32_t const ruled{ ((s.box_state != nullptr) && (st < s.n_box_state))
                            ? s.box_state[st].ruled
                            : 0U };
  auto const row = [st](std::vector<scav_rect> const &v) {
    return (st < v.size()) ? v[st] : scav_rect{};
  };
  scav_rect const b{ row(z.before) };
  scav_rect const a{ row(z.after) };
  scav_rect const l{ row(z.lead) };
  scav_rect const t{ row(z.trail) };
  // Each band's inner edge, in `ruled` bit order.
  std::array<scav_rect, 4> const edge{ { { .x = b.x, .y = b.y + b.h, .w = b.w, .h = 0 },
                                         { .x = a.x, .y = a.y, .w = a.w, .h = 0 },
                                         { .x = l.x + l.w, .y = l.y, .w = 0, .h = l.h },
                                         { .x = t.x, .y = t.y, .w = 0, .h = t.h } } };
  std::array<int32_t, 4> const depth{ b.h, a.h, l.w, t.w };
  for (uint32_t k = 0; k < 4; ++k) {
    if ((((ruled >> k) & 1U) != 0) && (depth[k] > 0) && on_rect(at, edge[k])) {
      return true;
    }
  }
  return false;
}

// Marks both transitions of a charged pair in `party`, where given.
void blame(std::vector<uint8_t> *party, uint32_t a, uint32_t b) {
  if (party == nullptr) { return; }
  if (a < party->size()) { (*party)[a] = 1; }
  if (b < party->size()) { (*party)[b] = 1; }
}

// The length each pair shares on one line, per `(axis, coordinate)` bucket, less the
// trunks; adds to `runs`, where given, each pair outside the routes' common head and tail.
Wide corridor_over(Routes const &r,
                   std::vector<Piece> const &pieces,
                   std::vector<Lane> const &lanes,
                   std::vector<uint8_t> *party,
                   int32_t *runs) {
  Wide total{ 0 };
  for (uint32_t lo = 0; lo < lanes.size();) {
    uint32_t hi{ lo };
    while ((hi < lanes.size()) && (lanes[hi].axis == lanes[lo].axis) &&
           (lanes[hi].at == lanes[lo].at)) {
      ++hi;
    }
    for (uint32_t i = lo; i < hi; ++i) {
      Piece const &u{ pieces[lanes[i].piece] };
      for (uint32_t j = i + 1; j < hi; ++j) {
        Piece const &v{ pieces[lanes[j].piece] };
        if (u.trans == v.trans) { continue; }
        Wide const shared{ shared_run(u.a, u.b, v.a, v.b) };
        if (shared <= 0) { continue; }
        scav_span const ru{ r.route[u.trans] };
        scav_span const rv{ r.route[v.trans] };
        if (runs != nullptr) {
          Trunk const fan{ trunk_of(r.points, ru, rv, ru.len, true) };
          if (!trunk_piece(fan, ru.len, u.k) || !trunk_piece(fan, rv.len, v.k)) {
            ++*runs;
          }
        }
        Trunk const pair{ trunk_of(r.points, ru, rv, TRUNK_TAIL, false) };
        if (trunk_piece(pair, ru.len, u.k) && trunk_piece(pair, rv.len, v.k)) { continue; }
        total += shared;
        blame(party, u.trans, v.trans);
      }
    }
    lo = hi;
  }
  return total;
}

// Each pair of lanes of different transitions on one axis, overlapping and under an em
// apart but not collinear: overlap times shortfall, summed, then divided once by `em`.
Wide crowding_over(std::vector<Piece> const &pieces,
                   std::vector<Lane> const &lanes,
                   int32_t em,
                   std::vector<uint8_t> *party) {
  if (em <= 0) { return 0; }
  Wide scaled{ 0 };
  for (uint32_t i = 0; i < lanes.size(); ++i) {
    Lane const &u{ lanes[i] };
    for (uint32_t j = i + 1; j < lanes.size(); ++j) {
      Lane const &v{ lanes[j] };
      if (v.axis != u.axis) { break; }
      Wide const apart{ Wide{ v.at } - u.at };
      if (apart >= em) { break; }    // sorted, so every later lane is further
      if (apart == 0) { continue; }  // on one line: charged by `corridor`
      if (pieces[u.piece].trans == pieces[v.piece].trans) { continue; }
      Wide const along{ imin(Wide{ u.hi }, Wide{ v.hi }) -
                        imax(Wide{ u.lo }, Wide{ v.lo }) };
      if (along <= 0) { continue; }
      scaled += along * (Wide{ em } - apart);
      blame(party, pieces[u.piece].trans, pieces[v.piece].trans);
    }
  }
  return scaled / em;
}

// The child grid without cell bounds: each frame's live children below `known` in span
// order; cells only for a frame of more than `scan_max` children.
void child_grid_frames(Chart const &c, size_t known, uint32_t scan_max, ChildGrid &g) {
  vec_assign(g.frame, c.submachines.size(), ChildGrid::Frame{});
  g.child.clear();
  uint32_t cells{ 0 };
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    ChildGrid::Frame &f{ g.frame[m] };
    uint32_t const first{ static_cast<uint32_t>(g.child.size()) };
    Span const kids{ c.submachines[m].children };
    for (uint32_t i = 0; i < kids.len; ++i) {
      uint32_t const st{ c.state_ids[kids.off + i].v };
      if ((st >= known) || (st >= c.states.size()) || (c.states[st].live == 0)) {
        continue;
      }
      vec_push_back(g.child, st);
    }
    f.children = make_span(first, static_cast<uint32_t>(g.child.size()) - first);
    if (f.children.len <= scan_max) { continue; }
    f.side = grid_side(f.children.len);
    f.bucket = cells;
    cells += f.side * f.side;
  }
  vec_assign(g.bucket_off, cells + 1, 0);
  g.bucket_at.clear();
}

void child_rects(ChildGrid const &g, SizedLayout const &z, std::vector<scav_rect> &kid) {
  vec_resize(kid, g.child.size());
  for (size_t i = 0; i < g.child.size(); ++i) { kid[i] = z.state[g.child[i]]; }
}

// Lays each gridded frame's cells over its children's bounds and buckets the children;
// `kid` is `child_rects`' output and `cursor` the placing pass's buffer.
void child_grid_fill(ChildGrid &g,
                     std::vector<scav_rect> const &kid,
                     std::vector<uint32_t> &cursor) {
  for (ChildGrid::Frame &f : g.frame) {
    if (f.side == 0) { continue; }
    scav_rect const r0{ kid[f.children.off] };
    Wide x0{ r0.x };
    Wide y0{ r0.y };
    Wide x1{ Wide{ r0.x } + r0.w };
    Wide y1{ Wide{ r0.y } + r0.h };
    for (uint32_t i = 1; i < f.children.len; ++i) {
      scav_rect const r{ kid[f.children.off + i] };
      x0 = imin(x0, Wide{ r.x });
      y0 = imin(y0, Wide{ r.y });
      x1 = imax(x1, Wide{ r.x } + r.w);
      y1 = imax(y1, Wide{ r.y } + r.h);
    }
    f.x0 = static_cast<int32_t>(x0);
    f.y0 = static_cast<int32_t>(y0);
    f.cell_w = imax(Wide{ 1 }, ceil_div(x1 - x0, Wide{ f.side }));
    f.cell_h = imax(Wide{ 1 }, ceil_div(y1 - y0, Wide{ f.side }));
  }

  // Counting sort into cells: count, prefix-sum, then place through a cursor copy; a child
  // spanning several cells is stored in each.
  vec_assign(g.bucket_off, g.bucket_off.size(), 0);
  auto const spread = [&g, &kid](auto step) {
    for (ChildGrid::Frame const &f : g.frame) {
      if (f.side == 0) { continue; }
      for (uint32_t i = 0; i < f.children.len; ++i) {
        scav_rect const r{ kid[f.children.off + i] };
        uint32_t const cx0{ cell_of(r.x, f.x0, f.cell_w, f.side) };
        uint32_t const cx1{ cell_of(Wide{ r.x } + r.w, f.x0, f.cell_w, f.side) };
        uint32_t const cy0{ cell_of(r.y, f.y0, f.cell_h, f.side) };
        uint32_t const cy1{ cell_of(Wide{ r.y } + r.h, f.y0, f.cell_h, f.side) };
        for (uint32_t cy = cy0; cy <= cy1; ++cy) {
          for (uint32_t cx = cx0; cx <= cx1; ++cx) {
            step(f.bucket + (cy * f.side) + cx, f.children.off + i);
          }
        }
      }
    }
  };
  spread([&g](uint32_t cell, uint32_t) { ++g.bucket_off[cell + 1]; });
  for (uint32_t i = 1; i < g.bucket_off.size(); ++i) {
    g.bucket_off[i] += g.bucket_off[i - 1];
  }
  vec_assign(g.bucket_at, g.bucket_off.back(), 0);
  vec_assign(cursor, g.bucket_off.begin(), g.bucket_off.end());
  spread([&g, &cursor](uint32_t cell, uint32_t child) {
    g.bucket_at[cursor[cell]] = child;
    ++cursor[cell];
  });
}

// Whether `state` lies inside region `m`; the climb stops after one step per state.
bool within(Chart const &c, StateId state, uint32_t m) {
  StateId at{ state };
  for (size_t step = 0; (step < c.states.size()) && (at.v < c.states.size()); ++step) {
    SubmachineId const up{ c.states[at.v].parent };
    if (up.v == m) { return true; }
    if (up.v >= c.submachines.size()) { return false; }
    at = c.submachines[up.v].owner;
  }
  return false;
}

// Region `m`'s share of its owner: its rect grown across to the owner's box and along to
// the dividers with its live sibling regions; its rect where it has no live sibling.
scav_rect region_cell(Chart const &c, SizedLayout const &z, uint32_t m) {
  scav_rect const &r{ z.sub[m] };
  uint32_t const owner{ c.submachines[m].owner.v };
  if (owner >= z.state.size()) { return r; }
  scav_rect const &b{ z.state[owner] };
  int32_t x0{ r.x };
  int32_t y0{ r.y };
  int32_t x1{ r.x + r.w };
  int32_t y1{ r.y + r.h };
  bool stacked{ false };  // a sibling above or below
  bool beside{ false };   // a sibling left or right
  Span const subs{ c.states[owner].submachines };
  for (uint32_t k = 0; k < subs.len; ++k) {
    uint32_t const o{ c.submachine_ids[subs.off + k].v };
    if ((o == m) || (o >= c.submachines.size()) || (c.submachines[o].live == 0)) {
      continue;
    }
    scav_rect const &q{ z.sub[o] };
    if ((q.y + q.h) <= r.y) {
      y0 = imin(y0, (q.y + q.h) + floor_div(r.y - (q.y + q.h), 2));
      stacked = true;
    } else if (q.y >= (r.y + r.h)) {
      y1 = imax(y1, (r.y + r.h) + floor_div(q.y - (r.y + r.h), 2));
      stacked = true;
    } else if ((q.x + q.w) <= r.x) {
      x0 = imin(x0, (q.x + q.w) + floor_div(r.x - (q.x + q.w), 2));
      beside = true;
    } else if (q.x >= (r.x + r.w)) {
      x1 = imax(x1, (r.x + r.w) + floor_div(q.x - (r.x + r.w), 2));
      beside = true;
    }
  }
  if (stacked) {
    x0 = imin(x0, b.x);
    x1 = imax(x1, b.x + b.w);
  }
  if (beside) {
    y0 = imin(y0, b.y);
    y1 = imax(y1, b.y + b.h);
  }
  return { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
}

// Whether bend `at` lies in a state `tr` only passes through. Each end climbs to `top`;
// the first state holding `at` is transit unless it is the end or its enclosing state.
bool in_transit(Chart const &c,
                SizedLayout const &z,
                Transition const &tr,
                std::array<uint32_t, 2> const &top,
                scav_point at) {
  std::array<StateId, 2> const ends{ tr.src, tr.dst };
  for (uint32_t i = 0; i < 2; ++i) {
    if (top[i] == INVALID) { continue; }
    StateId const own{ enclosing_state(c, ends[i]) };
    StateId st{ ends[i] };
    for (size_t step = 0; (step < c.states.size()) && (st.v != INVALID); ++step) {
      if (inside(at, z.state[st.v])) {
        if ((st != ends[i]) && (st != own)) { return true; }
        break;
      }
      if (st.v == top[i]) { break; }
      st = enclosing_state(c, st);
    }
  }
  return false;
}

Wide area_of(scav_rect const &r) { return Wide{ r.w } * r.h; }

// Per live composite, the hole between its bands inside its padding less its live
// children's rects and its loop room, floored at zero; the sum capped at `chart`.
Wide whitespace_of(Chart const &c, SizedLayout const &z, Wide chart) {
  Wide total{ 0 };
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if (c.states[st].live == 0) { continue; }
    bool composite{ false };
    Wide held{ 0 };
    Span const subs{ c.states[st].submachines };
    for (uint32_t k = 0; k < subs.len; ++k) {
      uint32_t const sub{ c.submachine_ids[subs.off + k].v };
      if ((sub >= c.submachines.size()) || (c.submachines[sub].live == 0)) { continue; }
      composite = true;
      Span const kids{ c.submachines[sub].children };
      for (uint32_t i = 0; i < kids.len; ++i) {
        uint32_t const kid{ c.state_ids[kids.off + i].v };
        if ((kid < c.states.size()) && (c.states[kid].live != 0)) {
          held += area_of(z.state[kid]);
        }
      }
    }
    if (!composite) { continue; }
    scav_rect const &r{ z.state[st] };
    scav_rect const &b{ z.before[st] };
    Wide const pad{ Wide{ b.y } - r.y };
    Wide const h{ Wide{ r.h } - (2 * pad) - b.h - z.after[st].h };
    Wide const w{ Wide{ b.w } - ((st < z.lead.size()) ? z.lead[st].w : 0) -
                  ((st < z.trail.size()) ? z.trail[st].w : 0) };
    if (st < z.loop.size()) { held += area_of(z.loop[st]); }
    total += imax((imax(w, Wide{ 0 }) * imax(h, Wide{ 0 })) - held, Wide{ 0 });
  }
  return imin(total, chart);
}

// A grid over the rects' bounds, about one cell per rect.
void grid_over(RectGrid &g,
               std::vector<scav_rect> const &rects,
               std::vector<uint32_t> &cursor) {
  Wide x0{ 0 };
  Wide y0{ 0 };
  Wide x1{ 0 };
  Wide y1{ 0 };
  for (uint32_t i = 0; i < rects.size(); ++i) {
    scav_rect const &r{ rects[i] };
    x0 = (i == 0) ? r.x : imin(x0, Wide{ r.x });
    y0 = (i == 0) ? r.y : imin(y0, Wide{ r.y });
    x1 = (i == 0) ? (Wide{ r.x } + r.w) : imax(x1, Wide{ r.x } + r.w);
    y1 = (i == 0) ? (Wide{ r.y } + r.h) : imax(y1, Wide{ r.y } + r.h);
  }
  scav_rect const bounds{ .x = static_cast<int32_t>(x0),
                          .y = static_cast<int32_t>(y0),
                          .w = static_cast<int32_t>(imax(x1 - x0, Wide{ 0 })),
                          .h = static_cast<int32_t>(imax(y1 - y0, Wide{ 0 })) };
  Wide const side{ grid_side(static_cast<uint32_t>(rects.size())) };
  grid_build(g,
             bounds,
             rects,
             static_cast<int32_t>(ceil_div(Wide{ bounds.w } + 1, side)),
             static_cast<int32_t>(ceil_div(Wide{ bounds.h } + 1, side)),
             cursor);
}

// `cost_box_overlaps` over `kid` from `child_rects`, with `q` the query buffer.
int32_t box_overlaps_over(Chart const &c,
                          ChildGrid const &g,
                          std::vector<scav_rect> const &kid,
                          GridQuery &q) {
  int32_t total{ 0 };
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    Span const kids{ g.frame[m].children };
    if (kids.len <= SCAN_MAX) {
      for (uint32_t i = 0; i < kids.len; ++i) {
        scav_rect const &a{ kid[kids.off + i] };
        for (uint32_t j = i + 1; j < kids.len; ++j) {
          if (overlaps(a, kid[kids.off + j])) { ++total; }
        }
      }
      continue;
    }
    for (uint32_t i = 0; i < kids.len; ++i) {
      scav_rect const &a{ kid[kids.off + i] };
      cost_grid_query(g, m, a, q);
      for (uint32_t const at : q.hit) {
        // A pair is charged once, from its lower span index.
        if (at <= (kids.off + i)) { continue; }
        if (overlaps(a, kid[at])) { ++total; }
      }
    }
  }
  return total;
}

// What the Tier 0 descent reuses from one piece to the next.
struct Descent {
  std::vector<uint32_t> roots, stack;
  GridQuery q;
};

// `cost_through_boxes` over `kid`, as `box_overlaps_over` takes it. `*regions` counts each
// piece entering a live region of a reached or detached state that neither end lies in:
// its `region_cell`, or its rect for a transition with an end at the region's owner.
int32_t through_boxes_over(Chart const &c,
                           SizedLayout const &z,
                           Ancestry const &an,
                           ChildGrid const &g,
                           std::vector<scav_rect> const &kid,
                           std::vector<Piece> const &pieces,
                           Descent &d,
                           int32_t *bands,
                           int32_t *regions) {
  d.roots.clear();
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].owner.v == INVALID) { vec_push_back(d.roots, m); }
  }

  int32_t total{ 0 };
  for (Piece const &piece : pieces) {
    Transition const &tr{ c.transitions[piece.trans] };
    scav_rect const reach{ span_rect(piece.a, piece.b) };
    // Entering a state other than an endpoint or an endpoint's ancestor is charged.
    auto const charge = [&](uint32_t st, scav_rect const &box) {
      if (!cost_ancestor(c, an, { st }, tr.src) && !cost_ancestor(c, an, { st }, tr.dst)) {
        if (enters(piece.a, piece.b, box)) { ++total; }
        return;
      }
      // Inside a state it may occupy, each band it enters adds to `*bands`.
      if ((bands == nullptr) || (st >= z.before.size())) { return; }
      std::array<scav_rect, 5> const walls{ state_walls(z, st) };
      for (uint32_t k = 0; k < 4; ++k) {
        scav_rect const &r{ walls[k] };
        if ((r.w > 0) && (r.h > 0) && enters(piece.a, piece.b, r)) { ++*bands; }
      }
    };
    bool foreign{ false };  // the piece entered a region neither end lies in
    // Tests `st`'s regions and, with `descend`, queues them as frames.
    auto const open = [&](uint32_t st, bool descend) {
      bool src_side{ false };  // entered a region holding the source alone
      bool dst_side{ false };  // entered a region holding the target alone
      Span const subs{ c.states[st].submachines };
      for (uint32_t i = 0; i < subs.len; ++i) {
        uint32_t const m{ c.submachine_ids[subs.off + i].v };
        if (descend) { vec_push_back(d.stack, m); }
        if ((regions == nullptr) || foreign || (m >= c.submachines.size()) ||
            (c.submachines[m].live == 0)) {
          continue;
        }
        bool const own{ (tr.src.v == st) || (tr.dst.v == st) };
        scav_rect const cell{ own ? z.sub[m] : region_cell(c, z, m) };
        bool const in{ overlaps(reach, cell) && enters(piece.a, piece.b, cell) };
        bool const has_src{ within(c, tr.src, m) };
        bool const has_dst{ within(c, tr.dst, m) };
        foreign = in && !has_src && !has_dst;
        src_side = src_side || (in && has_src && !has_dst);
        dst_side = dst_side || (in && has_dst && !has_src);
      }
      // An external route's piece entering both its ends' regions crosses their divider.
      foreign = foreign || ((tr.kind == TransKind::External) && src_side && dst_side);
    };
    for (uint32_t const st : an.detached) {
      charge(st, z.state[st]);
      open(st, false);
    }

    auto const visit = [&](uint32_t at) {
      if (!overlaps(reach, kid[at])) { return; }
      uint32_t const st{ g.child[at] };
      charge(st, kid[at]);
      open(st, true);
    };
    vec_assign(d.stack, d.roots.begin(), d.roots.end());
    while (!d.stack.empty()) {
      uint32_t const frame{ d.stack.back() };
      d.stack.pop_back();
      if (frame >= g.frame.size()) { continue; }
      Span const kids{ g.frame[frame].children };
      if (kids.len <= SCAN_MAX) {
        for (uint32_t at = kids.off; at < (kids.off + kids.len); ++at) { visit(at); }
        continue;
      }
      cost_grid_query(g, frame, reach, d.q);
      for (uint32_t const at : d.q.hit) { visit(at); }
    }
    if (foreign) { ++*regions; }
  }
  return total;
}

// Every buffer one `cost_terms` call uses, per thread. `cost_terms` never waits on the
// pool, so one call per thread runs at a time.
struct Scratch {
  std::vector<Piece> pieces;
  std::vector<uint32_t> first, crossings_of;
  std::vector<uint64_t> key, spare;
  std::vector<Lane> lanes;
  std::vector<uint8_t> is_loose;
  std::vector<Wide> carried;
  std::vector<scav_rect> state_box, state_rect;  // grown, and as placed
  std::vector<uint32_t> state_of;
  std::vector<scav_rect> seg_box;
  RectGrid states, segs, placed;
  std::vector<uint32_t> cursor;  // every grid build's placing pass
  std::vector<uint8_t> encloses;
  std::vector<uint32_t> common;
  ChildGrid grid;  // `CostContext::grid`, filled for the candidate
  std::vector<scav_rect> kid;
  Descent descent;
};

Scratch &scratch() {
  thread_local Scratch s;
  return s;
}

int32_t tier0_of(CostTerms const &t) {
  int32_t sum{ 0 };
  for (int32_t const n : tier0_terms(t)) { sum += n; }
  return sum;
}

// Divider ports whose two submachines are not adjacent; a fork or join end is exempt.
int64_t adjacency_of(Chart const &c,
                     SplitGraph const &g,
                     SizedLayout const &z,
                     scav_profile const &p) {
  int64_t out{ 0 };
  for (SplitPort const &port : g.ports) {
    if ((port.sub.v == INVALID) || (port.into.v == INVALID)) { continue; }
    Transition const &tr{ c.transitions[port.trans.v] };
    StateKind const src_kind{ c.states[tr.src.v].kind };
    StateKind const dst_kind{ c.states[tr.dst.v].kind };
    if ((src_kind == StateKind::Fork) || (src_kind == StateKind::Join) ||
        (dst_kind == StateKind::Fork) || (dst_kind == StateKind::Join)) {
      continue;
    }
    if (!adjacent(z.sub[port.sub.v], z.sub[port.into.v], p.sub_sep)) { ++out; }
  }
  return out;
}

// A closed axis-aligned region: x in [x0, x1], y in [y0, y1].
struct Region {
  Wide x0, x1, y0, y1;
};

Region region_of(scav_rect const &r) {
  return { .x0 = r.x, .x1 = Wide{ r.x } + r.w, .y0 = r.y, .y1 = Wide{ r.y } + r.h };
}

Region region_at(scav_point at) {
  return { .x0 = at.x, .x1 = at.x, .y0 = at.y, .y1 = at.y };
}

bool meets(Region const &a, Region const &b) {
  return (a.x0 <= b.x1) && (b.x0 <= a.x1) && (a.y0 <= b.y1) && (b.y0 <= a.y1);
}

// A path's first or last piece: its axis (0 horizontal, 1 vertical, 2 any) and the sign of
// its travel along it (0 either).
struct Leg {
  uint32_t axis{ 2 };
  int32_t sign{ 0 };
};

constexpr Leg ANY_LEG{};

// Whether `n` nonzero moves along one axis take some coordinate of [a0, a1] to some of
// [b0, b1], the first with sign `first` and the last with sign `last` (0 either).
bool axis_reaches(Wide a0,
                  Wide a1,
                  Wide b0,
                  Wide b1,
                  uint32_t n,
                  int32_t first,
                  int32_t last) {
  if (n == 0) { return (a0 <= b1) && (b0 <= a1); }
  if ((n == 1) && (first != 0) && (last != 0) && (first != last)) { return false; }
  int32_t const sign{ (first != 0) ? first : last };
  if ((n == 1) || ((n == 2) && (first != 0) && (first == last))) {
    if (sign > 0) { return b1 > a0; }
    if (sign < 0) { return b0 < a1; }
    return (a0 != a1) || (b0 != b1) || (a0 != b0);
  }
  return true;
}

constexpr int32_t FAR_TURNS{ 1 << 20 };  // exceeds any count of turns

std::vector<uint32_t> const NO_BENDS;

// Per arrival axis (0 horizontal, 1 vertical), the fewest axis changes from `a` to `b`
// after arriving at `a` along `in` (2 for none); `first`, `last` constrain the end pieces.
std::array<int32_t, 2> leg_turns(Region const &a,
                                 Region const &b,
                                 uint32_t in,
                                 Leg first,
                                 Leg last) {
  std::array<int32_t, 2> out{ FAR_TURNS, FAR_TURNS };
  if ((first.axis == 2) && (last.axis == 2) && meets(a, b)) {
    if (in == 2) {
      out = { 0, 0 };
    } else {
      out[in] = 0;
    }
  }
  for (uint32_t axis = 0; axis < 2; ++axis) {
    if ((first.axis != 2) && (first.axis != axis)) { continue; }
    for (uint32_t pieces = 1; pieces <= 6; ++pieces) {
      uint32_t const end{ ((pieces % 2) == 1) ? axis : (1 - axis) };
      if ((last.axis != 2) && (last.axis != end)) { continue; }
      uint32_t const along_x{ (axis == 0) ? ((pieces + 1) / 2) : (pieces / 2) };
      int32_t const x_first{ ((axis == 0) && (first.axis == 0)) ? first.sign : 0 };
      int32_t const x_last{ ((end == 0) && (last.axis == 0)) ? last.sign : 0 };
      int32_t const y_first{ ((axis == 1) && (first.axis == 1)) ? first.sign : 0 };
      int32_t const y_last{ ((end == 1) && (last.axis == 1)) ? last.sign : 0 };
      if (!axis_reaches(a.x0, a.x1, b.x0, b.x1, along_x, x_first, x_last) ||
          !axis_reaches(a.y0, a.y1, b.y0, b.y1, pieces - along_x, y_first, y_last)) {
        continue;
      }
      int32_t const turns{ static_cast<int32_t>(pieces - 1) +
                           (((in != 2) && (in != axis)) ? 1 : 0) };
      out[end] = imin(out[end], turns);
    }
  }
  if ((out[0] == FAR_TURNS) && (out[1] == FAR_TURNS)) { out = { 5, 5 }; }
  return out;
}

// The fewest axis changes from `s` through `via` in order to `t`; a face below 4 is left
// square outward or entered square inward, its region then that face.
int32_t path_turns(Region s,
                   uint32_t s_face,
                   std::vector<scav_point> const &via,
                   Region t,
                   uint32_t t_face) {
  auto const leg_of = [](uint32_t face, int32_t sign) {
    return Leg{ .axis = (face < 2) ? 0U : 1U, .sign = ((face % 2) == 0) ? -sign : sign };
  };
  std::array<int32_t, 2> turns{ 0, 0 };
  Region at{ s };
  auto const n{ static_cast<uint32_t>(via.size()) };
  for (uint32_t k = 0; k <= n; ++k) {
    Region const to{ (k < n) ? region_at(via[k]) : t };
    Leg const first{ ((k == 0) && (s_face < 4)) ? leg_of(s_face, 1) : ANY_LEG };
    Leg const last{ ((k == n) && (t_face < 4)) ? leg_of(t_face, -1) : ANY_LEG };
    if (k == 0) {
      turns = leg_turns(at, to, 2, first, last);
    } else {
      std::array<int32_t, 2> next{ FAR_TURNS, FAR_TURNS };
      for (uint32_t axis = 0; axis < 2; ++axis) {
        std::array<int32_t, 2> const step{ leg_turns(at, to, axis, first, last) };
        for (uint32_t end = 0; end < 2; ++end) {
          next[end] = imin(next[end], turns[axis] + step[end]);
        }
      }
      turns = next;
    }
    at = to;
  }
  return imin(turns[0], turns[1]);
}

// Box `r`, narrowed to face `face` when that is below 4.
Region face_region(scav_rect const &r, uint32_t face) {
  Region out{ region_of(r) };
  switch (face) {
    case 0: out.x1 = out.x0; break;
    case 1: out.x0 = out.x1; break;
    case 2: out.y1 = out.y0; break;
    case 3: out.y0 = out.y1; break;
    default: break;
  }
  return out;
}

}  // namespace

SCAV_INTERNAL_BEGIN

Ancestry cost_flatten_ancestry(Chart const &c) {
  Ancestry out;
  vec_assign(out.tin, c.states.size(), 0);
  vec_assign(out.tout, c.states.size(), 0);
  std::vector<uint8_t> buried(c.states.size(), 0);

  // `open` 0 marks an exit, pushed under the state's children; it writes `tout` after the
  // subtree.
  struct Visit {
    uint32_t state, open, buried;
  };
  std::vector<Visit> stack;
  auto const push_children = [&c, &stack](uint32_t sub, uint32_t under) {
    Span const kids{ c.submachines[sub].children };
    for (uint32_t i = kids.len; i-- > 0;) {
      vec_push_back(stack,
                    { .state = c.state_ids[kids.off + i].v, .open = 1, .buried = under });
    }
  };
  // The forest's roots are each ownerless submachine's children, pushed in reverse to pop
  // in ordinal order.
  uint32_t const frames{ static_cast<uint32_t>(c.submachines.size()) };
  for (uint32_t m = frames; m-- > 0;) {
    if (c.submachines[m].owner.v == INVALID) { push_children(m, 0); }
  }

  uint32_t clock{ 0 };
  while (!stack.empty()) {
    Visit const at{ stack.back() };
    stack.pop_back();
    if (at.open == 0) {
      out.tout[at.state] = clock;
      continue;
    }
    if ((at.state >= out.tin.size()) || (out.tin[at.state] != 0)) { continue; }
    ++clock;
    out.tin[at.state] = clock;
    buried[at.state] = static_cast<uint8_t>(at.buried);
    vec_push_back(stack, { .state = at.state, .open = 0, .buried = 0 });
    uint32_t const under{ ((at.buried != 0) || (c.states[at.state].live == 0)) ? 1U : 0U };
    Span const subs{ c.states[at.state].submachines };
    for (uint32_t i = subs.len; i-- > 0;) {
      push_children(c.submachine_ids[subs.off + i].v, under);
    }
  }

  for (uint32_t s = 0; s < c.states.size(); ++s) {
    if ((c.states[s].live != 0) && ((out.tin[s] == 0) || (buried[s] != 0))) {
      vec_push_back(out.detached, s);
    }
  }
  return out;
}

bool cost_ancestor(Chart const &c, Ancestry const &an, StateId ancestor, StateId of) {
  if ((ancestor.v >= an.tin.size()) || (of.v >= an.tin.size())) { return false; }
  if (ancestor == of) { return true; }
  // A state off the walk falls back to `ancestor_or_self`.
  if ((an.tin[ancestor.v] == 0) || (an.tin[of.v] == 0)) {
    return ancestor_or_self(c, ancestor, of);
  }
  return (an.tin[ancestor.v] <= an.tin[of.v]) && (an.tout[of.v] <= an.tout[ancestor.v]);
}

// This and every `[[maybe_unused]]` entry below are called only by tests.
[[maybe_unused]] ChildGrid cost_child_grid(Chart const &c, SizedLayout const &z) {
  ChildGrid g;
  child_grid_frames(c, z.state.size(), 0, g);
  std::vector<scav_rect> kid;
  std::vector<uint32_t> cursor;
  child_rects(g, z, kid);
  child_grid_fill(g, kid, cursor);
  return g;
}

void cost_grid_query(ChildGrid const &g,
                     uint32_t frame,
                     scav_rect const &q,
                     GridQuery &out) {
  out.hit.clear();
  if (frame >= g.frame.size()) { return; }
  ChildGrid::Frame const &f{ g.frame[frame] };
  if (f.children.len == 0) { return; }
  if (out.stamp.size() < g.child.size()) { vec_assign(out.stamp, g.child.size(), 0); }
  ++out.epoch;
  uint32_t const cx0{ cell_of(q.x, f.x0, f.cell_w, f.side) };
  uint32_t const cx1{ cell_of(Wide{ q.x } + q.w, f.x0, f.cell_w, f.side) };
  uint32_t const cy0{ cell_of(q.y, f.y0, f.cell_h, f.side) };
  uint32_t const cy1{ cell_of(Wide{ q.y } + q.h, f.y0, f.cell_h, f.side) };
  for (uint32_t cy = cy0; cy <= cy1; ++cy) {
    for (uint32_t cx = cx0; cx <= cx1; ++cx) {
      uint32_t const cell{ f.bucket + (cy * f.side) + cx };
      for (uint32_t at = g.bucket_off[cell]; at < g.bucket_off[cell + 1]; ++at) {
        uint32_t const child{ g.bucket_at[at] };
        if (out.stamp[child] == out.epoch) { continue; }
        out.stamp[child] = out.epoch;
        vec_push_back(out.hit, child);
      }
    }
  }
}

// Count of overlapping sibling pairs, taken per submachine through its grid.
[[maybe_unused]] int32_t cost_box_overlaps(Chart const &c,
                                           SizedLayout const &z,
                                           ChildGrid const &g) {
  std::vector<scav_rect> kid;
  child_rects(g, z, kid);
  GridQuery q;
  return box_overlaps_over(c, g, kid, q);
}

// Counts each state a piece enters but may not occupy, top down: a child whose rect misses
// the piece's bounding box is pruned with its subtree.
[[maybe_unused]] int32_t cost_through_boxes(Chart const &c,
                                            SizedLayout const &z,
                                            Ancestry const &an,
                                            ChildGrid const &g,
                                            std::vector<Piece> const &pieces) {
  std::vector<scav_rect> kid;
  child_rects(g, z, kid);
  Descent d;
  return through_boxes_over(c, z, an, g, kid, pieces, d, nullptr, nullptr);
}

// Each sweep over a sort of its own; `cost_terms` sorts once and runs all three.
[[maybe_unused]] int64_t cost_crossings(std::vector<Piece> const &pieces,
                                        std::vector<uint32_t> &per_trans) {
  std::vector<uint8_t> is_loose;
  return crossings_over(pieces, lanes_of(pieces), per_trans, is_loose);
}

[[maybe_unused]] Wide cost_corridor(Routes const &r, std::vector<Piece> const &pieces) {
  return corridor_over(r, pieces, lanes_of(pieces), nullptr, nullptr);
}

[[maybe_unused]] Wide cost_crowding(std::vector<Piece> const &pieces, int32_t em) {
  return crowding_over(pieces, lanes_of(pieces), em, nullptr);
}

[[maybe_unused]] int32_t cost_path_turns(scav_rect const &s,
                                         uint32_t s_face,
                                         std::vector<scav_point> const &via,
                                         scav_rect const &t,
                                         uint32_t t_face) {
  return path_turns(face_region(s, s_face), s_face, via, face_region(t, t_face), t_face);
}

SCAV_INTERNAL_END

CostContext cost_context(Chart const &c, SplitGraph const &g) {
  CostContext k;
  k.an = cost_flatten_ancestry(c);
  child_grid_frames(c, c.states.size(), SCAN_MAX, k.grid);
  vec_assign(k.transit_top, c.transitions.size(), { INVALID, INVALID });
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    Transition const &trans{ c.transitions[tr] };
    CommonAncestor const &lca{ g.trans_common[tr] };
    std::array<StateId, 2> const ends{ trans.src, trans.dst };
    for (uint32_t i = 0; i < 2; ++i) {
      StateId const top{ lca.child[i] };
      StateId const own{ enclosing_state(c, ends[i]) };
      if ((top.v != INVALID) && (own.v != INVALID) && (top != ends[i]) && (top != own)) {
        k.transit_top[tr][i] = top.v;
      }
    }
  }
  return k;
}

// Each pairing of two kinds queries a grid over the second; each term's predicate filters
// the superset, so every count equals an all-pairs scan's.
CostTerms cost_terms(CostContext const &ctx,
                     Chart const &c,
                     SplitGraph const &g,
                     SizedLayout const &z,
                     Routes const &r,
                     scav_spaces const &s,
                     scav_profile const &p,
                     std::vector<uint8_t> *party) {
  CostTerms t;
  if (party != nullptr) { vec_assign(*party, c.transitions.size(), 0); }
  if (!geometry_complete(c, z, r)) { return t; }
  t.aspect = (Wide{ z.chart.w } * p.dar_den) - (Wide{ z.chart.h } * p.dar_num);
  if (t.aspect < 0) { t.aspect = -t.aspect; }
  t.area = area_of(z.chart);
  Scratch &sc{ scratch() };
  t.whitespace = whitespace_of(c, z, t.area);
  // Every route segment once; transition `tr`'s pieces are `first[tr]..first[tr + 1]`.
  std::vector<Piece> &pieces{ sc.pieces };
  pieces.clear();
  std::vector<uint32_t> &first{ sc.first };
  vec_assign(first, c.transitions.size() + 1, 0);
  std::vector<uint32_t> &crossings_of{ sc.crossings_of };
  vec_assign(crossings_of, c.transitions.size(), 0);
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    first[tr] = static_cast<uint32_t>(pieces.size());
    scav_span const route{ r.route[tr] };
    std::array<uint32_t, 2> const top{ (tr < ctx.transit_top.size())
                                           ? ctx.transit_top[tr]
                                           : std::array<uint32_t, 2>{ INVALID, INVALID } };
    bool const crosses_through{ (top[0] != INVALID) || (top[1] != INVALID) };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      vec_push_back(pieces,
                    { .a = r.points[route.off + k],
                      .b = r.points[route.off + k + 1],
                      .trans = tr,
                      .k = k });
      if ((k + 2) < route.len) {
        uint32_t const in{ direction(r.points[route.off + k],
                                     r.points[route.off + k + 1]) };
        uint32_t const out{ direction(r.points[route.off + k + 1],
                                      r.points[route.off + k + 2]) };
        if (in != out) {
          ++t.bends;
          if (party != nullptr) { (*party)[tr] = 1; }
          if (crosses_through &&
              in_transit(c, z, c.transitions[tr], top, r.points[route.off + k + 1])) {
            ++t.transit_bends;
          }
        }
        // The reverse of `in` is `8 - in`; odd codes are axis-aligned.
        if (((in % 2) == 1) && (out == (8 - in))) { ++t.retrace; }
      }
    }
    for (uint32_t i = first[tr]; i < pieces.size(); ++i) {
      for (uint32_t j = i + 2; j < pieces.size(); ++j) {
        if (crosses(pieces[i].a, pieces[i].b, pieces[j].a, pieces[j].b)) {
          ++t.self_crossing;
        }
      }
    }
  }
  first[c.transitions.size()] = static_cast<uint32_t>(pieces.size());
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    if ((tr < g.trans_segments.size()) && (g.trans_segments[tr].len != 0) &&
        (r.route[tr].len < 2)) {
      ++t.vanished;
    }
  }
  lanes_of(pieces, sc.key, sc.spare, sc.lanes);
  std::vector<Lane> const &lanes{ sc.lanes };
  t.crossings = crossings_over(pieces, lanes, crossings_of, sc.is_loose);
  t.corridor = corridor_over(r, pieces, lanes, party, &t.shared_run);
  t.crowding = crowding_over(pieces, lanes, p.font_size_grid, party);

  // `excess_len` is what a route runs past the larger of its direct distance and its
  // carried boxes, times one plus its crossings; `length` sums every route.
  std::vector<Wide> &carried{ sc.carried };
  vec_assign(carried, c.transitions.size(), 0);
  if (s.path_box != nullptr) {
    for (uint32_t i = 0; i < s.n_path_box; ++i) {
      if (s.path_box[i].subject < carried.size()) {
        carried[s.path_box[i].subject] += s.path_box[i].w;
      }
    }
  }
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    scav_span const route{ r.route[tr] };
    if (route.len < 2) { continue; }
    Wide actual{ 0 };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      actual += length_of(r.points[route.off + k], r.points[route.off + k + 1]);
    }
    t.length += actual;
    Wide const direct{ length_of(r.points[route.off],
                                 r.points[route.off + route.len - 1]) };
    Wide const excess{ actual - imax(direct, carried[tr]) };
    if (excess > 0) { t.excess_len += excess * (1 + crossings_of[tr]); }
    if ((party != nullptr) && ((excess > 0) || (crossings_of[tr] != 0))) {
      (*party)[tr] = 1;
    }
  }

  // Live states grown by the band `flush` reads; the flush and placed-box queries share
  // this grid.
  int32_t const near{ border_band(p) - 1 };
  std::vector<scav_rect> &state_box{ sc.state_box };
  std::vector<scav_rect> &state_rect{ sc.state_rect };
  std::vector<uint32_t> &state_of{ sc.state_of };
  state_box.clear();
  state_rect.clear();
  state_of.clear();
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if (c.states[st].live == 0) { continue; }
    vec_push_back(state_box, grow(z.state[st], imax(near, 0)));
    vec_push_back(state_rect, z.state[st]);
    vec_push_back(state_of, st);
  }
  RectGrid &states{ sc.states };
  grid_over(states, state_box, sc.cursor);

  // Placed boxes against each other, the states and foreign routes; a state enclosing both
  // endpoints charges only its bands.
  if (!r.placed.empty()) {
    std::vector<scav_rect> &seg_box{ sc.seg_box };
    seg_box.clear();
    for (Piece const &piece : pieces) {
      vec_push_back(seg_box, span_rect(piece.a, piece.b));
    }
    RectGrid &segs{ sc.segs };
    grid_over(segs, seg_box, sc.cursor);
    RectGrid &placed{ sc.placed };
    grid_over(placed, r.placed, sc.cursor);

    std::vector<uint8_t> &encloses{ sc.encloses };  // 1 for a state in `common`
    vec_assign(encloses, c.states.size(), 0);
    std::vector<uint32_t> &common{ sc.common };
    // A placed box's subject, INVALID where the spaces name none.
    auto const subject_of = [&](uint32_t at) {
      return ((s.path_box != nullptr) && (at < s.n_path_box)) ? s.path_box[at].subject
                                                              : INVALID;
    };
    for (uint32_t i = 0; i < r.placed.size(); ++i) {
      scav_rect const &box{ r.placed[i] };
      grid_visit(placed, box, 0, [&](uint32_t j) {
        if ((j > i) && overlaps(box, r.placed[j])) {
          ++t.label;
          blame(party, subject_of(i), subject_of(j));
        }
        return false;
      });
      uint32_t subject{ INVALID };
      uint32_t host{ INVALID };  // an end enclosing the other, or an inner loop's state
      Wide height{ 0 };
      common.clear();
      if ((s.path_box != nullptr) && (i < s.n_path_box) &&
          (s.path_box[i].subject < c.transitions.size())) {
        subject = s.path_box[i].subject;
        height = s.path_box[i].h;
        // `common` collects the states strictly enclosing both ends.
        Transition const &tr{ c.transitions[subject] };
        StateId const both{ g.trans_common[subject].state };
        bool const end{ (both == tr.src) || (both == tr.dst) };
        if (end && (tr.src != tr.dst)) { host = both.v; }
        if (inner_loop(c, subject)) { host = tr.dst.v; }  // the loop is drawn inside it
        StateId up{ end ? enclosing_state(c, both) : both };
        for (size_t step = 0; (step < c.states.size()) && (up.v != INVALID); ++step) {
          encloses[up.v] = 1;
          vec_push_back(common, up.v);
          up = enclosing_state(c, up);
        }
      }
      // Overlap with any state's rect is charged except `common` and the host, which are
      // charged for their bands below: the host in Tier 0.
      grid_visit(states, box, 0, [&](uint32_t at) {
        uint32_t const st{ state_of[at] };
        if ((encloses[st] == 0) && (st != host) && overlaps(box, state_rect[at])) {
          ++t.label_over_box;
          blame(party, subject, INVALID);
        }
        return false;
      });
      auto const banded = [&](uint32_t st) {
        std::array<scav_rect, 5> const walls{ state_walls(z, st) };
        for (uint32_t k = 0; k < 4; ++k) {
          scav_rect const &w{ walls[k] };
          if ((w.w > 0) && (w.h > 0) && overlaps(box, w)) { return true; }
        }
        return false;
      };
      if ((host != INVALID) && (c.states[host].live != 0) && banded(host)) {
        ++t.label_over_box;
        blame(party, subject, INVALID);
      }
      for (uint32_t const st : common) {
        if ((c.states[st].live != 0) && banded(st)) {
          ++t.label;
          blame(party, subject, INVALID);
        }
      }
      // -1 until a leg of that kind is seen; `label_near` counts only a subject with both
      // an own and a foreign leg.
      Wide own{ -1 };
      if (subject != INVALID) {
        for (uint32_t j = first[subject]; j < first[subject + 1]; ++j) {
          Wide const away{ chebyshev_gap(box, seg_box[j]) };
          own = (own < 0) ? away : imin(own, away);
          if (overlaps(box, seg_box[j])) {  // its own line through it
            ++t.label_over_route;
            blame(party, subject, INVALID);
          }
        }
      }
      if (own > label_leader(p)) {
        ++t.label_far;
        blame(party, subject, INVALID);
      }
      // Search margin `own + height - 1`, the farthest a foreign leg still adds to
      // `label_near`; 0 with no own leg.
      Wide const reach{ (own < 0) ? Wide{ 0 } : imax((own + height) - 1, Wide{ 0 }) };
      Wide other{ -1 };
      uint32_t nearest{ INVALID };
      grid_visit(segs, box, reach, [&](uint32_t j) {
        if (pieces[j].trans == subject) { return false; }
        Wide const away{ chebyshev_gap(box, seg_box[j]) };
        if ((other < 0) || (away < other)) { nearest = pieces[j].trans; }
        other = (other < 0) ? away : imin(other, away);
        if (overlaps(box, seg_box[j])) {
          ++t.label_over_route;
          blame(party, subject, pieces[j].trans);
        }
        return false;
      });
      if ((own >= 0) && (other >= 0)) {
        Wide const shortfall{ (own + height) - other };
        if (shortfall > 0) {
          t.label_near += shortfall;
          blame(party, subject, nearest);
        }
      }
      for (uint32_t const st : common) { encloses[st] = 0; }
    }
  }

  t.adjacency = adjacency_of(c, g, z, p);

  // Tier 0 `box_overlap`, `through_box`, `through_band` and `through_region`, from the
  // context's ancestry and this candidate's fill of its child grid.
  ChildGrid &grid{ sc.grid };
  grid.frame = ctx.grid.frame;
  grid.child = ctx.grid.child;
  vec_resize(grid.bucket_off, ctx.grid.bucket_off.size());
  child_rects(grid, z, sc.kid);
  child_grid_fill(grid, sc.kid, sc.cursor);
  t.box_overlap = box_overlaps_over(c, grid, sc.kid, sc.descent.q);
  t.through_box = through_boxes_over(c,
                                     z,
                                     ctx.an,
                                     grid,
                                     sc.kid,
                                     pieces,
                                     sc.descent,
                                     &t.through_band,
                                     &t.through_region);

  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    scav_span const route{ r.route[tr] };
    if (!inner_loop(c, tr) || (route.len < 2)) { continue; }
    uint32_t const st{ c.transitions[tr].dst.v };
    if (!on_drawn_edge(z, s, st, r.points[route.off]) ||
        !on_drawn_edge(z, s, st, r.points[route.off + route.len - 1])) {
      ++t.loop_unanchored;
    }
  }

  // Counts each axial piece that runs along a state's border, found through the grid of
  // states grown by `near`.
  for (Piece const &piece : pieces) {
    if ((piece.a.y != piece.b.y) && (piece.a.x != piece.b.x)) { continue; }
    if (grid_visit(states, span_rect(piece.a, piece.b), 0, [&](uint32_t at) {
          return along_border(piece.a, piece.b, state_rect[at], near);
        })) {
      ++t.flush;
    }
  }

  if ((party != nullptr) && (tier0_of(t) != 0)) {
    vec_assign(*party, c.transitions.size(), uint8_t{ 1 });
  }
  return t;
}

CostTerms cost_terms(Chart const &c,
                     SplitGraph const &g,
                     SizedLayout const &z,
                     Routes const &r,
                     scav_spaces const &s,
                     scav_profile const &p) {
  return cost_terms(cost_context(c, g), c, g, z, r, s, p);
}

CostTerms cost_bound(Chart const &c,
                     SplitGraph const &g,
                     std::vector<std::vector<uint32_t>> const &bends,
                     SizedLayout const &z,
                     std::vector<uint32_t> const &faces,
                     scav_profile const &p,
                     int32_t clear,
                     bool rectilinear) {
  CostTerms t;
  t.area = area_of(z.chart);
  t.whitespace = (p.w_whitespace != 0) ? whitespace_of(c, z, t.area) : 0;
  t.adjacency = adjacency_of(c, g, z, p);
  thread_local std::vector<scav_point> via;
  // The face `faces` names at end `end` of segment `seg`, else INVALID.
  auto const named = [&faces](uint32_t seg, uint32_t end) {
    uint32_t const at{ (seg << 3U) | (end << 2U) };
    auto const hit{ std::ranges::lower_bound(faces, at) };
    return ((hit != faces.end()) && ((*hit & ~3U) == at)) ? (*hit & 3U) : INVALID;
  };
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    if ((tr >= g.trans_segments.size()) || (g.trans_segments[tr].len == 0)) { continue; }
    Transition const &trans{ c.transitions[tr] };
    if (inner_loop(c, tr)) {
      t.bends += rectilinear ? 2 : 0;  // out, along and back, the middle of nonzero length
      continue;
    }
    if (trans.src == trans.dst) { continue; }
    std::array<Region, 2> const box{ region_of(z.state[trans.src.v]),
                                     region_of(z.state[trans.dst.v]) };
    Wide const gap_x{ imax(Wide{ 0 },
                           imax(box[1].x0 - box[0].x1, box[0].x0 - box[1].x1)) };
    Wide const gap_y{ imax(Wide{ 0 },
                           imax(box[1].y0 - box[0].y1, box[0].y0 - box[1].y1)) };
    if (!rectilinear) {
      t.length += imax(gap_x, gap_y);
      continue;
    }
    t.length += gap_x + gap_y;
    // A box-to-box segment also takes its waypoints and the faces it names.
    Span const segs{ g.trans_segments[tr] };
    SplitSegment const &seg{ g.segments[segs.off] };
    bool const direct{ (segs.len == 1) && (trans.kind != TransKind::External) &&
                       (seg.src_port == INVALID) && (seg.dst_port == INVALID) &&
                       (seg.src_inner == 0) && (seg.dst_inner == 0) };
    via.clear();
    std::array<Region, 2> end_at{ box };
    std::array<uint32_t, 2> face{ INVALID, INVALID };
    if (direct) {
      for (uint32_t const bend : (segs.off < bends.size()) ? bends[segs.off] : NO_BENDS) {
        vec_push_back(via, z.node[bend]);
      }
      for (uint32_t end = 0; end < 2; ++end) {
        uint32_t const st{ (end == 0) ? trans.src.v : trans.dst.v };
        uint32_t const f{ named(segs.off, end) };
        scav_rect const &r{ z.state[st] };
        bool const looped{ (st >= z.loop.size()) || (z.loop[st].w > 0) ||
                           (z.loop[st].h > 0) };
        int32_t const arc{
          state_corner_radius(c.states[st].kind, r, z.before[st].x - r.x)
        };
        if ((f == INVALID) || kind_inscribed(c.states[st].kind) || looped ||
            !face_seats(r, f, clear, arc)) {
          continue;
        }
        Region const on{ face_region(r, f) };
        // A face meeting the next point takes no constraint.
        Region const next{ via.empty()
                               ? end_at[1 - end]
                               : region_at((end == 0) ? via.front() : via.back()) };
        if (meets(on, next)) { continue; }
        end_at[end] = on;
        face[end] = f;
      }
    }
    if (via.empty() && (face[0] == INVALID) && (face[1] == INVALID)) {
      t.bends += ((gap_x > 0) && (gap_y > 0)) ? 1 : 0;
    } else {
      t.bends += path_turns(end_at[0], face[0], via, end_at[1], face[1]);
    }
  }
  return t;
}

CostTerms cost_columns(Chart const &c,
                       SplitGraph const &g,
                       scav_profile const &p,
                       scav_spaces const &s,
                       std::vector<scav_rect> const &placed) {
  auto const rows = [&c](char const *name, auto &out) {
    ColumnId const id{ column_find(c, name) };
    if (id.v == INVALID) { return; }
    vec_resize(out, column_count(c, id));
    if (!out.empty()) {
      std::memcpy(out.data(),
                  column_data(c, id),
                  out.size() * sizeof(typename std::decay_t<decltype(out)>::value_type));
    }
  };
  SizedLayout z;
  rows("scav.geom.state", z.state);
  rows("scav.geom.state_before", z.before);
  rows("scav.geom.state_after", z.after);
  rows("scav.geom.state_lead", z.lead);
  rows("scav.geom.state_trail", z.trail);
  rows("scav.geom.sub", z.sub);
  std::vector<scav_rect> chart;
  rows("scav.geom.chart", chart);
  if (!chart.empty()) { z.chart = chart[0]; }
  Routes r;
  rows("scav.geom.point", r.points);
  rows("scav.geom.route", r.route);
  vec_resize(r.route, c.transitions.size());
  r.placed = placed;
  return cost_terms(c, g, z, r, s, p);
}

namespace {

// Each term in the profile's unit, then weighted: lengths in ems, area in ems squared, by
// ceiling; the em is floored at 1.
std::array<Wide, TIER2_TERMS> weighted_terms(CostTerms const &t, scav_profile const &p) {
  Wide const em{ imax(Wide{ p.font_size_grid }, Wide{ 1 }) };
  Wide const em2{ em * em };
  return { Wide{ p.w_bends } * t.bends,
           Wide{ p.w_corridor } * ceil_div(t.corridor, em),
           Wide{ p.w_crossings } * t.crossings,
           Wide{ p.w_excess_len } * ceil_div(t.excess_len, em),
           Wide{ p.w_adjacency } * t.adjacency,
           Wide{ p.w_label } * t.label,
           Wide{ p.w_label_near } * ceil_div(t.label_near, em),
           Wide{ p.w_aspect } * ceil_div(t.aspect, em),
           Wide{ p.w_area } * ceil_div(t.area, em2),
           Wide{ p.w_crowding } * ceil_div(t.crowding, em),
           Wide{ p.w_length } * ceil_div(t.length, em),
           Wide{ p.w_transit_bends } * t.transit_bends,
           Wide{ p.w_whitespace } * ceil_div(t.whitespace, em2) };
}

}  // namespace

CostTerms layout_cost(Chart const &c,
                      scav_profile const &p,
                      scav_spaces const &s,
                      std::vector<scav_rect> const &placed) {
  return cost_columns(c, decompose(c), p, s, placed);
}

std::array<int32_t, TIER0_TERMS> tier0_terms(CostTerms const &t) {
  return { t.through_box,    t.through_band,   t.box_overlap,      t.vanished,
           t.flush,          t.through_region, t.retrace,          t.self_crossing,
           t.shared_run,     t.label_over_box, t.label_over_route, t.label_far,
           t.loop_unanchored };
}

Cost cost_of(CostTerms const &t, scav_profile const &p) {
  Cost out;
  out.t0_violations = tier0_of(t);
  // Area, the largest term, is below (2 * COORD_MAX)^2 < 2^40; thirteen terms under
  // weights capped at 2^10 sum below 2^54.
  for (Wide const term : weighted_terms(t, p)) { out.t2 += term; }
  return out;
}

std::array<int64_t, TIER2_TERMS> cost_shares(CostTerms const &t, scav_profile const &p) {
  std::array<Wide, TIER2_TERMS> const part{ weighted_terms(t, p) };
  Wide whole{ 0 };
  for (Wide const term : part) { whole += term; }
  std::array<int64_t, TIER2_TERMS> out{};
  if (whole <= 0) { return out; }
  // A sum past 2^48 shifts both sides down until the product with the 14-bit multiplier
  // fits; the weight caps keep every sum below 2^48.
  constexpr Wide BASIS_POINTS{ 10'000 };
  constexpr uint32_t SAFE_BITS{ 48 };
  uint32_t const bits{ ilog2(static_cast<uint64_t>(whole)) };
  uint32_t const shift{ (bits > SAFE_BITS) ? (bits - SAFE_BITS) : 0U };
  Wide const den{ whole >> shift };
  for (uint32_t i = 0; i < TIER2_TERMS; ++i) {
    out[i] = floor_div((part[i] >> shift) * BASIS_POINTS, den);
  }
  return out;
}

bool cost_less(Cost const &a, Cost const &b) {
  if (a.t0_violations != b.t0_violations) { return a.t0_violations < b.t0_violations; }
  if (a.t1_hints != b.t1_hints) { return a.t1_hints < b.t1_hints; }
  return a.t2 < b.t2;
}

}  // namespace scav
