// Scoring, over the rects and polylines the phases produced. Every predicate
// here is degree 2: an intersection is four `orient2d` sign tests and the
// point is never constructed, so nothing needs more than int64 (11.2).

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

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace scav {

// Bracketed so the containment walk, the grid, and the three sweeps that read
// them are reachable from a test with hand-written rects rather than only
// through a whole chart's score. The prototypes a test uses are its own; see
// scav_internal.h.
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
SCAV_INTERNAL_END

namespace {

// The cell array is the square of this, so a frame past a few thousand
// children shares cells rather than growing one.
constexpr uint32_t GRID_SIDE_MAX{ 64 };

// A frame of at most this many children is scanned and has no grid cells.
constexpr uint32_t SCAN_MAX{ 16 };

Wide orient2d(scav_point a, scav_point b, scav_point c) {
  return ((Wide{ b.x } - a.x) * (Wide{ c.y } - a.y)) -
         ((Wide{ b.y } - a.y) * (Wide{ c.x } - a.x));
}

// Proper crossing only: a shared endpoint or a collinear overlap is not one,
// which is what keeps a route meeting its own port from counting.
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

// Touching or one separation apart on one axis while overlapping on the
// other, which is what a direct arrow between two regions needs (11.8).
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

// Which line a piece can share a run along: 0 the horizontal at `at`, 1 the
// vertical at it, 2 neither. A degenerate or diagonal piece is 2, and both
// arms of `shared_run` come out zero for one, so no bucket wants it.
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

// Two routes' shared ends: the identical points they finish and start with,
// and whether the leg reaching each of those runs lies along it.
struct Trunk {
  uint32_t tail{ 0 };
  bool merged_tail{ false };
};

// One shared kink into a common destination and nothing else (11.9.3). Routes
// arriving at one state may read as one line -- the reader has something to
// follow them to. A fan-out does not. So no head, and the tail caps at the
// final leg rather than at however much suffix coincides.
constexpr uint32_t TRUNK_TAIL{ 2 };

Trunk trunk_of(std::vector<scav_point> const &pts, scav_span a, scav_span b) {
  Trunk out;
  uint32_t const shortest{ imin(imin(a.len, b.len), TRUNK_TAIL) };
  while ((out.tail < shortest) &&
         same(pts[(a.off + a.len - 1) - out.tail], pts[(b.off + b.len - 1) - out.tail])) {
    ++out.tail;
  }
  if ((out.tail > 0) && (out.tail < a.len) && (out.tail < b.len)) {
    uint32_t const i{ (a.off + a.len - 1) - out.tail };
    uint32_t const j{ (b.off + b.len - 1) - out.tail };
    out.merged_tail = shared_run(pts[i], pts[i + 1], pts[j], pts[j + 1]) > 0;
  }
  return out;
}

// Segment `k` is in the trunk: the final leg they arrive on as one, or the leg
// that merges into it. `len` counts that route's points.
bool trunk_piece(Trunk const &t, uint32_t len, uint32_t k) {
  return ((k + t.tail) >= len) || (t.merged_tail && ((k + t.tail + 1) == len));
}

// 0, 1, or 2 per axis, the same token the structural hash uses, so a bend is
// a change in the pair.
uint32_t direction(scav_point a, scav_point b) {
  auto const axis = [](int32_t from, int32_t to) {
    if (to > from) { return 2U; }
    return (to < from) ? 0U : 1U;
  };
  return (axis(a.x, b.x) * 3U) + axis(a.y, b.y);
}

// Cells per axis over `n` disjoint children, so a cell holds a small constant
// number of them.
uint32_t grid_side(uint32_t n) {
  return imin(static_cast<uint32_t>(isqrt(n)) + 1U, GRID_SIDE_MAX);
}

// The cell a coordinate falls in, clamped to the grid: a query rect may reach
// outside the bounds the frame's own children drew.
uint32_t cell_of(Wide at, int32_t origin, Wide size, uint32_t side) {
  Wide const i{ floor_div(at - origin, size) };
  return static_cast<uint32_t>(imax(Wide{ 0 }, imin(i, Wide{ side } - 1)));
}

// Every term below indexes a column by entity ordinal and every route by its
// span, so geometry shorter than the entities it parallels is answered here.
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

// A piece along a line: `axis` and `at` as `piece_axis` gives them, and its extent.
struct Lane {
  uint32_t axis;
  int32_t at;
  int32_t lo, hi;
  uint32_t piece;
};

// Ascending LSD radix sort, one counting scatter per byte through `spare`; a byte
// every key shares skips its pass. The keys are distinct, so the order is unique.
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
    // First horizontal past `lo`: the band is open at both ends, a horizontal
    // through one of the vertical's own endpoints being a touch.
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
    // Inside the band the y test already holds, so `crosses` reduces to the
    // vertical's x lying strictly inside the horizontal's extent.
    for (uint32_t at = low; (at < flat) && (lanes[at].at < lv.hi); ++at) {
      Lane const &lh{ lanes[at] };
      if (pieces[lh.piece].trans == pieces[lv.piece].trans) { continue; }
      if ((lh.lo < lv.at) && (lv.at < lh.hi)) { charge(lv.piece, lh.piece); }
    }
  }

  for (uint32_t d = 0; d < pieces.size(); ++d) {
    if (is_loose[d] == 0) { continue; }
    for (uint32_t j = 0; j < pieces.size(); ++j) {
      // Two of them are one pair, charged from the earlier index alone, which
      // is also what skips a piece against itself.
      if ((is_loose[j] != 0) && (j <= d)) { continue; }
      if (pieces[d].trans == pieces[j].trans) { continue; }
      if (crosses(pieces[d].a, pieces[d].b, pieces[j].a, pieces[j].b)) { charge(d, j); }
    }
  }
  return total;
}

// Marks both transitions of a charged pair in `party`, where given.
void blame(std::vector<uint8_t> *party, uint32_t a, uint32_t b) {
  if (party == nullptr) { return; }
  if (a < party->size()) { (*party)[a] = 1; }
  if (b < party->size()) { (*party)[b] = 1; }
}

// The length each pair shares on one line, per `(axis, coordinate)` bucket, less the
// trunks.
Wide corridor_over(Routes const &r,
                   std::vector<Piece> const &pieces,
                   std::vector<Lane> const &lanes,
                   std::vector<uint8_t> *party) {
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
        Trunk const pair{ trunk_of(r.points, r.route[u.trans], r.route[v.trans]) };
        if (trunk_piece(pair, r.route[u.trans].len, u.k) &&
            trunk_piece(pair, r.route[v.trans].len, v.k)) {
          continue;
        }
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
      if (apart == 0) { continue; }  // on one line: `corridor`'s, not this
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
// order, and cells only for a frame of more than `scan_max`; no other frame is queried.
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

  // Count into the cell after each, prefix-sum, then place through a cursor
  // copy: the usual two passes, so a child spanning cells is stored in each.
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

// Whether `state` lies inside region `m`, by walking up from it at most one step per
// state.
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
Wide whitespace_of(Chart const &c,
                   SizedLayout const &z,
                   std::vector<scav_extent> const &room,
                   Wide chart) {
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
    if (st < room.size()) { held += Wide{ room[st].w } * room[st].h; }
    total += imax((imax(w, Wide{ 0 }) * imax(h, Wide{ 0 })) - held, Wide{ 0 });
  }
  return imin(total, chart);
}

// About one cell per rect, over the rects' own bounds.
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
        // Span order inside a frame, so a pair is charged from its first
        // member alone however many cells the two share.
        if (at <= (kids.off + i)) { continue; }
        if (overlaps(a, kid[at])) { ++total; }
      }
    }
  }
  return total;
}

// What the Tier-0 descent reuses from one piece to the next.
struct Descent {
  std::vector<uint32_t> roots, stack;
  GridQuery q;
};

// `cost_through_boxes` over `kid`, as `box_overlaps_over` takes it.
int32_t through_boxes_over(Chart const &c,
                           SizedLayout const &z,
                           Ancestry const &an,
                           ChildGrid const &g,
                           std::vector<scav_rect> const &kid,
                           std::vector<Piece> const &pieces,
                           Descent &d,
                           int32_t *bands) {
  d.roots.clear();
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].owner.v == INVALID) { vec_push_back(d.roots, m); }
  }

  int32_t total{ 0 };
  for (Piece const &piece : pieces) {
    Transition const &tr{ c.transitions[piece.trans] };
    scav_rect const reach{ span_rect(piece.a, piece.b) };
    // An edge may occupy the interior of a state it is an endpoint of or a
    // descendant of, and only that one: 11.14's carve-out.
    auto const charge = [&](uint32_t st, scav_rect const &box) {
      if (!cost_ancestor(c, an, { st }, tr.src) && !cost_ancestor(c, an, { st }, tr.dst)) {
        if (enters(piece.a, piece.b, box)) { ++total; }
        return;
      }
      // Inside a state it may occupy, its bands are the walls.
      if ((bands == nullptr) || (st >= z.before.size())) { return; }
      std::array<scav_rect, 5> const walls{ state_walls(z, st) };
      for (uint32_t k = 0; k < 4; ++k) {
        scav_rect const &r{ walls[k] };
        if ((r.w > 0) && (r.h > 0) && enters(piece.a, piece.b, r)) { ++*bands; }
      }
    };
    for (uint32_t const st : an.detached) { charge(st, z.state[st]); }

    auto const visit = [&](uint32_t at) {
      if (!overlaps(reach, kid[at])) { return; }
      uint32_t const st{ g.child[at] };
      charge(st, kid[at]);
      Span const subs{ c.states[st].submachines };
      for (uint32_t i = 0; i < subs.len; ++i) {
        vec_push_back(d.stack, c.submachine_ids[subs.off + i].v);
      }
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
  }
  return total;
}

// Every buffer one `cost_terms` call uses, per thread and reassigned in place.
// Nothing `cost_terms` calls waits on the pool, so no second call starts on this thread.
struct Scratch {
  std::vector<Piece> pieces;
  std::vector<uint32_t> first, crossings_of;
  std::vector<uint64_t> key, spare;
  std::vector<Lane> lanes;
  std::vector<uint8_t> is_loose;
  std::vector<Wide> carried;
  std::vector<scav_rect> state_box, state_rect;  // grown, and as placed
  std::vector<uint32_t> state_of;
  std::vector<scav_rect> seg_box, region_box;
  std::vector<uint32_t> region_of;
  RectGrid states, segs, placed, regions;
  std::vector<uint32_t> cursor;  // every grid build's placing pass
  std::vector<uint8_t> encloses;
  std::vector<uint32_t> common;
  ChildGrid grid;  // `CostContext::grid`, filled for the candidate
  std::vector<scav_rect> kid;
  Descent descent;
  std::vector<scav_extent> loop_label, loop_room;
};

Scratch &scratch() {
  thread_local Scratch s;
  return s;
}

int32_t tier0_of(CostTerms const &t) {
  return t.through_box + t.through_band + t.box_overlap + t.vanished + t.flush +
         t.through_region + t.retrace + t.label_over_box + t.label_over_route;
}

}  // namespace

SCAV_INTERNAL_BEGIN

Ancestry cost_flatten_ancestry(Chart const &c) {
  Ancestry out;
  vec_assign(out.tin, c.states.size(), 0);
  vec_assign(out.tout, c.states.size(), 0);
  std::vector<uint8_t> buried(c.states.size(), 0);

  // `open` 0 is the exit marker, pushed under a state's own children so `tout`
  // is written once the whole subtree has been walked.
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
  // A submachine no state owns is a document root; the forest is its children
  // and theirs. Pushed in reverse, so the walk leaves in ordinal order.
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
  // A containment cycle leaves its states off the walk, so those two climb.
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

// Siblings may not overlap; a nested box legitimately does, so the pairs are
// taken within one submachine's children and through that frame's own grid.
[[maybe_unused]] int32_t cost_box_overlaps(Chart const &c,
                                           SizedLayout const &z,
                                           ChildGrid const &g) {
  std::vector<scav_rect> kid;
  child_rects(g, z, kid);
  GridQuery q;
  return box_overlaps_over(c, g, kid, q);
}

// Tier 0, top down. A piece meets a box's interior only if it meets every box
// enclosing it, so a frame whose child the piece misses prunes that child's
// whole subtree; the test that prunes is the piece's bounding box against the
// rect, which unlike `enters` cannot answer false for an enclosing box.
[[maybe_unused]] int32_t cost_through_boxes(Chart const &c,
                                            SizedLayout const &z,
                                            Ancestry const &an,
                                            ChildGrid const &g,
                                            std::vector<Piece> const &pieces) {
  std::vector<scav_rect> kid;
  child_rects(g, z, kid);
  Descent d;
  return through_boxes_over(c, z, an, g, kid, pieces, d, nullptr);
}

// Each sweep over a sort of its own; `cost_terms` sorts once and runs all three.
[[maybe_unused]] int64_t cost_crossings(std::vector<Piece> const &pieces,
                                        std::vector<uint32_t> &per_trans) {
  std::vector<uint8_t> is_loose;
  return crossings_over(pieces, lanes_of(pieces), per_trans, is_loose);
}

[[maybe_unused]] Wide cost_corridor(Routes const &r, std::vector<Piece> const &pieces) {
  return corridor_over(r, pieces, lanes_of(pieces), nullptr);
}

[[maybe_unused]] Wide cost_crowding(std::vector<Piece> const &pieces, int32_t em) {
  return crowding_over(pieces, lanes_of(pieces), em, nullptr);
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

// Each pairing of two kinds is a grid over the second kind. The grid yields a superset
// and each term's own predicate decides, so every count equals an all-pairs scan's.
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
  // Every route segment once, tagged with its transition; transition `tr`'s pieces
  // run from `first[tr]` to `first[tr + 1]`.
  Scratch &sc{ scratch() };
  loop_rooms(c, s, p, sc.loop_label, sc.loop_room);
  t.whitespace = whitespace_of(c, z, sc.loop_room, t.area);
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
        // `direction` is three steps per axis with the middle one still, so
        // the reverse of `in` is `8 - in`; odd codes are the axis-aligned ones.
        if (((in % 2) == 1) && (out == (8 - in))) { ++t.retrace; }
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
  t.corridor = corridor_over(r, pieces, lanes, party);
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

  // The live states, each grown by the band `flush` reads, so one grid answers
  // both a route along a border and a placed box over a state.
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
  // endpoints, walked from above them, bars only its text bands.
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
      uint32_t host{ INVALID };  // an endpoint enclosing the other end
      Wide height{ 0 };
      common.clear();
      if ((s.path_box != nullptr) && (i < s.n_path_box) &&
          (s.path_box[i].subject < c.transitions.size())) {
        subject = s.path_box[i].subject;
        height = s.path_box[i].h;
        // Only a state strictly enclosing both ends is exempt from its own rect.
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
      // The grid charges every state's rect but those in `common` and the host, which pay
      // for their bands: the host's in Tier 0.
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
      // -1 until a leg of that kind is seen, so a routeless subject and a chart
      // with one transition both fall out of `label_near` rather than into it.
      Wide own{ -1 };
      if (subject != INVALID) {
        for (uint32_t j = first[subject]; j < first[subject + 1]; ++j) {
          Wide const away{ chebyshev_gap(box, seg_box[j]) };
          own = (own < 0) ? away : imin(own, away);
        }
      }
      // A shortfall needs a foreign leg within `own + height - 1`, so that margin
      // finds the nearest one that counts; with no own leg it is zero and finds overlaps.
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

  // A direct arrow between concurrent submachines wants them adjacent; fork and join fans
  // are exempt.
  for (SplitSegment const &seg : g.segments) {
    if (seg.separator == 0) { continue; }
    Transition const &tr{ c.transitions[seg.trans.v] };
    StateKind const src_kind{ c.states[tr.src.v].kind };
    StateKind const dst_kind{ c.states[tr.dst.v].kind };
    if ((src_kind == StateKind::Fork) || (src_kind == StateKind::Join) ||
        (dst_kind == StateKind::Fork) || (dst_kind == StateKind::Join)) {
      continue;
    }
    SubmachineId const from{ g.ports[seg.src_port].sub };
    SubmachineId const to{ g.ports[seg.dst_port].sub };
    if ((from.v == INVALID) || (to.v == INVALID)) { continue; }
    if (!adjacent(z.sub[from.v], z.sub[to.v], p.sub_sep)) { ++t.adjacency; }
  }

  // Tier 0, both counts off one flattening of the containment forest and one
  // grid per frame over it.
  ChildGrid &grid{ sc.grid };
  grid.frame = ctx.grid.frame;
  grid.child = ctx.grid.child;
  vec_resize(grid.bucket_off, ctx.grid.bucket_off.size());
  child_rects(grid, z, sc.kid);
  child_grid_fill(grid, sc.kid, sc.cursor);
  t.box_overlap = box_overlaps_over(c, grid, sc.kid, sc.descent.q);
  t.through_box =
      through_boxes_over(c, z, ctx.an, grid, sc.kid, pieces, sc.descent, &t.through_band);

  // A diagonal runs along no border; an axial leg along one lies within `near` of the
  // state, so the grid of grown rects finds it.
  for (Piece const &piece : pieces) {
    if ((piece.a.y != piece.b.y) && (piece.a.x != piece.b.x)) { continue; }
    if (grid_visit(states, span_rect(piece.a, piece.b), 0, [&](uint32_t at) {
          return along_border(piece.a, piece.b, state_rect[at], near);
        })) {
      ++t.flush;
    }
  }

  std::vector<scav_rect> &region_box{ sc.region_box };
  std::vector<uint32_t> &region_of{ sc.region_of };
  region_box.clear();
  region_of.clear();
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if ((c.submachines[m].live == 0) || (c.submachines[m].owner.v == INVALID)) {
      continue;
    }
    vec_push_back(region_box, grow(z.sub[m], 1));
    vec_push_back(region_of, m);
  }
  RectGrid &regions{ sc.regions };
  grid_over(regions, region_box, sc.cursor);
  for (Piece const &piece : pieces) {
    Transition const &trans{ c.transitions[piece.trans] };
    scav_rect const reach{ span_rect(piece.a, piece.b) };
    if (grid_visit(regions, reach, 0, [&](uint32_t at) {
          uint32_t const m{ region_of[at] };
          return overlaps(reach, region_box[at]) && !within(c, trans.src, m) &&
                 !within(c, trans.dst, m) && enters(piece.a, piece.b, z.sub[m]);
        })) {
      ++t.through_region;
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

Cost cost_of(CostTerms const &t, scav_profile const &p) {
  Cost out;
  out.t0_violations = tier0_of(t);
  // Area is the largest term at (2 * COORD_MAX)^2 < 2^40, its em^2 only divides
  // it down, and thirteen of those under a weight capped at 2^10 stay below 2^54.
  for (Wide const term : weighted_terms(t, p)) { out.t2 += term; }
  return out;
}

std::array<int64_t, TIER2_TERMS> cost_shares(CostTerms const &t, scav_profile const &p) {
  std::array<Wide, TIER2_TERMS> const part{ weighted_terms(t, p) };
  Wide whole{ 0 };
  for (Wide const term : part) { whole += term; }
  std::array<int64_t, TIER2_TERMS> out{};
  if (whole <= 0) { return out; }
  // The multiplier needs fourteen bits, so a sum past 2^48 shifts both sides
  // down until the product fits; no sum the weight caps allow ever gets there.
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
