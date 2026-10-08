// The `orthogonal` router: a visibility graph per frame with h-plane and v-plane copies
// joined by a bend-penalty edge, searched by A*.

#include "layout/router_orthogonal.h"

#include "layout/geom.h"
#include "layout/router.h"
#include "layout/trace.h"
#include "scav/scav_layout.h"
#include "scav_cold.h"
#include "scav_int.h"
#include "scav_pod_vector.h"
#include "scav_stable_sort.h"
#include "scav_vec.h"

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

// `f`, then `g`, then node; strict over the open list, which holds a node once per `g`.
bool frontier_before(OrthoFrontierEntry const &a, OrthoFrontierEntry const &b) {
  if (a.f != b.f) { return a.f < b.f; }
  if (a.g != b.g) { return a.g < b.g; }
  return a.node < b.node;
}

// A 4-ary heap: children of `i` are `4i+1 .. 4i+4`. Push and pop shift a hole and write
// the moving entry once.
constexpr uint32_t HEAP_ARITY{ 4 };

void heap_push(PodVector<OrthoFrontierEntry> &heap, OrthoFrontierEntry const &item) {
  heap.push_back(item);
  uint32_t i{ static_cast<uint32_t>(heap.size()) - 1 };
  while (i > 0) {
    uint32_t const up{ (i - 1) / HEAP_ARITY };
    if (!frontier_before(item, heap[up])) { break; }
    heap[i] = heap[up];
    i = up;
  }
  heap[i] = item;
}

OrthoFrontierEntry heap_pop(PodVector<OrthoFrontierEntry> &heap) {
  OrthoFrontierEntry const top{ heap[0] };
  OrthoFrontierEntry const last{ heap.back() };
  heap.pop_back();
  auto const n{ static_cast<uint32_t>(heap.size()) };
  if (n == 0) { return top; }
  uint32_t i{ 0 };
  for (;;) {
    uint32_t const first{ (HEAP_ARITY * i) + 1 };
    if (first >= n) { break; }
    uint32_t const end{ imin(first + HEAP_ARITY, n) };
    uint32_t least{ first };
    for (uint32_t c = first + 1; c < end; ++c) {
      if (frontier_before(heap[c], heap[least])) { least = c; }
    }
    if (!frontier_before(heap[least], last)) { break; }
    heap[i] = heap[least];
    i = least;
  }
  heap[i] = last;
  return top;
}

// `f` as an unsigned key in the same order.
uint64_t radix_key(Wide f) { return static_cast<uint64_t>(f) ^ (UINT64_C(1) << 63U); }

// Pushes `e` onto `ties` when its key equals `last`, else into the bucket of the highest
// bit where its key differs from `last`.
void radix_place(OrthoRadixHeap &h, OrthoFrontierEntry const &e) {
  uint64_t const diff{ radix_key(e.f) ^ h.last };
  if (diff == 0) {
    heap_push(h.ties, e);
    return;
  }
  uint32_t const b{ static_cast<uint32_t>(std::bit_width(diff)) - 1U };
  h.bucket[b].push_back(e);
  h.full |= UINT64_C(1) << b;
}

}  // namespace

void ortho_open_clear(OrthoRadixHeap &h) {
  for (uint64_t full{ h.full }; full != 0; full &= full - 1) {
    h.bucket[static_cast<uint32_t>(std::countr_zero(full))].clear();
  }
  h.ties.clear();
  h.full = 0;
  h.last = 0;
}

bool ortho_open_empty(OrthoRadixHeap const &h) { return h.ties.empty() && (h.full == 0); }

void ortho_open_push(OrthoRadixHeap &h, OrthoFrontierEntry const &e) {
  uint64_t const key{ radix_key(e.f) };
  if (key < h.last) {
    // A key below `last` lowers it and re-places every entry.
    h.spill.clear();
    for (OrthoFrontierEntry const &t : h.ties) { h.spill.push_back(t); }
    for (uint64_t full{ h.full }; full != 0; full &= full - 1) {
      PodVector<OrthoFrontierEntry> &from{
        h.bucket[static_cast<uint32_t>(std::countr_zero(full))]
      };
      for (OrthoFrontierEntry const &t : from) { h.spill.push_back(t); }
      from.clear();
    }
    h.ties.clear();
    h.full = 0;
    h.last = key;
    for (OrthoFrontierEntry const &t : h.spill) { radix_place(h, t); }
  }
  radix_place(h, e);
}

OrthoFrontierEntry ortho_open_pop(OrthoRadixHeap &h) {
  if (h.ties.empty()) {
    // The lowest nonempty bucket holds the least `f`, which becomes `last`; its entries
    // re-place into lower buckets and `ties`.
    auto const b{ static_cast<uint32_t>(std::countr_zero(h.full)) };
    PodVector<OrthoFrontierEntry> &from{ h.bucket[b] };
    uint64_t least{ radix_key(from[0].f) };
    for (OrthoFrontierEntry const &t : from) { least = imin(least, radix_key(t.f)); }
    h.last = least;
    h.full &= ~(UINT64_C(1) << b);
    for (OrthoFrontierEntry const &t : from) { radix_place(h, t); }
    from.clear();
  }
  return heap_pop(h.ties);
}

namespace {

Wide distance(int32_t a, int32_t b) { return (a < b) ? (Wide{ b } - a) : (Wide{ a } - b); }

// How far `v` lies outside `[lo, lo + len]`, zero inside it.
Wide beyond(int32_t v, int32_t lo, int32_t len) {
  Wide const hi{ Wide{ lo } + len };
  if (v < lo) { return Wide{ lo } - v; }
  if (Wide{ v } > hi) { return Wide{ v } - hi; }
  return Wide{ 0 };
}

// One face's run `[lo, lo + len]` and the inset a seat keeps from each corner.
struct FaceRun {
  int32_t lo, len, inset;
};

FaceRun face_run(scav_rect const &r, uint32_t face, int32_t clear, int32_t arc) {
  int32_t const len{ (face < 2) ? r.h : r.w };
  return { .lo = (face < 2) ? r.y : r.x,
           .len = len,
           .inset = imin(imax(clear, arc), len / 2) };
}

int32_t onto_face(int32_t v, FaceRun const &run) {
  return imin(imax(v, run.lo + run.inset), (run.lo + run.len) - run.inset);
}

// A seat on a box face; `pos` is its coordinate along the face, `rank` its place among
// seats on one point.
struct Spot {
  uint32_t box, face, end, slot;
  int32_t pos, rank;
};

// A seat the spread places. Bound set 0 is its face's run between its walls; set 1 is that
// run cut to the far face's where the net runs straight.
struct Member {
  uint32_t seat;  // index into the spread's seats
  int32_t pos;
  std::array<int32_t, 2> lo, hi;
};

// Members `first` on that the spread places at `start + i * s`; `how` is 0 inside both
// faces of a straight net, 1 inside their own face, 2 where they stand.
struct Block {
  uint32_t first, count;
  int32_t s, start;
  uint32_t how;
};

// For `count` members from `m`, the `i`th at `start + i * s`: the largest `s` from `least`
// to `step` that keeps each within bound set `b`, and the `start` nearest the members'
// mean; false where no `s` fits.
bool fit_unit(Member const *m,
              uint32_t count,
              uint32_t b,
              int32_t least,
              int32_t step,
              int32_t &s,
              int32_t &start) {
  Wide most{ step };
  Wide fewest{ least };
  for (uint32_t i = 0; i < count; ++i) {
    if (m[i].lo[b] > m[i].hi[b]) { return false; }
    for (uint32_t j = i + 1; j < count; ++j) {
      Wide const dk{ j - i };
      most = imin(most, floor_div(Wide{ m[j].hi[b] } - m[i].lo[b], dk));
      fewest = imax(fewest, ceil_div(Wide{ m[j].lo[b] } - m[i].hi[b], dk));
    }
  }
  if (most < fewest) { return false; }
  s = static_cast<int32_t>(most);
  Wide lo{ INT32_MIN };
  Wide hi{ INT32_MAX };
  Wide sum{ 0 };
  for (uint32_t i = 0; i < count; ++i) {
    Wide const off{ Wide{ i } * s };
    lo = imax(lo, Wide{ m[i].lo[b] } - off);
    hi = imin(hi, Wide{ m[i].hi[b] } - off);
    sum += Wide{ m[i].pos } - off;
  }
  Wide const n{ count };
  Wide const want{ floor_div((Wide{ 2 } * sum) + n,
                             Wide{ 2 } * n) };  // the mean, halves up
  start = static_cast<int32_t>(imin(imax(want, lo), hi));
  return true;
}

// `aim` from `seat` on `face`: along the face, then out from it, 0 for an aim behind it
// and 1 for an aim on the seat.
std::array<Wide, 2> aim_offset(scav_point aim, scav_point seat, uint32_t face) {
  bool const along_y{ face < 2 };
  Wide const along{ along_y ? (Wide{ aim.y } - seat.y) : (Wide{ aim.x } - seat.x) };
  Wide out{ 0 };
  switch (face) {
    case 0: out = Wide{ seat.x } - aim.x; break;
    case 1: out = Wide{ aim.x } - seat.x; break;
    case 2: out = Wide{ seat.y } - aim.y; break;
    default: out = Wide{ aim.y } - seat.y; break;
  }
  out = imax(out, Wide{ 0 });
  if ((along == 0) && (out == 0)) { out = 1; }
  return { along, out };
}

// True when offset `u` points nearer the face's low end than `v`, by angle about the seat.
bool lower_aim(std::array<Wide, 2> const &u, std::array<Wide, 2> const &v) {
  if ((u[1] == 0) && (v[1] == 0)) { return u[0] < v[0]; }
  return ((v[0] * u[1]) - (v[1] * u[0])) > 0;
}

// True when nets `a` and `b` name a common box.
bool shares_box(PodVector<RouteNet> const &nets, uint32_t a, uint32_t b) {
  std::array<uint32_t, 2> const one{ nets[a].src_obstacle, nets[a].dst_obstacle };
  std::array<uint32_t, 2> const two{ nets[b].src_obstacle, nets[b].dst_obstacle };
  for (uint32_t const x : one) {
    for (uint32_t const y : two) {
      if ((x != INVALID) && (x == y)) { return true; }
    }
  }
  return false;
}

// Midpoint of `face`, rounded as `ortho_attach_box` rounds an inscribed glyph's seat.
scav_point face_middle(scav_rect const &r, uint32_t face) {
  switch (face) {
    case 0: return { .x = r.x, .y = r.y + (r.h / 2) };
    case 1: return { .x = r.x + r.w, .y = r.y + (r.h / 2) };
    case 2: return { .x = r.x + (r.w / 2), .y = r.y };
    default: return { .x = r.x + (r.w / 2), .y = r.y + r.h };
  }
}

// Face of `s`'s box that `at[s.slot]` lies on; INVALID for an end naming no box.
uint32_t face_at(Seat const &s,
                 PodVector<scav_rect> const &boxes,
                 PodVector<scav_point> const &at) {
  return (s.box < boxes.size()) ? face_of(at[s.slot], boxes[s.box]) : INVALID;
}

// The face nearer `toward` on one axis: 2 or 3 for y, 0 or 1 for x. Ties go to top or
// left, as in `ortho_escape_box`.
uint32_t nearer_face(scav_point toward, scav_rect const &r, bool axis_is_y) {
  if (axis_is_y) {
    bool const top{ (Wide{ toward.y } - r.y) <= ((Wide{ r.y } + r.h) - toward.y) };
    return top ? 2U : 3U;
  }
  bool const left{ (Wide{ toward.x } - r.x) <= ((Wide{ r.x } + r.w) - toward.x) };
  return left ? 0U : 1U;
}

}  // namespace

bool ortho_blocks_h(scav_rect const &r, int32_t y, int32_t x0, int32_t x1) {
  return (r.w > 0) && (y > r.y) && (y < (r.y + r.h)) && (x0 < (r.x + r.w)) && (x1 > r.x);
}

bool ortho_blocks_v(scav_rect const &r, int32_t x, int32_t y0, int32_t y1) {
  return (r.h > 0) && (x > r.x) && (x < (r.x + r.w)) && (y0 < (r.y + r.h)) && (y1 > r.y);
}

namespace {

// First index with `v[i] > key` in sorted `v`.
uint32_t ortho_after(PodVector<int32_t> const &v, int32_t key) {
  uint32_t lo{ 0 };
  uint32_t hi{ static_cast<uint32_t>(v.size()) };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (v[mid] > key) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// First index with `v[i] >= key` in sorted `v`.
uint32_t ortho_from(PodVector<int32_t> const &v, int32_t key) {
  uint32_t lo{ 0 };
  uint32_t hi{ static_cast<uint32_t>(v.size()) };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (v[mid] >= key) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

}  // namespace

void ortho_sort_unique(PodVector<int32_t> &v) {
  // LSD radix sort on sign-flipped bytes through a thread-local buffer, skipping a byte
  // every value shares; insertion sort for small inputs.
  size_t const n{ v.size() };
  if (n <= SCAV_SORT_SMALL) {
    scav_insertion_sort(v.data(), v.data() + n, [](int32_t a, int32_t b) {
      return a < b;
    });
  } else {
    constexpr uint32_t DIGITS{ 4 };
    constexpr uint32_t RADIX{ 256 };
    thread_local PodVector<int32_t> spare;
    spare.resize(n);
    auto const digit = [](int32_t x, uint32_t d) {
      return ((static_cast<uint32_t>(x) ^ 0x8000'0000U) >> (8U * d)) & 0xFFU;
    };
    std::array<std::array<uint32_t, RADIX>, DIGITS> count{};
    for (int32_t const x : v) {
      for (uint32_t d = 0; d < DIGITS; ++d) { ++count[d][digit(x, d)]; }
    }
    int32_t *src{ v.data() };
    int32_t *dst{ spare.data() };
    for (uint32_t d = 0; d < DIGITS; ++d) {
      std::array<uint32_t, RADIX> &at{ count[d] };
      if (at[digit(src[0], d)] == n) { continue; }
      uint32_t sum{ 0 };
      for (uint32_t &slot : at) {
        uint32_t const here{ slot };
        slot = sum;
        sum += here;
      }
      for (size_t i = 0; i < n; ++i) { dst[at[digit(src[i], d)]++] = src[i]; }
      int32_t *const was{ src };
      src = dst;
      dst = was;
    }
    if (src != v.data()) {
      for (size_t i = 0; i < n; ++i) { v[i] = src[i]; }
    }
  }
  uint32_t kept{ 0 };
  for (uint32_t i = 0; i < v.size(); ++i) {
    if ((kept == 0) || (v[i] != v[kept - 1])) { v[kept++] = v[i]; }
  }
  v.resize(kept);
}

uint32_t ortho_index_of(PodVector<int32_t> const &v, int32_t at) {
  uint32_t lo{ 0 };
  uint32_t hi{ static_cast<uint32_t>(v.size()) };
  while ((hi - lo) > 1) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (v[mid] <= at) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

bool ortho_escape_horizontal(scav_point toward, scav_rect const &r) {
  return beyond(toward.x, r.x, r.w) >= beyond(toward.y, r.y, r.h);
}

uint32_t ortho_escape_face(scav_point toward, scav_rect const &r) {
  if (ortho_escape_horizontal(toward, r)) {
    return ((Wide{ toward.x } - r.x) <= ((Wide{ r.x } + r.w) - toward.x)) ? 0U : 1U;
  }
  return ((Wide{ toward.y } - r.y) <= ((Wide{ r.y } + r.h) - toward.y)) ? 2U : 3U;
}

scav_point ortho_escape_box(scav_point at, scav_point toward, scav_rect const &r) {
  switch (ortho_escape_face(toward, r)) {
    case 0: return { .x = r.x, .y = at.y };
    case 1: return { .x = r.x + r.w, .y = at.y };
    case 2: return { .x = at.x, .y = r.y };
    default: return { .x = at.x, .y = r.y + r.h };
  }
}

scav_point ortho_attach_face(scav_point toward,
                             scav_rect const &r,
                             int32_t clear,
                             bool inscribed,
                             int32_t corner,
                             uint32_t face) {
  if (!inscribed && !face_seats(r, face, clear, corner)) {
    return ortho_attach_box(toward, r, clear, inscribed, corner);
  }
  FaceRun const run{ face_run(r, face, clear, corner) };
  if (face < 2) {  // left or right: the position along it is a y
    int32_t const y{ inscribed ? (r.y + (r.h / 2)) : onto_face(toward.y, run) };
    return { .x = (face == 0) ? r.x : (r.x + r.w), .y = y };
  }
  int32_t const x{ inscribed ? (r.x + (r.w / 2)) : onto_face(toward.x, run) };
  return { .x = x, .y = (face == 2) ? r.y : (r.y + r.h) };
}

scav_point ortho_attach_box(scav_point toward,
                            scav_rect const &r,
                            int32_t clear,
                            bool inscribed,
                            int32_t corner) {
  scav_point aimed{ toward };
  if (ortho_escape_horizontal(toward, r)) {
    aimed.y =
        inscribed ? (r.y + (r.h / 2)) : onto_face(toward.y, face_run(r, 0, clear, corner));
  } else {
    aimed.x =
        inscribed ? (r.x + (r.w / 2)) : onto_face(toward.x, face_run(r, 2, clear, corner));
  }
  return ortho_escape_box(aimed, toward, r);
}

void ortho_seats(PodVector<RouteNet> const &nets,
                 PodVector<uint8_t> const &inscribed,
                 PodVector<int32_t> const &corner,
                 PodVector<Seat> &out) {
  out.assign(2 * nets.size(), Seat{});
  for (uint32_t slot = 0; slot < out.size(); ++slot) {
    RouteNet const &net{ nets[slot / 2] };
    uint32_t const box{ ((slot % 2) == 0) ? net.src_obstacle : net.dst_obstacle };
    out[slot] = { .box = box,
                  .end = slot % 2,
                  .slot = slot,
                  .inscribed = (box < inscribed.size()) && (inscribed[box] != 0),
                  .arc = (box < corner.size()) ? corner[box] : 0 };
  }
}

void ortho_reface_attachments(PodVector<scav_rect> const &boxes,
                              PodVector<Seat> const &table,
                              PodVector<scav_point> const &toward,
                              PodVector<scav_point> &at) {
  thread_local PodVector<Spot> seats;
  seats.clear();
  for (Seat const &s : table) {
    uint32_t const face{ face_at(s, boxes, at) };
    if (!s.inscribed || (face == INVALID)) { continue; }
    seats.push_back(
        { .box = s.box, .face = face, .end = s.end, .slot = s.slot, .pos = 0, .rank = 0 });
  }
  scav_stable_sort(seats, [](Spot const &a, Spot const &b) {
    if (a.box != b.box) { return a.box < b.box; }
    if (a.face != b.face) { return a.face < b.face; }
    if (a.end != b.end) { return a.end < b.end; }
    return a.slot < b.slot;
  });

  for (uint32_t start = 0; start < seats.size();) {
    uint32_t stop{ start };
    while ((stop < seats.size()) && (seats[stop].box == seats[start].box)) { ++stop; }
    uint32_t const first{ start };
    scav_rect const &r{ boxes[seats[first].box] };
    start = stop;

    // Per face, the ends it holds: bit 0 departures, bit 1 arrivals.
    std::array<uint8_t, 4> holds{ 0, 0, 0, 0 };
    for (uint32_t i = first; i < stop; ++i) {
      uint32_t const face{ seats[i].face };
      holds[face] = static_cast<uint8_t>(holds[face] | (1U << seats[i].end));
    }
    for (uint32_t face = 0; face < 4; ++face) {
      if (holds[face] != 3) { continue; }
      bool const cross_is_y{ face < 2 };  // the other two faces are top and bottom
      uint32_t const cross_low{ cross_is_y ? 2U : 0U };  // top for y, left for x
      // The direction whose aims lie farthest outside the box on the cross axis moves; a
      // tie moves the departures.
      std::array<Wide, 2> gain{ 0, 0 };
      for (uint32_t i = first; i < stop; ++i) {
        if (seats[i].face != face) { continue; }
        scav_point const aim{ toward[seats[i].slot] };
        Wide const off{ cross_is_y ? beyond(aim.y, r.y, r.h) : beyond(aim.x, r.x, r.w) };
        gain[seats[i].end] = imax(gain[seats[i].end], off);
      }
      uint32_t const mover{ (gain[1] > gain[0]) ? 1U : 0U };
      // Movers vote for the near or far cross-axis face by `nearer_face` of their aims.
      int32_t votes{ 0 };
      for (uint32_t i = first; i < stop; ++i) {
        if ((seats[i].face != face) || (seats[i].end != mover)) { continue; }
        bool const near{ nearer_face(toward[seats[i].slot], r, cross_is_y) == cross_low };
        votes += near ? 1 : -1;
      }
      // Tries the voted face, the other cross face, then the opposite face; the first
      // holding none of the other direction takes the movers.
      uint32_t const nearest{ (votes >= 0) ? cross_low : (cross_low + 1) };
      std::array<uint32_t, 3> const tries{ nearest, nearest ^ 1U, face ^ 1U };
      uint32_t chosen{ INVALID };
      for (uint32_t const candidate : tries) {
        if ((holds[candidate] & (1U << (1U - mover))) == 0) {
          chosen = candidate;
          break;
        }
      }
      if (chosen == INVALID) { continue; }  // no free face: the movers stay
      for (uint32_t i = first; i < stop; ++i) {
        if ((seats[i].face != face) || (seats[i].end != mover)) { continue; }
        at[seats[i].slot] = face_middle(r, chosen);
        seats[i].face = chosen;
      }
      holds[face] = static_cast<uint8_t>(holds[face] & ~(1U << mover));
      holds[chosen] = static_cast<uint8_t>(holds[chosen] | (1U << mover));
    }
  }
}

void ortho_align_attachments(PodVector<RouteNet> const &nets,
                             PodVector<scav_rect> const &boxes,
                             PodVector<Seat> const &table,
                             PodVector<scav_point> &at) {
  for (uint32_t n = 0; n < nets.size(); ++n) {
    RouteNet const &net{ nets[n] };
    if (net.waypoint_len != 0) { continue; }
    uint32_t const src_box{ net.src_obstacle };
    uint32_t const dst_box{ net.dst_obstacle };
    if ((src_box >= boxes.size()) || (dst_box >= boxes.size()) || (src_box == dst_box)) {
      continue;
    }
    uint32_t const src_slot{ 2 * n };
    uint32_t const dst_slot{ src_slot + 1 };
    uint32_t const leaves{ face_of(at[src_slot], boxes[src_box]) };
    uint32_t const arrives{ face_of(at[dst_slot], boxes[dst_box]) };
    if ((leaves == INVALID) || (arrives == INVALID)) { continue; }
    bool const along_y{ leaves < 2 };
    if (along_y != (arrives < 2)) { continue; }  // not parallel: no shared coordinate

    // `lo`..`hi` is the run both faces can seat: each face inset `max(arc, 1)` from its
    // corners, an inscribed glyph's face its midpoint alone.
    int32_t lo{ 0 };
    int32_t hi{ 0 };
    Wide centres{ 0 };
    for (uint32_t k = 0; k < 2; ++k) {
      Seat const &seat{ table[src_slot + k] };
      FaceRun const run{
        face_run(boxes[seat.box], (k == 0) ? leaves : arrives, 1, seat.arc)
      };
      int32_t const inset{ seat.inscribed ? (run.len / 2) : run.inset };
      int32_t const face_lo{ run.lo + inset };
      int32_t const face_hi{ seat.inscribed ? face_lo : ((run.lo + run.len) - inset) };
      lo = (k == 0) ? face_lo : imax(lo, face_lo);
      hi = (k == 0) ? face_hi : imin(hi, face_hi);
      centres += Wide{ run.lo } + (run.len / 2);
    }
    if (lo > hi) { continue; }  // no coordinate both faces can seat

    // The face centres' midpoint clamped to the run; a leaning net takes its low end.
    int32_t const want{ static_cast<int32_t>(floor_div(centres, Wide{ 2 })) };
    int32_t const got{ (net.lean != 0) ? lo : imin(imax(want, lo), hi) };
    if (along_y) {
      at[src_slot].y = got;
      at[dst_slot].y = got;
    } else {
      at[src_slot].x = got;
      at[dst_slot].x = got;
    }
  }
}

void ortho_spread_attachments(PodVector<scav_rect> const &boxes,
                              PodVector<Seat> const &table,
                              PodVector<scav_point> const &toward,
                              int32_t clear,
                              int32_t pitch,
                              PodVector<scav_point> &at) {
  if (clear <= 0) { return; }
  int32_t const apart{ imax(clear, pitch) };
  // Every box end on a face but a self-loop's, grouped by face.
  thread_local PodVector<Spot> seats;
  seats.clear();
  for (Seat const &s : table) {
    uint32_t const face{ face_at(s, boxes, at) };
    if (s.inscribed || (face == INVALID) || (table[s.slot ^ 1U].box == s.box)) {
      continue;
    }
    seats.push_back(
        { .box = s.box, .face = face, .end = s.end, .slot = s.slot, .pos = 0, .rank = 0 });
  }
  scav_stable_sort(seats, [](Spot const &a, Spot const &b) {
    if (a.box != b.box) { return a.box < b.box; }
    return a.face < b.face;
  });

  // Per face, its first seat, its seat count and its step: a pitch apart, or as evenly as
  // the run between its insets holds every seat. Per slot, its face.
  thread_local PodVector<std::array<int32_t, 3>> faces;
  thread_local PodVector<uint32_t> face_of_slot;
  thread_local PodVector<uint32_t> order;  // faces, widest step first
  faces.clear();
  face_of_slot.assign(table.size(), INVALID);
  for (uint32_t first = 0; first < seats.size();) {
    uint32_t end{ first + 1 };
    while ((end < seats.size()) && (seats[end].box == seats[first].box) &&
           (seats[end].face == seats[first].face)) {
      ++end;
    }
    FaceRun const run{ face_run(boxes[seats[first].box],
                                seats[first].face,
                                clear,
                                table[seats[first].slot].arc) };
    Wide const room{ Wide{ run.len } - (Wide{ 2 } * run.inset) };
    Wide const gaps{ end - first - 1 };
    Wide step{ 0 };
    if (gaps > 0) { step = (room >= (gaps * apart)) ? apart : (room / gaps); }
    for (uint32_t i = first; i < end; ++i) {
      face_of_slot[seats[i].slot] = static_cast<uint32_t>(faces.size());
    }
    faces.push_back({ static_cast<int32_t>(first),
                      static_cast<int32_t>(end - first),
                      static_cast<int32_t>(step) });
    first = end;
  }
  order.clear();
  for (uint32_t f = 0; f < faces.size(); ++f) { order.push_back(f); }
  scav_stable_sort(order,
                   [](uint32_t a, uint32_t b) { return faces[a][2] > faces[b][2]; });

  // True when a seat's far end names no box and its aim is level with it: a port's
  // straight leg, to the port or to the waypoint before it.
  auto const level = [&](Spot const &seat) {
    scav_point const aim{ toward[seat.slot] };
    return (table[seat.slot ^ 1U].box >= boxes.size()) &&
           (((seat.face < 2) ? aim.y : aim.x) == seat.pos);
  };

  // The far end's coordinate along a box face parallel to `seat`'s, else INT32_MIN.
  auto const parallel = [&](Spot const &seat) {
    Seat const &twin{ table[seat.slot ^ 1U] };
    if (twin.box >= boxes.size()) { return INT32_MIN; }
    uint32_t const face{ face_of(at[twin.slot], boxes[twin.box]) };
    bool const along_y{ seat.face < 2 };
    if ((face == INVALID) || ((face < 2) != along_y)) { return INT32_MIN; }
    return along_y ? at[twin.slot].y : at[twin.slot].x;
  };

  // What keeps a seat where it is: 1 a `level` leg, 2 a straight leg to an inscribed
  // glyph's one seat, 3 a straight leg to a face holding other seats too; else 0.
  auto const hold = [&](Spot const &seat) {
    if (level(seat)) { return 1; }
    if (parallel(seat) != seat.pos) { return 0; }
    Seat const &twin{ table[seat.slot ^ 1U] };
    if (twin.inscribed) { return 2; }
    uint32_t const far{ face_of_slot[twin.slot] };
    return ((far != INVALID) && (faces[far][1] > 1)) ? 3 : 0;
  };

  // Moves `seat` to `got` along its face; returns false when it is already there.
  auto const place = [&](Spot const &seat, int32_t got) {
    bool const along_y{ seat.face < 2 };
    int32_t &held{ along_y ? at[seat.slot].y : at[seat.slot].x };
    if (held == got) { return false; }
    held = got;

    // A straight net's far end moves with it where its face seats it.
    Seat const &twin{ table[seat.slot ^ 1U] };
    if ((twin.box >= boxes.size()) || twin.inscribed) { return true; }
    uint32_t const face{ face_of(at[twin.slot], boxes[twin.box]) };
    if ((face == INVALID) || ((face < 2) != along_y)) { return true; }
    int32_t &there{ along_y ? at[twin.slot].y : at[twin.slot].x };
    if ((there == seat.pos) &&
        (onto_face(got, face_run(boxes[twin.box], face, clear, twin.arc)) == got)) {
      there = got;
    }
    return true;
  };

  // `lo`..`hi` cut to the run of the far end's face where `seat`'s net runs straight
  // between parallel faces of two boxes.
  auto const tied = [&](Spot const &seat, int32_t lo, int32_t hi) {
    std::array<int32_t, 2> out{ lo, hi };
    Seat const &twin{ table[seat.slot ^ 1U] };
    if (twin.inscribed || (parallel(seat) != seat.pos)) { return out; }
    FaceRun const far{
      face_run(boxes[twin.box], face_of(at[twin.slot], boxes[twin.box]), clear, twin.arc)
    };
    out[0] = imax(lo, far.lo + far.inset);
    out[1] = imin(hi, (far.lo + far.len) - far.inset);
    return out;
  };

  // Spreads face `f`'s seats; true when any moved.
  thread_local PodVector<Member> members;
  thread_local PodVector<Block> blocks;
  uint32_t round{ 0 };
  auto const spread_face = [&](uint32_t f) {
    auto const first{ static_cast<uint32_t>(faces[f][0]) };
    auto const end{ first + static_cast<uint32_t>(faces[f][1]) };
    int32_t const d{ faces[f][2] };
    if (d <= 0) { return false; }
    FaceRun const run{ face_run(boxes[seats[first].box],
                                seats[first].face,
                                clear,
                                table[seats[first].slot].arc) };
    int32_t const lo{ run.lo + run.inset };
    int32_t const hi{ (run.lo + run.len) - run.inset };

    // Along the face. A seat alone at its point that something holds stays; of seats on
    // one point, a level or glyph leg keeps it, and the rest take a side: the side
    // running + across the face low, each in aim order.
    bool const departures_low{ (seats[first].face == 1) || (seats[first].face == 3) };
    for (uint32_t i = first; i < end; ++i) {
      Spot &seat{ seats[i] };
      seat.pos = (seat.face < 2) ? at[seat.slot].y : at[seat.slot].x;
      seat.rank = hold(seat);
    }
    scav_insertion_sort(seats.data() + first,
                        seats.data() + end,
                        [](Spot const &a, Spot const &b) {
                          return (a.pos < b.pos) ||
                                 ((a.pos == b.pos) && (a.slot < b.slot));
                        });
    for (uint32_t i = first; i < end;) {
      uint32_t j{ i + 1 };
      while ((j < end) && (seats[j].pos == seats[i].pos)) { ++j; }
      uint32_t keeper{ ((j == (i + 1)) && (seats[i].rank != 0)) ? i : INVALID };
      for (uint32_t k = i; (j > (i + 1)) && (k < j); ++k) {
        if ((seats[k].rank == 1) || (seats[k].rank == 2)) {
          if ((keeper == INVALID) || (seats[k].rank < seats[keeper].rank)) { keeper = k; }
        }
      }
      std::array<Wide, 3> side{ 0, 0, 0 };
      for (uint32_t k = i; k < j; ++k) {
        Spot &seat{ seats[k] };
        if (k == keeper) { continue; }
        seat.rank = ((seat.end == 0) == departures_low) ? 0 : 2;
        ++side[static_cast<uint32_t>(seat.rank)];
      }
      // The keeper goes past the others where their side's run cannot hold them.
      if (keeper != INVALID) {
        Wide const pos{ seats[i].pos };
        Wide const all{ side[0] + side[2] };
        seats[keeper].rank = 1;
        if (((pos + (side[2] * d)) > hi) && ((pos - (all * d)) >= lo)) {
          seats[keeper].rank = 3;
        }
        if (((pos - (side[0] * d)) < lo) && ((pos + (all * d)) <= hi)) {
          seats[keeper].rank = -1;
        }
      }
      i = j;
    }
    scav_insertion_sort(
        seats.data() + first,
        seats.data() + end,
        [&](Spot const &a, Spot const &b) {
          if (a.pos != b.pos) { return a.pos < b.pos; }
          if (a.rank != b.rank) { return a.rank < b.rank; }
          std::array<Wide, 2> const u{ aim_offset(toward[a.slot], at[a.slot], a.face) };
          std::array<Wide, 2> const v{ aim_offset(toward[b.slot], at[b.slot], b.face) };
          if (lower_aim(u, v) != lower_aim(v, u)) { return lower_aim(u, v); }
          return a.slot < b.slot;
        });

    // A kept seat is a wall; the seats between two walls part a step apart, a lone one
    // moving only off a wall.
    bool moved{ false };
    std::array<uint32_t, 3> placed{ 0, 0, 0 };  // blocks by how each was placed
    uint32_t count{ 0 };                        // blocks
    for (uint32_t from = first; from < end;) {
      if ((seats[from].rank & 1) != 0) {  // a keeper's rank is odd
        ++from;
        continue;
      }
      uint32_t to{ from };
      while ((to < end) && ((seats[to].rank & 1) == 0)) { ++to; }
      // A step off each wall, or as evenly as the room between the walls holds the seats.
      bool const below{ from > first };
      bool const above{ to < end };
      Wide const room{ Wide{ above ? seats[to].pos : hi } -
                       (below ? seats[from - 1].pos : lo) };
      Wide const gaps{ Wide{ to - from - 1 } + (below ? 1 : 0) + (above ? 1 : 0) };
      auto const g{ static_cast<int32_t>(
          (gaps > 0) ? imax(Wide{ 0 }, imin(Wide{ d }, room / gaps)) : d) };
      int32_t const wall_lo{ below ? (seats[from - 1].pos + g) : INT32_MIN };
      int32_t const wall_hi{ above ? (seats[to].pos - g) : INT32_MAX };
      members.clear();
      for (uint32_t i = from; i < to; ++i) {
        Spot const &seat{ seats[i] };
        int32_t const face_lo{ imax(lo, wall_lo) };
        int32_t const face_hi{ imin(hi, wall_hi) };
        std::array<int32_t, 2> const far{ tied(seat, face_lo, face_hi) };
        members.push_back({ .seat = i,
                            .pos = seat.pos,
                            .lo = { face_lo, far[0] },
                            .hi = { face_hi, far[1] } });
      }
      // A lone seat stays between its walls; several part inside both faces of a straight
      // net down to a clearance apart, else inside their own face, else stay.
      auto const solve = [&](Block &b) {
        Member const *const m{ members.data() + b.first };
        b.s = d;
        b.start = imin(imax(m[0].pos, wall_lo), wall_hi);
        b.how = 0;
        if ((b.count == 1) && (wall_lo <= wall_hi) && (b.start >= imin(lo, m[0].pos)) &&
            (b.start <= imax(hi, m[0].pos))) {
          return;
        }
        if ((b.count > 1) && fit_unit(m, b.count, 1, imin(d, clear), d, b.s, b.start)) {
          return;
        }
        b.how = 1;
        if (fit_unit(m, b.count, 0, 0, d, b.s, b.start)) { return; }
        b.how = 2;
      };
      // Where block `b` puts its `i`th member.
      auto const spot_of = [&](Block const &b, uint32_t i) {
        return (b.how == 2) ? members[b.first + i].pos
                            : (b.start + (static_cast<int32_t>(i) * b.s));
      };
      blocks.clear();
      for (uint32_t i = 0; i < members.size(); ++i) {
        blocks.push_back(
            { .first = i, .count = 1, .s = d, .start = members[i].pos, .how = 0 });
        solve(blocks.back());
        while (blocks.size() >= 2) {
          Block &low{ blocks[blocks.size() - 2] };
          Block const &high{ blocks.back() };
          if ((Wide{ spot_of(high, 0) } - spot_of(low, low.count - 1)) >= d) { break; }
          low.count += high.count;
          blocks.pop_back();
          solve(blocks.back());
        }
      }
      for (Block const &b : blocks) {
        ++placed[b.how];
        ++count;
        for (uint32_t i = 0; i < b.count; ++i) {
          if (place(seats[members[b.first + i].seat], spot_of(b, i))) { moved = true; }
        }
      }
      from = to;
    }
    if (moved) {
      trace_emit({ .kind = TraceKind::FaceSpread,
                   .spread = { .box = seats[first].box,
                               .face = seats[first].face,
                               .round = round,
                               .seats = end - first,
                               .step = d,
                               .blocks = count,
                               .relaxed = placed[1],
                               .frozen = placed[2] } });
    }
    return moved;
  };

  // Sweeps every face, widest step first, until no seat moves, at most one round per seat.
  for (; round < seats.size(); ++round) {
    bool moved{ false };
    for (uint32_t const f : order) {
      if (spread_face(f)) { moved = true; }
    }
    if (!moved) { break; }
  }
}

SCAV_COLD void ortho_order_attachments(PodVector<RouteNet> const &nets,
                                       PodVector<scav_rect> const &boxes,
                                       int32_t clear,
                                       PodVector<uint32_t> const &groups,
                                       PodVector<scav_point> &points,
                                       PodVector<scav_span> const &spans,
                                       PodVector<scav_point> &at) {
  // A group's end whose bend nearest its box can slide along the leg out to it: the
  // route's points from that end inward, the side that leg runs to, and the bend's
  // distance out from the face.
  struct Bent {
    uint32_t slot;
    std::array<uint32_t, 3> pt;  // the end, its bend, and the point past the bend
    int32_t side;
    Wide out;
    int32_t from;  // the along-face coordinate of the point past the bend
    bool inward;   // the leg on from that point runs back nearer the face
  };
  thread_local PodVector<Bent> moving;
  thread_local PodVector<int32_t> seats;
  for (uint32_t start = 0; start < groups.size();) {
    uint32_t stop{ start };
    while ((stop < groups.size()) && (groups[stop] != INVALID)) { ++stop; }
    uint32_t const first{ start };
    start = stop + 1;
    moving.clear();
    seats.clear();
    uint32_t face{ INVALID };
    bool whole{ true };  // every movable end sits on one face
    for (uint32_t i = first; i < stop; ++i) {
      uint32_t const slot{ groups[i] };
      scav_span const span{ spans[slot / 2] };
      if (span.len < 3) { continue; }
      bool const head{ (slot % 2) == 0 };
      // The route's `k`th point counted in from this end.
      auto const in_from = [&](uint32_t k) {
        return head ? (span.off + k) : ((span.off + span.len - 1) - k);
      };
      scav_point const e{ points[in_from(0)] };
      scav_point const b{ points[in_from(1)] };
      scav_point const a{ points[in_from(2)] };
      uint32_t const box{ ortho_box_at(e, boxes) };
      if (!same(e, at[slot]) || (box == INVALID)) { continue; }
      uint32_t const f{ face_of(e, boxes[box]) };
      bool const along_y{ f < 2 };
      int32_t const a_along{ along_y ? a.y : a.x };
      int32_t const b_along{ along_y ? b.y : b.x };
      bool const square{ (b_along == (along_y ? e.y : e.x)) &&
                         ((along_y ? b.x : b.y) != (along_y ? e.x : e.y)) };
      bool const beside{ ((along_y ? a.x : a.y) == (along_y ? b.x : b.y)) &&
                         (a_along != b_along) };
      if ((f >= 4) || !square || !beside) { continue; }
      whole = whole && ((face == INVALID) || (face == f));
      face = f;
      Wide const out{ along_y ? distance(b.x, e.x) : distance(b.y, e.y) };
      bool inward{ false };
      if (span.len >= 4) {
        scav_point const lead{ points[in_from(3)] };
        inward = (along_y ? distance(lead.x, e.x) : distance(lead.y, e.y)) < out;
      }
      moving.push_back({ .slot = slot,
                         .pt = { in_from(0), in_from(1), in_from(2) },
                         .side = (a_along < b_along) ? -1 : 1,
                         .out = out,
                         .from = a_along,
                         .inward = inward });
      seats.push_back(b_along);
    }
    if (!whole || (moving.size() < 2)) { continue; }

    // Low side first, nearest bend lowest; high side last, nearest bend highest. Of bends
    // on one line, the inward-led sit outermost, farther-off first if led outward.
    scav_stable_sort(seats, [](int32_t x, int32_t y) { return x < y; });
    scav_stable_sort(moving, [](Bent const &x, Bent const &y) {
      if (x.side != y.side) { return x.side < y.side; }
      if (x.out != y.out) { return (x.side < 0) ? (x.out < y.out) : (x.out > y.out); }
      if (x.inward != y.inward) { return (x.side < 0) == x.inward; }
      if (x.from != y.from) { return x.inward ? (x.from > y.from) : (x.from < y.from); }
      return x.slot < y.slot;
    });
    bool const along_y{ face < 2 };
    bool ok{ true };
    for (uint32_t k = 0; ok && (k < moving.size()); ++k) {
      Bent const &m{ moving[k] };
      RouteNet const &net{ nets[m.slot / 2] };
      scav_point const e{ points[m.pt[0]] };
      scav_point const b{ points[m.pt[1]] };
      scav_point const a{ points[m.pt[2]] };
      int32_t const to{ seats[k] };
      int32_t const a_along{ along_y ? a.y : a.x };
      ok = ((a_along < to) == (a_along < (along_y ? b.y : b.x))) && (a_along != to);
      // A leg into the route's other end keeps the ring the router seated that end at.
      scav_span const span{ spans[m.slot / 2] };
      if (span.len == 3) {
        bool const head{ (m.slot % 2) == 0 };
        uint32_t const other{ head ? net.dst_obstacle : net.src_obstacle };
        int32_t const keep{ head ? net.dst_clear : net.src_clear };
        ok = ok &&
             ((other >= boxes.size()) || (distance(a_along, to) >= imax(clear, keep)));
      }
      int32_t const lo{ imin(a_along, to) };
      int32_t const hi{ imax(a_along, to) };
      int32_t const near{ along_y ? imin(b.x, e.x) : imin(b.y, e.y) };
      int32_t const far{ along_y ? imax(b.x, e.x) : imax(b.y, e.y) };
      // No moved leg enters a box, or the clearance round any box but the net's own.
      for (uint32_t r = 0; ok && (r < boxes.size()); ++r) {
        bool const own{ (r == net.src_obstacle) || (r == net.dst_obstacle) };
        scav_rect const q{ own ? boxes[r] : grow(boxes[r], clear) };
        ok = along_y
                 ? !(ortho_blocks_v(q, a.x, lo, hi) || ortho_blocks_h(q, to, near, far))
                 : !(ortho_blocks_h(q, a.y, lo, hi) || ortho_blocks_v(q, to, near, far));
      }
    }
    if (!ok) { continue; }
    for (uint32_t k = 0; k < moving.size(); ++k) {
      int32_t const to{ seats[k] };
      for (uint32_t const i : { moving[k].pt[0], moving[k].pt[1] }) {
        (along_y ? points[i].y : points[i].x) = to;
      }
      at[moving[k].slot] = points[moving[k].pt[0]];
    }
  }
}

SCAV_COLD void ortho_seat_loops(PodVector<RouteNet> const &nets,
                                PodVector<scav_rect> const &boxes,
                                PodVector<Seat> const &table,
                                std::vector<OccupiedSpan> const &occupied,
                                int32_t clear,
                                int32_t pitch,
                                PodVector<scav_point> &at) {
  thread_local PodVector<std::array<int32_t, 2>> taken;  // runs `[lo, hi]` seats avoid
  for (uint32_t n = 0; n < nets.size(); ++n) {
    RouteNet const &net{ nets[n] };
    uint32_t const b{ net.src_obstacle };
    if ((net.loop <= 0) || (b >= boxes.size()) || (net.dst_obstacle != b)) { continue; }
    scav_rect const &r{ boxes[b] };
    uint32_t const slot{ 2 * n };
    uint32_t const face{ face_of(at[slot], r) };
    if ((face == INVALID) || (face_of(at[slot + 1], r) != face)) { continue; }
    bool const along_y{ face < 2 };
    FaceRun const run{ face_run(r, face, clear, table[slot].arc) };
    int32_t const lo{ run.lo + run.inset };
    int32_t const hi{ (run.lo + run.len) - run.inset };
    taken.clear();
    for (Seat const &other : table) {
      if ((other.slot / 2) == n) { continue; }
      bool const point{ other.box >= boxes.size() };  // no box: the end is its own point
      RouteNet const &owner{ nets[other.slot / 2] };
      scav_point const own{ (other.end == 0) ? owner.src : owner.dst };
      scav_point const seat{ point ? own : at[other.slot] };
      if (((other.box == b) || point) && (face_of(seat, r) == face)) {
        int32_t const pos{ along_y ? seat.y : seat.x };
        taken.push_back({ pos, pos });
      }
    }
    for (OccupiedSpan const &o : occupied) {
      if ((o.obstacle == b) && (o.face == face)) {
        taken.push_back({ o.lo, o.lo + o.len });
      }
    }
    scav_stable_sort(taken,
                     [](std::array<int32_t, 2> const &lhs,
                        std::array<int32_t, 2> const &rhs) { return lhs[0] < rhs[0]; });
    // The widest gap between taken runs and the face run's ends.
    int32_t from{ lo };
    int32_t best_lo{ lo };
    int32_t best_hi{ lo };
    for (size_t k = 0; k <= taken.size(); ++k) {
      int32_t const to{ (k < taken.size()) ? imin(imax(taken[k][0], lo), hi) : hi };
      if ((to - from) > (best_hi - best_lo)) {
        best_lo = from;
        best_hi = to;
      }
      from = imax(from, (k < taken.size()) ? imin(imax(taken[k][1], lo), hi) : hi);
    }
    // Centred, `pitch` apart and `pitch` from neighbouring seats where that fits; else
    // `room / parts` apart, a part per neighbouring seat plus one.
    bool const ends_lo{ best_lo == lo };
    bool const ends_hi{ best_hi == hi };
    int32_t const inner_lo{ best_lo + (ends_lo ? 0 : pitch) };
    int32_t const inner_hi{ best_hi - (ends_hi ? 0 : pitch) };
    int32_t &first{ along_y ? at[slot].y : at[slot].x };
    int32_t &second{ along_y ? at[slot + 1].y : at[slot + 1].x };
    if ((inner_hi - inner_lo) >= pitch) {
      first = inner_lo + floor_div((inner_hi - inner_lo) - pitch, 2);
      second = first + pitch;
      continue;
    }
    int32_t const room{ best_hi - best_lo };
    int32_t parts{ 3 };
    if (ends_lo || ends_hi) { parts = (ends_lo && ends_hi) ? 1 : 2; }
    int32_t const apart{ imin(pitch, room / parts) };
    first = (best_lo + (room / 2)) - (apart / 2);
    second = first + apart;
  }
}

void ortho_separate_attachments(PodVector<RouteNet> const &nets,
                                PodVector<scav_rect> const &boxes,
                                PodVector<Seat> const &table,
                                PodVector<scav_point> const &toward,
                                int32_t clear,
                                int32_t pitch,
                                PodVector<scav_point> &at) {
  if ((pitch <= 0) || (clear <= 0)) { return; }

  // One end's leg: `pos` is the coordinate it runs at (y on a left or right face, x on top
  // or bottom); `lo`..`hi` is the span it crosses to the other end.
  struct Leg {
    uint32_t box, face, slot, net;
    int32_t pos, lo, hi;
  };
  thread_local PodVector<Leg> legs;
  legs.clear();
  // True for a port's straight leg: its far end names no box and its aim is level with it.
  auto const level = [&](Leg const &leg) {
    scav_point const aim{ toward[leg.slot] };
    return (table[leg.slot ^ 1U].box >= boxes.size()) &&
           (((leg.face < 2) ? aim.y : aim.x) == leg.pos);
  };
  for (Seat const &s : table) {
    uint32_t const face{ face_at(s, boxes, at) };
    if (s.inscribed || (face == INVALID)) { continue; }
    scav_point const here{ at[s.slot] };
    scav_point const other{ at[s.slot ^ 1U] };
    bool const along_y{ face < 2 };
    int32_t const run{ along_y ? other.x : other.y };
    int32_t const from{ along_y ? here.x : here.y };
    legs.push_back({ .box = s.box,
                     .face = face,
                     .slot = s.slot,
                     .net = s.slot / 2,
                     .pos = along_y ? here.y : here.x,
                     .lo = imin(from, run),
                     .hi = imax(from, run) });
  }

  // Compares legs of one orientation, adjacent in `pos` order.
  auto const sweep = [&](bool along_y) {
    thread_local PodVector<uint32_t> here;
    here.clear();
    for (uint32_t i = 0; i < legs.size(); ++i) {
      if ((legs[i].face < 2) == along_y) { here.push_back(i); }
    }
    scav_stable_sort(here, [](uint32_t a, uint32_t b) {
      if (legs[a].pos != legs[b].pos) { return legs[a].pos < legs[b].pos; }
      return legs[a].slot < legs[b].slot;
    });
    bool moved{ false };
    for (uint32_t k = 0; (k + 1) < here.size(); ++k) {
      Leg &low{ legs[here[k]] };
      Leg &high{ legs[here[k + 1]] };
      // Skips a net's own two ends, and two legs on one face, which the spread handles.
      if (low.net == high.net) { continue; }
      if ((low.box == high.box) && (low.face == high.face)) { continue; }
      if (shares_box(nets, low.net, high.net)) { continue; }
      int32_t const apart{ high.pos - low.pos };
      if ((apart <= 0) || (apart >= pitch)) { continue; }
      if ((low.hi <= high.lo) || (high.hi <= low.lo)) { continue; }  // spans disjoint
      // Half the shortfall each, or all of it to the partner of a level leg.
      int32_t const want{ pitch - apart };
      std::array<bool, 2> const fixed{ level(low), level(high) };
      if (fixed[0] && fixed[1]) { continue; }
      std::array<int32_t, 2> got{ low.pos, high.pos };
      for (uint32_t which = 0; which < 2; ++which) {
        Leg const &leg{ (which == 0) ? low : high };
        if (fixed[which]) { continue; }
        int32_t share{ (which == 0) ? (want / 2) : (want - (want / 2)) };
        if (fixed[1 - which]) { share = want; }
        int32_t const aim{ leg.pos + ((which == 0) ? -share : share) };
        got[which] =
            onto_face(aim, face_run(boxes[leg.box], leg.face, clear, table[leg.slot].arc));
      }
      for (uint32_t which = 0; which < 2; ++which) {
        Leg &leg{ (which == 0) ? low : high };
        if (got[which] == leg.pos) { continue; }
        bool taken{ false };  // another seat of the face holds the point
        for (Leg const &other : legs) {
          taken = taken || ((other.slot != leg.slot) && (other.box == leg.box) &&
                            (other.face == leg.face) && (other.pos == got[which]));
        }
        if (taken) { continue; }
        leg.pos = got[which];
        if (leg.face < 2) {
          at[leg.slot].y = got[which];
        } else {
          at[leg.slot].x = got[which];
        }
        moved = true;
      }
    }
    return moved;
  };

  // Repeats y and x sweeps until neither moves, at most one round per leg.
  for (uint32_t round = 0; round < legs.size(); ++round) {
    bool const y{ sweep(true) };
    bool const x{ sweep(false) };
    if (!y && !x) { break; }
  }
}

SCAV_COLD void ortho_clear_occupied(PodVector<RouteNet> const &nets,
                                    PodVector<scav_rect> const &boxes,
                                    PodVector<Seat> const &table,
                                    PodVector<scav_point> const &toward,
                                    std::vector<OccupiedSpan> const &occupied,
                                    int32_t clear,
                                    PodVector<scav_point> &at,
                                    PodVector<int32_t> &stuck) {
  stuck.assign(nets.size(), 0);
  if (occupied.empty()) { return; }
  thread_local PodVector<uint32_t> cleared;  // slots moved off an occupied span
  cleared.clear();
  for (Seat const &s : table) {
    uint32_t const face{ face_at(s, boxes, at) };
    if (s.inscribed || (face == INVALID)) { continue; }
    scav_point &seat{ at[s.slot] };
    int32_t const was{ (face < 2) ? seat.y : seat.x };
    if (!occupied_at(occupied, s.box, face, was)) { continue; }
    RouteNet const &net{ nets[s.slot / 2] };
    if ((net.loop > 0) &&
        (net.src_obstacle == net.dst_obstacle)) {  // seated by the loop pass
      ++stuck[s.slot / 2];
      continue;
    }
    // The occupied spans, plus `clear` either side of each seat this pass moved off one.
    thread_local std::vector<OccupiedSpan> taken;
    taken = occupied;
    for (uint32_t const k : cleared) {
      uint32_t const at_face{ face_at(table[k], boxes, at) };
      if ((table[k].box != s.box) || (at_face == INVALID)) { continue; }
      int32_t const pos{ (at_face < 2) ? at[k].y : at[k].x };
      vec_push_back(
          taken,
          { .obstacle = s.box, .face = at_face, .lo = pos - clear, .len = 2 * clear });
    }
    // Face `f`'s free position nearest `pos` and its distance from the aim, -1 for none.
    scav_rect const &r{ boxes[s.box] };
    scav_point const aim{ toward[s.slot] };
    auto const free_on = [&](uint32_t f, int32_t pos, scav_point &got) {
      FaceRun const run{ face_run(r, f, clear, s.arc) };
      if ((run.len <= (2 * run.inset)) || !occupied_free(taken,
                                                         s.box,
                                                         f,
                                                         run.lo + run.inset,
                                                         (run.lo + run.len) - run.inset,
                                                         pos)) {
        return Wide{ -1 };
      }
      got = face_middle(r, f);
      if (f < 2) {
        got.y = pos;
      } else {
        got.x = pos;
      }
      return distance(got.x, aim.x) + distance(got.y, aim.y);
    };
    scav_point best{ seat };
    Wide best_gap{ free_on(face, was, best) };
    bool const own{ best_gap >= 0 };
    for (uint32_t f = 0; !own && (f < 4); ++f) {
      if (f == face) { continue; }
      scav_point got{};
      Wide const gap{
        free_on(f, onto_face((f < 2) ? aim.y : aim.x, face_run(r, f, clear, s.arc)), got)
      };
      if ((gap >= 0) && ((best_gap < 0) || (gap < best_gap))) {
        best = got;
        best_gap = gap;
      }
    }
    if (best_gap < 0) {
      ++stuck[s.slot / 2];
      continue;
    }
    seat = best;
    cleared.push_back(s.slot);

    // A far end level with the old seat on a parallel face moves level with the new one.
    Seat const &twin{ table[s.slot ^ 1U] };
    if ((face_of(best, r) != face) || (twin.box >= boxes.size()) || twin.inscribed) {
      continue;
    }
    uint32_t const there{ face_of(at[twin.slot], boxes[twin.box]) };
    int32_t &held{ (face < 2) ? at[twin.slot].y : at[twin.slot].x };
    int32_t const now{ (face < 2) ? best.y : best.x };
    if ((there == INVALID) || ((there < 2) != (face < 2)) || (held != was)) { continue; }
    FaceRun const run{ face_run(boxes[twin.box], there, clear, twin.arc) };
    if ((onto_face(now, run) == now) && !occupied_at(occupied, twin.box, there, now)) {
      held = now;
    }
  }
}

scav_point ortho_escape(scav_point at,
                        scav_point toward,
                        PodVector<scav_rect> const &boxes) {
  uint32_t chosen{ INVALID };
  Wide smallest{ -1 };
  for (uint32_t i = 0; i < boxes.size(); ++i) {
    if (!inside(at, boxes[i])) { continue; }
    Wide const area{ Wide{ boxes[i].w } * boxes[i].h };
    if ((smallest < 0) || (area < smallest)) {
      smallest = area;
      chosen = i;
    }
  }
  return (chosen == INVALID) ? at : ortho_escape_box(at, toward, boxes[chosen]);
}

uint32_t ortho_box_at(scav_point at, PodVector<scav_rect> const &boxes) {
  uint32_t chosen{ INVALID };
  Wide smallest{ -1 };
  for (uint32_t i = 0; i < boxes.size(); ++i) {
    scav_rect const &r{ boxes[i] };
    bool const on_side{ ((at.x == r.x) || (at.x == (r.x + r.w))) && (at.y >= r.y) &&
                        (at.y <= (r.y + r.h)) };
    bool const on_cap{ ((at.y == r.y) || (at.y == (r.y + r.h))) && (at.x >= r.x) &&
                       (at.x <= (r.x + r.w)) };
    if (!on_side && !on_cap) { continue; }
    Wide const area{ Wide{ r.w } * r.h };
    if ((smallest < 0) || (area < smallest)) {
      smallest = area;
      chosen = i;
    }
  }
  return chosen;
}

scav_point ortho_ring(scav_point at, scav_rect const &r, int32_t clear) {
  if (at.x == r.x) { return { .x = at.x - clear, .y = at.y }; }
  if (at.x == (r.x + r.w)) { return { .x = at.x + clear, .y = at.y }; }
  if (at.y == r.y) { return { .x = at.x, .y = at.y - clear }; }
  if (at.y == (r.y + r.h)) { return { .x = at.x, .y = at.y + clear }; }
  return at;
}

void ortho_simplify(PodVector<scav_point> const &from, PodVector<scav_point> &to) {
  for (scav_point const &at : from) {
    // Repeats both rules until neither applies; a pop can expose a duplicate point.
    bool skip{ false };
    for (;;) {
      if (!to.empty() && (to.back().x == at.x) && (to.back().y == at.y)) {
        skip = true;
        break;
      }
      if (to.size() < 2) { break; }
      scav_point const &a{ to[to.size() - 2] };
      scav_point const &b{ to[to.size() - 1] };
      bool const flat{ (a.y == b.y) && (b.y == at.y) };
      bool const upright{ (a.x == b.x) && (b.x == at.x) };
      if (!flat && !upright) { break; }
      to.pop_back();
    }
    if (!skip) { to.push_back(at); }
  }
}

SCAV_COLD void ortho_enclosure_walls(scav_rect const &region,
                                     scav_rect const &enclosure,
                                     int32_t inset,
                                     PodVector<scav_rect> &out) {
  out.clear();
  if ((enclosure.w <= 0) || (enclosure.h <= 0)) { return; }
  Wide const rx0{ region.x };
  Wide const ry0{ region.y };
  Wide const rx1{ Wide{ region.x } + region.w };
  Wide const ry1{ Wide{ region.y } + region.h };
  Wide const ex0{ Wide{ enclosure.x } + inset };
  Wide const ey0{ Wide{ enclosure.y } + inset };
  Wide const ex1{ (Wide{ enclosure.x } + enclosure.w) - inset };
  Wide const ey1{ (Wide{ enclosure.y } + enclosure.h) - inset };
  auto const wall = [&out](Wide x0, Wide y0, Wide x1, Wide y1) {
    if ((x1 <= x0) || (y1 <= y0)) { return; }
    out.push_back({ .x = static_cast<int32_t>(x0),
                    .y = static_cast<int32_t>(y0),
                    .w = static_cast<int32_t>(x1 - x0),
                    .h = static_cast<int32_t>(y1 - y0) });
  };
  wall(rx0, ry0, ex0, ry1);
  wall(ex1, ry0, rx1, ry1);
  wall(ex0, ry0, ex1, ey0);
  wall(ex0, ey1, ex1, ry1);
}

bool ortho_grid(scav_rect const &region,
                PodVector<scav_rect> const &obstacles,
                PodVector<scav_point> const &anchors,
                int32_t clear,
                OrthoGrid &out,
                PodVector<scav_rect> const *fixed) {
  int32_t const lo_x{ region.x };
  int32_t const hi_x{ region.x + region.w };
  int32_t const lo_y{ region.y };
  int32_t const hi_y{ region.y + region.h };

  out.xs.clear();
  out.ys.clear();
  out.pass_h.clear();
  out.pass_v.clear();
  out.xs.push_back(lo_x);
  out.xs.push_back(hi_x);
  out.ys.push_back(lo_y);
  out.ys.push_back(hi_y);
  // A line at each obstacle side grown by `clear`, where ring points sit.
  for (scav_rect const &r : obstacles) {
    for (int32_t const at : { r.x - clear, r.x + r.w + clear }) {
      if ((at > lo_x) && (at < hi_x)) { out.xs.push_back(at); }
    }
    for (int32_t const at : { r.y - clear, r.y + r.h + clear }) {
      if ((at > lo_y) && (at < hi_y)) { out.ys.push_back(at); }
    }
  }
  if (fixed != nullptr) {
    for (scav_rect const &r : *fixed) {
      for (int32_t const at : { r.x, r.x + r.w }) {
        if ((at > lo_x) && (at < hi_x)) { out.xs.push_back(at); }
      }
      for (int32_t const at : { r.y, r.y + r.h }) {
        if ((at > lo_y) && (at < hi_y)) { out.ys.push_back(at); }
      }
    }
  }
  for (scav_point const &at : anchors) {
    // An anchor outside the region adds neither of its lines.
    if ((at.x < lo_x) || (at.x > hi_x) || (at.y < lo_y) || (at.y > hi_y)) { continue; }
    out.xs.push_back(at.x);
    out.ys.push_back(at.y);
  }
  ortho_sort_unique(out.xs);
  ortho_sort_unique(out.ys);
  if ((Wide{ out.nx() } * out.ny()) > ORTHO_VERTEX_BUDGET) { return false; }

  out.pass_h.assign(static_cast<size_t>(out.ny()) * (out.nx() - 1), 1);
  out.pass_v.assign(static_cast<size_t>(out.ny() - 1) * out.nx(), 1);
  auto const block = [&out](scav_rect const &r) {
    if (r.w > 0) {
      // y strictly inside the box, and the cell [xs[ix], xs[ix+1]] overlapping it.
      uint32_t const iy0{ ortho_after(out.ys, r.y) };
      uint32_t const iy1{ ortho_from(out.ys, r.y + r.h) };
      uint32_t const after_x{ ortho_after(out.xs, r.x) };
      uint32_t const ix0{ (after_x == 0) ? 0U : (after_x - 1U) };
      uint32_t const ix1{ imin(ortho_from(out.xs, r.x + r.w), out.nx() - 1) };
      for (uint32_t iy = iy0; iy < iy1; ++iy) {
        uint8_t *const row{ out.pass_h.data() +
                            (static_cast<size_t>(iy) * (out.nx() - 1)) };
        for (uint32_t ix = ix0; ix < ix1; ++ix) { row[ix] = 0; }
      }
    }
    if (r.h > 0) {
      uint32_t const ix0{ ortho_after(out.xs, r.x) };
      uint32_t const ix1{ ortho_from(out.xs, r.x + r.w) };
      uint32_t const after_y{ ortho_after(out.ys, r.y) };
      uint32_t const iy0{ (after_y == 0) ? 0U : (after_y - 1U) };
      uint32_t const iy1{ imin(ortho_from(out.ys, r.y + r.h), out.ny() - 1) };
      for (uint32_t iy = iy0; iy < iy1; ++iy) {
        uint8_t *const row{ out.pass_v.data() + (static_cast<size_t>(iy) * out.nx()) };
        for (uint32_t ix = ix0; ix < ix1; ++ix) { row[ix] = 0; }
      }
    }
  };
  // Blocks each box grown by `clear`, keeping every route at least `clear` from a box.
  // `block` solves `ortho_blocks_h` and `ortho_blocks_v` for index ranges.
  for (scav_rect const &box : obstacles) { block(grow(box, clear)); }
  if (fixed != nullptr) {
    for (scav_rect const &r : *fixed) { block(r); }
  }
  return true;
}

namespace {

// The search behind `ortho_search` and `ortho_search_planes`, from `starts` when set, else
// `from`; stops at the first goal unless `both`, then at each plane `to_plane` allows.
bool search_planes(OrthoGrid const &g,
                   uint32_t from,
                   uint32_t to,
                   Wide bend,
                   OrthoScratch &s,
                   std::array<Wide, 2> const &seed,
                   std::array<int32_t, 2> const &heading,
                   uint32_t from_plane,
                   uint32_t to_plane,
                   uint32_t waypoints,
                   bool both,
                   PodVector<uint32_t> const *starts,
                   PodVector<uint32_t> &hops,
                   std::array<OrthoFinish, 2> &out) {
  out = {};
  uint32_t const vertices{ g.nx() * g.ny() };
  if ((vertices == 0) || (from >= vertices) || (to >= vertices)) { return false; }
  if (g.pass_h.empty() && (g.nx() > 1)) { return false; }
  uint32_t const nodes{ vertices * 2 };
  // Only grown; every stamp in it predates this search's generation.
  if (s.state.size() < nodes) {
    s.state.resize(nodes, OrthoNodeState{ .stamp = 0, .parent = INVALID, .best = 0 });
  }
  if (++s.generation == 0) {
    for (OrthoNodeState &n : s.state) { n.stamp = 0; }
    s.generation = 1;
  }
  uint32_t const gen{ s.generation };
  OrthoNodeState *const state{ s.state.data() };
  OrthoRadixHeap &open{ s.open };
  uint32_t const nx{ g.nx() };
  uint32_t const ny{ g.ny() };
  int32_t const *const xs{ g.xs.data() };
  int32_t const *const ys{ g.ys.data() };
  uint8_t const *const pass_h{ g.pass_h.data() };
  uint8_t const *const pass_v{ g.pass_v.data() };
  scav_point const goal{ g.point(to) };

  // Weights in quarter units. An end turn costs a quarter less than a bend: ties in length
  // and bends go to the path turning at its ends.
  constexpr Wide QUARTERS{ 4 };
  Wide const turn_w{ QUARTERS * bend };
  Wide const end_turn_w{ (bend > 0) ? (turn_w - 1) : turn_w };
  // A turn at a waypoint costs a quarter less again: ties go to turning where the corridor
  // turns. `waypoints` bit 0 marks `from` as one, bit 1 `to`.
  Wide const way_turn_w{ (bend > 0) ? (turn_w - 2) : turn_w };

  // Admissible: Manhattan distance, a turn when this plane cannot reach the goal alone,
  // and an end turn to finish in a fixed goal plane.
  bool const fixed_goal{ to_plane < 2 };
  auto const heuristic = [&](Wide dx, Wide dy, uint32_t plane) {
    bool const turn{ (plane == 0) ? (dy != 0) : (dx != 0) };
    uint32_t const last{ turn ? (1U - plane) : plane };
    bool const settle{ fixed_goal && (last != to_plane) };
    return (QUARTERS * (dx + dy)) + (turn ? turn_w : Wide{ 0 }) +
           (settle ? end_turn_w : Wide{ 0 });
  };

  ortho_open_clear(open);
  std::array<uint32_t, 1> const one{ from };
  uint32_t const *const first{ (starts != nullptr) ? starts->data() : one.data() };
  size_t const count{ (starts != nullptr) ? starts->size() : 1 };
  for (size_t i = 0; i < count; ++i) {
    uint32_t const at{ first[i] };
    if (at >= vertices) { continue; }
    Wide const dx{ distance(xs[at % nx], goal.x) };
    Wide const dy{ distance(ys[at / nx], goal.y) };
    // Each seeded plane starts at its seed; the turn reaches the other.
    for (uint32_t plane = 0; plane < 2; ++plane) {
      if (seed[plane] < 0) { continue; }
      uint32_t const node{ (at * 2) + plane };
      state[node] = { .stamp = gen, .parent = INVALID, .best = seed[plane] };
      ortho_open_push(
          open,
          { .f = seed[plane] + heuristic(dx, dy, plane), .g = seed[plane], .node = node });
    }
  }

  // Appends the path to `node` from its seed to `hops` as `out[plane]`.
  auto const finish = [&](uint32_t node, Wide cost) {
    s.path.clear();
    uint32_t root{ node };
    for (uint32_t at = node; at != INVALID; at = state[at].parent) {
      s.path.push_back(at / 2);
      root = at;
    }
    uint32_t const off{ static_cast<uint32_t>(hops.size()) };
    for (auto i = static_cast<uint32_t>(s.path.size()); i-- > 0;) {
      if ((hops.size() == off) || (hops.back() != s.path[i])) {
        hops.push_back(s.path[i]);
      }
    }
    // The last move's direction along the goal's plane; 0 after a turn at the goal.
    uint32_t const before{ state[node].parent };
    int32_t way{ 0 };
    if ((before != INVALID) && ((before / 2) != (node / 2))) {
      scav_point const a{ g.point(before / 2) };
      scav_point const b{ g.point(node / 2) };
      bool const ahead{ ((node % 2) == 0) ? (b.x > a.x) : (b.y > a.y) };
      way = ahead ? 1 : -1;
    }
    out[node % 2] = { .cost = cost,
                      .start = root % 2,
                      .heading = way,
                      .hops = { .off = off,
                                .len = static_cast<uint32_t>(hops.size()) - off } };
  };

  uint32_t expansions{ 0 };
  bool reached{ false };
  while (!ortho_open_empty(open)) {
    OrthoFrontierEntry const top{ ortho_open_pop(open) };
    uint32_t const node{ top.node };
    OrthoNodeState const &here{ state[node] };
    if ((here.stamp != gen) || (top.g != here.best)) { continue; }
    uint32_t const v{ node / 2 };
    uint32_t const plane{ node % 2 };
    if ((v == to) && (!fixed_goal || (plane == to_plane)) && (out[plane].cost < 0)) {
      finish(node, top.g);
      reached = true;
      if (!both || fixed_goal || (out[plane ^ 1U].cost >= 0)) { break; }
    }
    if (++expansions > ORTHO_EXPANSION_BUDGET) { return reached; }

    uint32_t const iy{ v / nx };
    uint32_t const ix{ v - (iy * nx) };
    Wide const top_g{ top.g };
    Wide const dx{ distance(xs[ix], goal.x) };
    Wide const dy{ distance(ys[iy], goal.y) };

    auto const relax = [&](uint32_t next, Wide step, Wide h) {
      Wide const g_next{ top_g + step };
      OrthoNodeState &there{ state[next] };
      if ((there.stamp == gen) && (there.best <= g_next)) { return; }
      there = { .stamp = gen, .parent = node, .best = g_next };
      ortho_open_push(open, { .f = g_next + h, .g = g_next, .node = next });
    };

    // Relaxes the plane switch first, at end-turn weight at a fixed start or goal, then
    // this plane's grid moves.
    bool const start{ (starts != nullptr) ? (here.parent == INVALID) : (v == from) };
    bool const end_turn{ (start && (plane == from_plane)) || ((v == to) && fixed_goal) };
    bool const way_turn{ (start && ((waypoints & 1U) != 0)) ||
                         ((v == to) && ((waypoints & 2U) != 0)) };
    Wide switch_w{ end_turn ? end_turn_w : turn_w };
    if (way_turn) { switch_w = way_turn_w; }
    relax(node ^ 1U, switch_w, heuristic(dx, dy, 1 - plane));
    // A seed's first move back against its heading turns twice.
    int32_t const back{ (here.parent == INVALID) ? -heading[plane] : 0 };
    Wide const up_turn{ (back == 1) ? (2 * turn_w) : Wide{ 0 } };
    Wide const down_turn{ (back == -1) ? (2 * turn_w) : Wide{ 0 } };
    if (plane == 0) {
      uint8_t const *const row{ pass_h + (static_cast<size_t>(iy) * (nx - 1)) };
      if (((ix + 1) < nx) && (row[ix] != 0)) {
        relax(node + 2,
              up_turn + (QUARTERS * (Wide{ xs[ix + 1] } - xs[ix])),
              heuristic(distance(xs[ix + 1], goal.x), dy, 0));
      }
      if ((ix > 0) && (row[ix - 1] != 0)) {
        relax(node - 2,
              down_turn + (QUARTERS * (Wide{ xs[ix] } - xs[ix - 1])),
              heuristic(distance(xs[ix - 1], goal.x), dy, 0));
      }
    } else {
      uint32_t const down{ 2 * nx };
      if (((iy + 1) < ny) && (pass_v[(static_cast<size_t>(iy) * nx) + ix] != 0)) {
        relax(node + down,
              up_turn + (QUARTERS * (Wide{ ys[iy + 1] } - ys[iy])),
              heuristic(dx, distance(ys[iy + 1], goal.y), 1));
      }
      if ((iy > 0) && (pass_v[(static_cast<size_t>(iy - 1) * nx) + ix] != 0)) {
        relax(node - down,
              down_turn + (QUARTERS * (Wide{ ys[iy] } - ys[iy - 1])),
              heuristic(dx, distance(ys[iy - 1], goal.y), 1));
      }
    }
  }
  return reached;
}

}  // namespace

bool ortho_search(OrthoGrid const &g,
                  uint32_t from,
                  uint32_t to,
                  Wide bend,
                  OrthoScratch &s,
                  PodVector<uint32_t> &out,
                  uint32_t from_plane,
                  uint32_t to_plane) {
  out.clear();
  std::array<OrthoFinish, 2> finishes{};
  std::array<Wide, 2> const seed{ (from_plane == 1) ? -1 : 0, (from_plane == 0) ? -1 : 0 };
  std::array<int32_t, 2> const either{ 0, 0 };
  if (!search_planes(g,
                     from,
                     to,
                     bend,
                     s,
                     seed,
                     either,
                     from_plane,
                     to_plane,
                     0,
                     false,
                     nullptr,
                     out,
                     finishes)) {
    out.clear();
    return false;
  }
  return true;
}

bool ortho_search_planes(OrthoGrid const &g,
                         uint32_t from,
                         uint32_t to,
                         Wide bend,
                         OrthoScratch &s,
                         std::array<Wide, 2> const &seed,
                         std::array<int32_t, 2> const &heading,
                         uint32_t from_plane,
                         uint32_t to_plane,
                         uint32_t waypoints,
                         PodVector<uint32_t> &hops,
                         std::array<OrthoFinish, 2> &out,
                         PodVector<uint32_t> const *starts,
                         bool both) {
  return search_planes(g,
                       from,
                       to,
                       bend,
                       s,
                       seed,
                       heading,
                       from_plane,
                       to_plane,
                       waypoints,
                       both,
                       starts,
                       hops,
                       out);
}

Wide ortho_bend_penalty(scav_profile const &p) { return route_bend_penalty(p); }

int32_t ortho_clearance(scav_profile const &p) { return box_clearance(p); }

int32_t OrthogonalRouter::margin(scav_profile const &p) const {
  return ortho_clearance(p);
}

namespace {

// Every buffer one `route` call uses, per thread and reassigned in place; a thread runs
// one `route` call at a time.
struct RouteScratch {
  PodVector<scav_rect> walls;
  PodVector<scav_point> lead, anchors, seat, toward, was, unspread;
  PodVector<uint32_t> ends,
      groups;  // `groups`: slots of each face with a shared point, INVALID-ended
  PodVector<scav_span> net_anchors, net_lead, net_tail;
  OrthoGrid g, tight, open;  // `open` blocks boxes only
  PodVector<scav_rect> boxes;
  OrthoScratch search;
  PodVector<uint32_t> hop;
  PodVector<std::array<OrthoFinish, 2>> finishes;
  PodVector<uint32_t> chosen;
  PodVector<uint32_t> starts;
  PodVector<scav_point> rebuilt;
  PodVector<int32_t> beside;
  PodVector<uint16_t> holding;
  PodVector<uint8_t> kept;
  PodVector<scav_point> piece, shape;
  PodVector<Seat> seats;
  PodVector<int32_t> stuck;
  PodVector<uint32_t> closed;       // `close_route`'s record of the passes it cleared
  PodVector<uint32_t> stub_closed;  // `close_stub`'s
};

RouteScratch &route_scratch() {
  thread_local RouteScratch s;
  return s;
}

// The turns of the `len` points at `pts`, each change between horizontal and vertical.
uint32_t polyline_bends(scav_point const *pts, uint32_t len) {
  uint32_t bends{ 0 };
  for (uint32_t k = 1; (k + 1) < len; ++k) {
    bool const flat_in{ pts[k - 1].y == pts[k].y };
    bool const flat_out{ pts[k].y == pts[k + 1].y };
    bends += (flat_in != flat_out) ? 1U : 0U;
  }
  return bends;
}

// The axis-aligned length of the `len` points at `pts`.
Wide polyline_length(scav_point const *pts, uint32_t len) {
  Wide sum{ 0 };
  for (uint32_t k = 1; k < len; ++k) {
    sum += distance(pts[k - 1].x, pts[k].x) + distance(pts[k - 1].y, pts[k].y);
  }
  return sum;
}

// How many times a horizontal piece of one polyline crosses a vertical piece of the other
// strictly inside both.
uint32_t polyline_crossings(scav_point const *a,
                            uint32_t a_len,
                            scav_point const *b,
                            uint32_t b_len) {
  uint32_t count{ 0 };
  auto const cut = [](scav_point h0, scav_point h1, scav_point v0, scav_point v1) {
    return (h0.y == h1.y) && (v0.x == v1.x) && (v0.x > imin(h0.x, h1.x)) &&
           (v0.x < imax(h0.x, h1.x)) && (h0.y > imin(v0.y, v1.y)) &&
           (h0.y < imax(v0.y, v1.y));
  };
  for (uint32_t i = 1; i < a_len; ++i) {
    for (uint32_t j = 1; j < b_len; ++j) {
      bool const crossed{ cut(a[i - 1], a[i], b[j - 1], b[j]) ||
                          cut(b[j - 1], b[j], a[i - 1], a[i]) };
      count += crossed ? 1U : 0U;
    }
  }
  return count;
}

// Calls `visit(vertical, i)` for each pass of `g` crossing, touching or along an
// axis-aligned piece of the `len` points at `pts`; `i` indexes `pass_v` or `pass_h`.
template <typename Visit>
void route_passes(OrthoGrid const &g, scav_point const *pts, uint32_t len, Visit &&visit) {
  uint32_t const nx{ g.nx() };
  uint32_t const ny{ g.ny() };
  if ((nx < 2) || (ny < 2)) { return; }
  for (uint32_t k = 0; (k + 1) < len; ++k) {
    scav_point const a{ pts[k] };
    scav_point const b{ pts[k + 1] };
    bool const upright{ (a.x == b.x) && (a.y != b.y) };
    if (!upright && ((a.y != b.y) || (a.x == b.x))) { continue; }
    // The piece lies on `on` among the lines `fix` and spans `[lo, hi]` among `run`.
    PodVector<int32_t> const &fix{ upright ? g.xs : g.ys };
    PodVector<int32_t> const &run{ upright ? g.ys : g.xs };
    int32_t const on{ upright ? a.x : a.y };
    int32_t const lo{ upright ? imin(a.y, b.y) : imin(a.x, b.x) };
    int32_t const hi{ upright ? imax(a.y, b.y) : imax(a.x, b.x) };
    uint32_t const nf{ static_cast<uint32_t>(fix.size()) };
    uint32_t const nr{ static_cast<uint32_t>(run.size()) };
    // The pass from line `f` to `f + 1` on line `r`, and from `r` to `r + 1` on line `f`.
    auto const across = [&](uint32_t f, uint32_t r) {
      return upright ? ((r * (nx - 1)) + f) : ((f * nx) + r);
    };
    auto const along = [&](uint32_t f, uint32_t r) {
      return upright ? ((r * nx) + f) : ((f * (nx - 1)) + r);
    };
    uint32_t const f{ ortho_index_of(fix, on) };
    if (fix[f] > on) { continue; }
    bool const on_line{ fix[f] == on };
    uint32_t r0{ ortho_index_of(run, lo) };
    if (run[r0] < lo) { ++r0; }
    for (uint32_t r = r0; (r < nr) && (run[r] <= hi); ++r) {
      if ((f + 1) < nf) { visit(!upright, across(f, r)); }
      if (on_line && (f > 0)) { visit(!upright, across(f - 1, r)); }
    }
    for (uint32_t r = ortho_index_of(run, lo); on_line && ((r + 1) < nr) && (run[r] < hi);
         ++r) {
      if (run[r + 1] > lo) { visit(upright, along(f, r)); }
    }
  }
}

// Clears each open pass `route_passes` names for the `len` points at `pts`, recorded in
// `closed` as `2 * i`, or `2 * i + 1` in `pass_v`.
SCAV_COLD void close_route(OrthoGrid &g,
                           scav_point const *pts,
                           uint32_t len,
                           PodVector<uint32_t> &closed) {
  route_passes(g, pts, len, [&](bool vertical, uint32_t i) {
    PodVector<uint8_t> &pass{ vertical ? g.pass_v : g.pass_h };
    if (pass[i] == 0) { return; }
    pass[i] = 0;
    closed.push_back((2 * i) + (vertical ? 1U : 0U));
  });
}

// Reopens the passes `close_route` recorded in `closed`.
// Shuts the pass from grid point `stub` toward `exact` on their shared line, recording it
// in `closed` as `close_route` does.
SCAV_COLD void close_stub(OrthoGrid &g,
                          scav_point stub,
                          scav_point exact,
                          PodVector<uint32_t> &closed) {
  uint32_t const nx{ g.nx() };
  uint32_t const ix{ ortho_index_of(g.xs, stub.x) };
  uint32_t const iy{ ortho_index_of(g.ys, stub.y) };
  if ((nx < 2) || (g.ny() < 2) || (g.xs[ix] != stub.x) || (g.ys[iy] != stub.y)) { return; }
  bool const upright{ (exact.x == stub.x) && (exact.y != stub.y) };
  bool const flat{ (exact.y == stub.y) && (exact.x != stub.x) };
  uint32_t edge{ INVALID };
  if (flat && (exact.x > stub.x) && ((ix + 1) < nx)) { edge = (iy * (nx - 1)) + ix; }
  if (flat && (exact.x < stub.x) && (ix > 0)) { edge = (iy * (nx - 1)) + (ix - 1); }
  if (upright && (exact.y > stub.y) && ((iy + 1) < g.ny())) { edge = (iy * nx) + ix; }
  if (upright && (exact.y < stub.y) && (iy > 0)) { edge = ((iy - 1) * nx) + ix; }
  if (edge == INVALID) { return; }
  PodVector<uint8_t> &pass{ upright ? g.pass_v : g.pass_h };
  if (pass[edge] == 0) { return; }
  pass[edge] = 0;
  closed.push_back((2 * edge) + (upright ? 1U : 0U));
}

void reopen_route(OrthoGrid &g, PodVector<uint32_t> const &closed) {
  for (uint32_t const e : closed) { (((e & 1U) != 0) ? g.pass_v : g.pass_h)[e >> 1U] = 1; }
}

// The search plane a leg from `a` to `b` runs in: 0 horizontal, 1 vertical,
// INVALID for a point or a diagonal.
uint32_t leg_plane(scav_point a, scav_point b) {
  if ((a.y == b.y) && (a.x != b.x)) { return 0; }
  if ((a.x == b.x) && (a.y != b.y)) { return 1; }
  return INVALID;
}

// What an end is aimed at: its nearest corridor point, or the other end with no corridor.
scav_point aim_of(RouteInput const &in, RouteNet const &net, uint32_t end) {
  if (net.waypoint_len == 0) { return (end == 0) ? net.dst : net.src; }
  return in.waypoints[net.waypoint_off + ((end == 0) ? 0 : (net.waypoint_len - 1))];
}

// An end's seat before the seat passes: on `face` when below 4, else by the escape rule;
// the aim for an end naming no box.
scav_point seat_of(RouteInput const &in,
                   RouteNet const &net,
                   uint32_t end,
                   uint32_t face,
                   int32_t clear) {
  scav_point const aim{ aim_of(in, net, end) };
  uint32_t const box{ (end == 0) ? net.src_obstacle : net.dst_obstacle };
  if (box >= in.obstacles.size()) { return aim; }
  bool const glyph{ (box < in.inscribed.size()) && (in.inscribed[box] != 0) };
  int32_t const arc{ (box < in.corner.size()) ? in.corner[box] : 0 };
  if (face < 4) {
    return ortho_attach_face(aim, in.obstacles[box], clear, glyph, arc, face);
  }
  return ortho_attach_box(aim, in.obstacles[box], clear, glyph, arc);
}

// How far a loop runs off `face` of obstacle `box` between its ends `at`: `loop`, held
// `clear` off every other obstacle beyond the face across that span, and at least `clear`.
int32_t loop_reach(RouteInput const &in,
                   int32_t loop,
                   uint32_t box,
                   uint32_t face,
                   std::array<scav_point, 2> const &at,
                   int32_t clear) {
  if (face >= 4) { return loop; }
  scav_rect const &r{ in.obstacles[box] };
  bool const along_y{ face < 2 };
  int32_t const lo{ imin(along_y ? at[0].y : at[0].x, along_y ? at[1].y : at[1].x) -
                    clear };
  int32_t const hi{ imax(along_y ? at[0].y : at[0].x, along_y ? at[1].y : at[1].x) +
                    clear };
  int32_t reach{ loop };
  for (uint32_t o = 0; o < in.obstacles.size(); ++o) {
    scav_rect const &q{ in.obstacles[o] };
    int32_t const q_lo{ along_y ? q.y : q.x };
    int32_t const q_hi{ q_lo + (along_y ? q.h : q.w) };
    if ((o == box) || (q_hi <= lo) || (q_lo >= hi)) { continue; }
    int32_t gap{ -1 };  // from the face out to `q`'s near side; negative when not beyond
    switch (face) {
      case 0: gap = r.x - (q.x + q.w); break;
      case 1: gap = q.x - (r.x + r.w); break;
      case 2: gap = r.y - (q.y + q.h); break;
      default: gap = q.y - (r.y + r.h); break;
    }
    if (gap >= 0) { reach = imin(reach, gap - clear); }
  }
  return imax(reach, clear);
}

// The face of loop `n`'s box holding the fewest other ends, unpinned loops excluded; ties
// prefer right, then bottom, top, left.
uint32_t loop_face(RouteInput const &in, uint32_t n, int32_t clear) {
  RouteNet const &net{ in.nets[n] };
  uint32_t const b{ net.src_obstacle };
  if (b >= in.obstacles.size()) { return INVALID; }
  scav_rect const &r{ in.obstacles[b] };
  std::array<uint32_t, 4> use{};
  for (uint32_t m = 0; m < in.nets.size(); ++m) {
    RouteNet const &other{ in.nets[m] };
    for (uint32_t end = 0; (m != n) && (end < 2); ++end) {
      uint32_t const named{ (end == 0) ? other.src_obstacle : other.dst_obstacle };
      uint32_t const pinned{ (end == 0) ? other.src_face : other.dst_face };
      uint32_t face{ INVALID };
      if (named == b) {
        if ((other.loop > 0) && (pinned >= 4)) { continue; }
        face = face_of(seat_of(in, other, end, pinned, clear), r);
      } else if (named >= in.obstacles.size()) {
        face = face_of((end == 0) ? other.src : other.dst, r);
      }
      if (face < 4) { ++use[face]; }
    }
  }
  uint32_t best{ 1 };
  for (uint32_t const face : { 3U, 2U, 0U }) {
    if (use[face] < use[best]) { best = face; }
  }
  return best;
}

// `seat_of` for net `n`; a loop with no named face takes `loop_face`.
scav_point seat_at(RouteInput const &in,
                   uint32_t n,
                   uint32_t end,
                   uint32_t face,
                   int32_t clear) {
  RouteNet const &net{ in.nets[n] };
  if ((face >= 4) && (net.loop > 0)) { face = loop_face(in, n, clear); }
  return seat_of(in, net, end, face, clear);
}

}  // namespace

uint32_t OrthogonalRouter::seat_face(scav_rect const &r, scav_point aim) const {
  return ortho_escape_face(aim, r);
}

SCAV_COLD uint32_t OrthogonalRouter::effective_faces(RouteInput const &in,
                                                     uint32_t net,
                                                     uint32_t end) const {
  RouteNet const &nt{ in.nets[net] };
  if (((end == 0) ? nt.src_obstacle : nt.dst_obstacle) >= in.obstacles.size()) {
    return 0;
  }
  int32_t const clear{ route_clearance(in.profile) };
  scav_point const ruled{ seat_at(in, net, end, INVALID, clear) };
  uint32_t out{ 0 };
  for (uint32_t face = 0; face < 4; ++face) {
    if (!same(seat_of(in, nt, end, face, clear), ruled)) { out |= 1U << face; }
  }
  return out;
}

void OrthogonalRouter::route(RouteInput const &in, RouteOutput &out) const {
  out.points.clear();
  out.net_points.clear();
  out.metrics.clear();
  out.net_points.reserve(in.nets.size());
  out.metrics.reserve(in.nets.size());

  Wide const bend{ ortho_bend_penalty(in.profile) };
  int32_t const clear{ route_clearance(in.profile) };  // between seats
  int32_t const bumper{ ortho_clearance(in.profile) };
  int32_t const inset{ border_band(in.profile) };
  RouteScratch &sc{ route_scratch() };
  PodVector<scav_rect> &walls{ sc.walls };
  ortho_enclosure_walls(in.region, in.enclosure, inset, walls);
  walls.insert(walls.end(), in.gaps.begin(), in.gaps.end());
  scav_rect const &enc{ in.enclosure };
  // The enclosure border whose band holds an end: 0 left, 1 right, 2 top, 3 bottom,
  // INVALID for none.
  auto const band_side = [&enc, inset](scav_point at) {
    if ((enc.w <= 0) || (enc.h <= 0)) { return INVALID; }
    if ((at.x < enc.x) || (at.x > (enc.x + enc.w)) || (at.y < enc.y) ||
        (at.y > (enc.y + enc.h))) {
      return INVALID;
    }
    std::array<Wide, 4> const gap{ Wide{ at.x } - enc.x,
                                   (Wide{ enc.x } + enc.w) - at.x,
                                   Wide{ at.y } - enc.y,
                                   (Wide{ enc.y } + enc.h) - at.y };
    uint32_t side{ 0 };
    for (uint32_t k = 1; k < 4; ++k) {
      if (gap[k] < gap[side]) { side = k; }
    }
    return (gap[side] < inset) ? side : INVALID;
  };

  // Routes stay inside the region; an anchor outside it degrades only its own net.
  int32_t const lo_x{ in.region.x };
  int32_t const hi_x{ in.region.x + in.region.w };
  int32_t const lo_y{ in.region.y };
  int32_t const hi_y{ in.region.y + in.region.h };
  auto const in_region = [&](scav_point at) {
    return (at.x >= lo_x) && (at.x <= hi_x) && (at.y >= lo_y) && (at.y <= hi_y);
  };

  // Each end is up to three points: the caller's, the border seat, and the ring `bumper`
  // out; the search runs between rings.
  PodVector<scav_point> &lead{ sc.lead };
  PodVector<scav_point> &anchors{ sc.anchors };
  PodVector<scav_span> &net_anchors{ sc.net_anchors };
  PodVector<scav_span> &net_lead{ sc.net_lead };
  PodVector<scav_span> &net_tail{ sc.net_tail };
  lead.clear();
  anchors.clear();
  net_anchors.assign(in.nets.size(), scav_span{});
  net_lead.assign(in.nets.size(), scav_span{});
  net_tail.assign(in.nets.size(), scav_span{});

  auto const approach =
      [&](scav_point exact, uint32_t named, scav_point seated, int32_t keep) {
        uint32_t box{ named };
        scav_point attach{ exact };
        if (box < in.obstacles.size()) {
          // `exact` is the named box's centre; the end attaches at `seated` instead.
          attach = seated;
        } else {
          scav_point const moved{ ortho_escape(exact, seated, in.obstacles) };
          if ((moved.x != exact.x) || (moved.y != exact.y)) {
            // An exact end inside a box is kept, with a stub out to the box's border.
            lead.push_back(exact);
            attach = moved;
          }
          // An end in the enclosure's band attaches to the enclosure even where a box
          // touches it; its stub runs square to that border, to the band's inner edge.
          uint32_t const side{ band_side(attach) };
          box = (side != INVALID) ? INVALID : ortho_box_at(attach, in.obstacles);
          if (side != INVALID) {
            lead.push_back(attach);
            scav_point stub{ attach };
            switch (side) {
              case 0: stub.x = enc.x + inset; break;
              case 1: stub.x = (enc.x + enc.w) - inset; break;
              case 2: stub.y = enc.y + inset; break;
              default: stub.y = (enc.y + enc.h) - inset; break;
            }
            return stub;
          }
        }
        scav_point at{ attach };
        if (box < in.obstacles.size()) {
          // The ring, clamped into the region; unused at `attach` or in a box or wall.
          scav_point ring{ ortho_ring(attach, in.obstacles[box], imax(bumper, keep)) };
          ring.x = imin(imax(ring.x, lo_x), hi_x);
          ring.y = imin(imax(ring.y, lo_y), hi_y);
          bool ok{ (ring.x != attach.x) || (ring.y != attach.y) };
          for (scav_rect const &r : in.obstacles) {
            if (inside(ring, r)) { ok = false; }
          }
          for (scav_rect const &r : walls) {
            if (inside(ring, r)) { ok = false; }
          }
          if (ok) {
            lead.push_back(attach);
            at = ring;
          }
        }
        return at;
      };

  // A stubbed end's leg runs straight from the end to its stub, where the search ends.
  auto const stub_end = [&lead](scav_point exact, scav_point stub) {
    lead.push_back(exact);
    return stub;
  };
  // Whether net `nt`'s end `end` names no box and a stub off it.
  auto const stubbed = [&in](RouteNet const &nt, uint32_t end) {
    bool const named{ ((end == 0) ? nt.src_obstacle : nt.dst_obstacle) <
                      in.obstacles.size() };
    return !named && (((end == 0) ? nt.src_stubbed : nt.dst_stubbed) != 0) &&
           !same((end == 0) ? nt.src_stub : nt.dst_stub, (end == 0) ? nt.src : nt.dst);
  };

  // Seats every end before approaching any net.
  PodVector<scav_point> &seat{ sc.seat };
  PodVector<scav_point> &toward{ sc.toward };
  seat.assign(2 * in.nets.size(), scav_point{});
  toward.assign(2 * in.nets.size(), scav_point{});
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    uint32_t const src_slot{ 2 * n };
    toward[src_slot] = aim_of(in, net, 0);
    toward[src_slot + 1] = aim_of(in, net, 1);
    seat[src_slot] = seat_at(in, n, 0, net.src_face, clear);
    seat[src_slot + 1] = seat_at(in, n, 1, net.dst_face, clear);
    if ((net.loop > 0) && (net.src_face >= 4) &&
        (net.src_obstacle < in.obstacles.size())) {
      trace_emit(
          { .kind = TraceKind::LoopFaced,
            .port = { .seg = net.seg,
                      .trans = net.trans,
                      .leg = n,
                      .side = face_of(seat[src_slot], in.obstacles[net.src_obstacle]) } });
    }
  }
  // Emits `SeatMoved` for each seat the last pass moved.
  PodVector<scav_point> &was{ sc.was };
  was = toward;
  auto const moved = [&](SeatPass pass) {
    if (trace_sink() == nullptr) { return; }
    for (uint32_t i = 0; i < seat.size(); ++i) {
      if ((seat[i].x == was[i].x) && (seat[i].y == was[i].y)) { continue; }
      trace_emit({ .kind = TraceKind::SeatMoved,
                   .pass = static_cast<uint16_t>(pass),
                   .seat = { .net = i / 2,
                             .end = i % 2,
                             .from_x = was[i].x,
                             .from_y = was[i].y,
                             .to_x = seat[i].x,
                             .to_y = seat[i].y } });
    }
    was = seat;
  };
  moved(SeatPass::Attach);

  PodVector<Seat> &seats{ sc.seats };
  ortho_seats(in.nets, in.inscribed, in.corner, seats);
  // Refacing runs first; alignment and spreading read the faces it settles.
  ortho_reface_attachments(in.obstacles, seats, toward, seat);
  moved(SeatPass::Reface);
  ortho_align_attachments(in.nets, in.obstacles, seats, seat);
  moved(SeatPass::Align);
  int32_t const pitch{ label_line_height(in.profile) };
  PodVector<scav_point> &unspread{ sc.unspread };
  unspread = seat;
  ortho_spread_attachments(in.obstacles, seats, toward, clear, pitch, seat);
  moved(SeatPass::Spread);
  // The only pass comparing seats on two boxes, over the seats the passes above settled.
  ortho_separate_attachments(in.nets, in.obstacles, seats, toward, clear, pitch, seat);
  moved(SeatPass::Separate);
  ortho_seat_loops(in.nets, in.obstacles, seats, in.occupied, clear, pitch, seat);
  moved(SeatPass::Loop);
  PodVector<int32_t> &stuck{ sc.stuck };
  ortho_clear_occupied(in.nets,
                       in.obstacles,
                       seats,
                       toward,
                       in.occupied,
                       clear,
                       seat,
                       stuck);
  moved(SeatPass::Occupied);
  // A loop between a box and a point on its border seats the box end on the point's face,
  // `max(loop, 2 * clear)` along it toward the face's middle.
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    bool const from_box{ net.src_obstacle < in.obstacles.size() };
    if ((net.loop <= 0) || (from_box == (net.dst_obstacle < in.obstacles.size()))) {
      continue;
    }
    uint32_t const box{ from_box ? net.src_obstacle : net.dst_obstacle };
    scav_point const fixed{ from_box ? net.dst : net.src };
    scav_rect const &r{ in.obstacles[box] };
    uint32_t const face{ face_of(fixed, r) };
    if (face >= 4) { continue; }
    bool const along_y{ face < 2 };
    FaceRun const run{
      face_run(r, face, clear, (box < in.corner.size()) ? in.corner[box] : 0)
    };
    int32_t const lo{ run.lo + run.inset };
    int32_t const hi{ (run.lo + run.len) - run.inset };
    int32_t const pos{ along_y ? fixed.y : fixed.x };
    int32_t const gap{ imax(net.loop, 2 * clear) };
    int32_t const to{
      imin(imax((pos < (lo + ((hi - lo) / 2))) ? (pos + gap) : (pos - gap), lo), hi)
    };
    scav_point at{ fixed };
    (along_y ? at.y : at.x) = to;
    seat[(2 * n) + (from_box ? 0U : 1U)] = at;
    seat[(2 * n) + (from_box ? 1U : 0U)] = fixed;
  }

  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    uint32_t const off{ static_cast<uint32_t>(anchors.size()) };
    uint32_t const lead_first{ static_cast<uint32_t>(lead.size()) };
    uint32_t const src_slot{ 2 * n };
    scav_point const from{
      stubbed(net, 0) ? stub_end(net.src, net.src_stub)
                      : approach(net.src, net.src_obstacle, seat[src_slot], net.src_clear)
    };
    net_lead[n] = { .off = lead_first,
                    .len = static_cast<uint32_t>(lead.size()) - lead_first };

    anchors.push_back(from);
    for (uint32_t k = 0; k < net.waypoint_len; ++k) {
      anchors.push_back(in.waypoints[net.waypoint_off + k]);
    }
    if ((net.loop > 0) && ((net.src_obstacle < in.obstacles.size()) ||
                           (net.dst_obstacle < in.obstacles.size()) ||
                           (net.loop_box < in.obstacles.size()))) {
      // A loop's corridor runs `loop_reach` out from each box end's seat and point end,
      // off its box, else the other end's, else `loop_box`; clamped to region and band.
      std::array<scav_point, 2> at{};
      std::array<uint32_t, 2> box{};
      std::array<uint32_t, 2> face{};
      for (uint32_t end = 0; end < 2; ++end) {
        box[end] = (end == 0) ? net.src_obstacle : net.dst_obstacle;
        scav_point const point{ (end == 0) ? net.src : net.dst };
        at[end] = (box[end] < in.obstacles.size()) ? seat[src_slot + end] : point;
        if (box[end] >= in.obstacles.size()) {
          box[end] = (end == 0) ? net.dst_obstacle : net.src_obstacle;
        }
        if (box[end] >= in.obstacles.size()) { box[end] = net.loop_box; }
        face[end] = face_of(at[end], in.obstacles[box[end]]);
      }
      int32_t const reach{ ((box[0] == box[1]) && (face[0] == face[1]))
                               ? loop_reach(in, net.loop, box[0], face[0], at, bumper)
                               : net.loop };
      for (uint32_t end = 0; end < 2; ++end) {
        switch (face[end]) {
          case 0: at[end].x -= reach; break;
          case 1: at[end].x += reach; break;
          case 2: at[end].y -= reach; break;
          case 3: at[end].y += reach; break;
          default: break;
        }
        int32_t x0{ lo_x };
        int32_t y0{ lo_y };
        int32_t x1{ hi_x };
        int32_t y1{ hi_y };
        if ((enc.w > (2 * inset)) && (enc.h > (2 * inset))) {
          x0 = imax(x0, enc.x + inset);
          y0 = imax(y0, enc.y + inset);
          x1 = imin(x1, (enc.x + enc.w) - inset);
          y1 = imin(y1, (enc.y + enc.h) - inset);
        }
        at[end].x = imin(imax(at[end].x, x0), x1);
        at[end].y = imin(imax(at[end].y, y0), y1);
        anchors.push_back(at[end]);
      }
    }
    uint32_t const tail_first{ static_cast<uint32_t>(lead.size()) };
    anchors.push_back(
        stubbed(net, 1)
            ? stub_end(net.dst, net.dst_stub)
            : approach(net.dst, net.dst_obstacle, seat[src_slot + 1], net.dst_clear));
    net_tail[n] = { .off = tail_first,
                    .len = static_cast<uint32_t>(lead.size()) - tail_first };
    net_anchors[n] = { .off = off, .len = static_cast<uint32_t>(anchors.size()) - off };
  }

  OrthoGrid &g{ sc.g };
  bool const affordable{ ortho_grid(in.region, in.obstacles, anchors, bumper, g, &walls) };

  // Zero-clearance fallback grid, built on first need for nets the main grid cannot route.
  OrthoGrid &tight{ sc.tight };
  bool tight_built{ false };
  bool tight_ok{ false };
  bool open_built{ false };
  bool open_ok{ false };

  OrthoScratch &scratch{ sc.search };
  PodVector<uint32_t> &hop{ sc.hop };
  PodVector<std::array<OrthoFinish, 2>> &finishes{ sc.finishes };  // per piece
  PodVector<uint32_t> &chosen{ sc.chosen };  // per piece, the plane it arrives in
  PodVector<scav_point> &piece{ sc.piece };
  PodVector<scav_point> &shape{ sc.shape };
  // Routes net `n` on `use` into `shape`, in its end leads' planes; from `starts` when
  // set, in place of its first anchor, its seat moving to the one taken.
  auto const search = [&](uint32_t n,
                          OrthoGrid const &use,
                          PodVector<uint32_t> const *starts) {
    scav_span const at{ net_anchors[n] };
    uint32_t const src_plane{ (net_lead[n].len > 0)
                                  ? leg_plane(lead[net_lead[n].off + net_lead[n].len - 1],
                                              anchors[at.off])
                                  : INVALID };
    uint32_t const dst_plane{ (net_tail[n].len > 0)
                                  ? leg_plane(anchors[at.off + at.len - 1],
                                              lead[net_tail[n].off + net_tail[n].len - 1])
                                  : INVALID };
    // Each piece starts in each plane at the cheapest cost of arriving there, so a turn at
    // a waypoint is paid; the cheapest finish is traced back piece by piece.
    uint32_t const pieces{ at.len - 1 };
    finishes.resize(pieces);
    hop.clear();
    std::array<Wide, 2> seed{ (src_plane == 1) ? -1 : 0, (src_plane == 0) ? -1 : 0 };
    std::array<int32_t, 2> heading{ 0, 0 };  // each plane's arrival at the piece's start
    for (uint32_t k = 0; k < pieces; ++k) {
      scav_point const a{ anchors[at.off + k] };
      scav_point const b{ anchors[at.off + k + 1] };
      uint32_t const v_from{ use.vertex(ortho_index_of(use.xs, a.x),
                                        ortho_index_of(use.ys, a.y)) };
      uint32_t const v_to{ use.vertex(ortho_index_of(use.xs, b.x),
                                      ortho_index_of(use.ys, b.y)) };
      if (!ortho_search_planes(use,
                               v_from,
                               v_to,
                               bend,
                               scratch,
                               seed,
                               heading,
                               (k == 0) ? src_plane : INVALID,
                               ((k + 1) == pieces) ? dst_plane : INVALID,
                               ((k > 0) ? 1U : 0U) | (((k + 1) < pieces) ? 2U : 0U),
                               hop,
                               finishes[k],
                               (k == 0) ? starts : nullptr,
                               (k + 1) < pieces)) {
        shape.clear();
        return false;
      }
      seed = { finishes[k][0].cost, finishes[k][1].cost };
      heading = { finishes[k][0].heading, finishes[k][1].heading };
    }
    std::array<OrthoFinish, 2> const &last{ finishes[pieces - 1] };
    uint32_t plane{
      ((last[1].cost >= 0) && ((last[0].cost < 0) || (last[1].cost < last[0].cost))) ? 1U
                                                                                     : 0U
    };
    chosen.resize(pieces);
    for (uint32_t k = pieces; k-- > 0;) {
      chosen[k] = plane;
      plane = finishes[k][plane].start;
    }
    // Leads go through `ortho_simplify` too; a net whose ends coincide emits one point.
    shape.clear();
    piece.clear();
    for (uint32_t k = 0; k < net_lead[n].len; ++k) {
      piece.push_back(lead[net_lead[n].off + k]);
    }
    piece.push_back(anchors[at.off]);
    if (starts != nullptr) {
      // The seat moves along its face to the ring point taken.
      scav_point const ring{ use.point(hop[finishes[0][chosen[0]].hops.off]) };
      scav_point &seated{ piece[piece.size() - 2] };
      bool const flat{ seated.y == piece.back().y };  // a level lead slides along y
      (flat ? seated.y : seated.x) = flat ? ring.y : ring.x;
      piece.back() = ring;
    }
    ortho_simplify(piece, shape);
    for (uint32_t k = 0; k < pieces; ++k) {
      scav_span const run{ finishes[k][chosen[k]].hops };
      piece.clear();
      for (uint32_t i = 0; i < run.len; ++i) {
        piece.push_back(use.point(hop[run.off + i]));
      }
      ortho_simplify(piece, shape);
    }
    // The tail is stored outward from the box; appended reversed.
    piece.clear();
    for (uint32_t k = net_tail[n].len; k-- > 0;) {
      piece.push_back(lead[net_tail[n].off + k]);
    }
    ortho_simplify(piece, shape);
    return true;
  };
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    scav_span const at{ net_anchors[n] };
    RouteFailure why{ affordable ? RouteFailure::None : RouteFailure::TooLarge };
    for (uint32_t k = 0; (why == RouteFailure::None) && (k < at.len); ++k) {
      if (!in_region(anchors[at.off + k])) { why = RouteFailure::OutsideRegion; }
    }
    for (uint32_t k = 0; (why == RouteFailure::None) && (k < net_lead[n].len); ++k) {
      if (!in_region(lead[net_lead[n].off + k])) { why = RouteFailure::OutsideRegion; }
    }
    for (uint32_t k = 0; (why == RouteFailure::None) && (k < net_tail[n].len); ++k) {
      if (!in_region(lead[net_tail[n].off + k])) { why = RouteFailure::OutsideRegion; }
    }

    auto const attempt = [&](OrthoGrid &use) {
      sc.stub_closed.clear();
      if (stubbed(net, 0)) { close_stub(use, net.src_stub, net.src, sc.stub_closed); }
      if (stubbed(net, 1)) { close_stub(use, net.dst_stub, net.dst, sc.stub_closed); }
      bool const found{ search(n, use, nullptr) };
      reopen_route(use, sc.stub_closed);
      return found;
    };

    int32_t reseated{ 0 };
    bool ok{ false };
    // A net whose stub reaches its other end is the leg between its ends.
    if ((why == RouteFailure::None) && ((stubbed(net, 0) && same(net.src_stub, net.dst) &&
                                         (net.dst_obstacle >= in.obstacles.size())) ||
                                        (stubbed(net, 1) && same(net.dst_stub, net.src) &&
                                         (net.src_obstacle >= in.obstacles.size())))) {
      shape.clear();
      shape.push_back(net.src);
      shape.push_back(net.dst);
      ok = true;
    }
    if ((why == RouteFailure::None) && !ok) {
      // A net kept apart searches first with its `apart` net's route closed.
      sc.closed.clear();
      if (net.apart < n) {
        scav_span const had{ out.net_points[net.apart] };
        close_route(g, out.points.data() + had.off, had.len, sc.closed);
      }
      ok = attempt(g);
      if (!sc.closed.empty()) {
        reopen_route(g, sc.closed);
        if (!ok) {  // kept apart without bumpers, else across the `apart` route
          if (!tight_built) {
            tight_built = true;
            tight_ok = ortho_grid(in.region, in.obstacles, anchors, 0, tight, &walls);
          }
          if (tight_ok) {
            scav_span const had{ out.net_points[net.apart] };
            sc.closed.clear();
            close_route(tight, out.points.data() + had.off, had.len, sc.closed);
            ok = attempt(tight);
            reopen_route(tight, sc.closed);
            if (ok) {
              reseated = 1;
              trace_emit({ .kind = TraceKind::RouteReseated, .seg = { .seg = net.seg } });
            }
          }
          if (!ok) {
            ok = attempt(g);
            trace_emit({ .kind = TraceKind::RouteCrossed, .seg = { .seg = net.seg } });
          }
        }
      }
    }
    if ((why == RouteFailure::None) && !ok) {
      if (!tight_built) {
        tight_built = true;
        tight_ok = ortho_grid(in.region, in.obstacles, anchors, 0, tight, &walls);
      }
      ok = tight_ok && attempt(tight);
      if (ok) {
        reseated = 1;
        trace_emit({ .kind = TraceKind::RouteReseated, .seg = { .seg = net.seg } });
      }
      if (!ok) { why = RouteFailure::Unreachable; }
    }
    if ((why == RouteFailure::Unreachable) && (in.first_wall < in.obstacles.size())) {
      // The walls enclose an end: routes across them, blocking boxes only.
      if (!open_built) {
        open_built = true;
        sc.boxes.assign(in.obstacles.begin(), in.obstacles.begin() + in.first_wall);
        open_ok = ortho_grid(in.region, sc.boxes, anchors, bumper, sc.open, &walls);
      }
      ok = open_ok && attempt(sc.open);
      if (ok) {
        why = RouteFailure::None;
        trace_emit({ .kind = TraceKind::RouteWalled, .seg = { .seg = net.seg } });
      }
    }

    if (!ok) {
      // On failure, a straight polyline through the waypoints; a loop runs through its
      // seats and corridor anchors.
      shape.clear();
      bool const loop{ net.loop > 0 };
      uint32_t const slot{ 2 * n };
      shape.push_back(loop ? seat[slot] : net.src);
      for (uint32_t k = 0; k < net.waypoint_len; ++k) {
        shape.push_back(in.waypoints[net.waypoint_off + k]);
      }
      for (uint32_t k = 1; loop && ((k + 1) < at.len); ++k) {
        shape.push_back(anchors[at.off + k]);
      }
      shape.push_back(loop ? seat[slot + 1] : net.dst);
    }

    uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
    for (scav_point const &pt : shape) { out.points.push_back(pt); }
    scav_span const span{ .off = off,
                          .len = static_cast<uint32_t>(out.points.size()) - off };
    out.net_points.push_back(span);
    out.metrics.push_back({ .failed = why, .reseated = reseated, .occupied = stuck[n] });
  }

  // Re-routes each departure an aim off its face's run seated at a corner from every free
  // ring point on that face, other routes shut; keeps the cheaper, a crossing as a bend.
  PodVector<uint32_t> &starts{ sc.starts };
  PodVector<scav_point> &rebuilt{ sc.rebuilt };
  PodVector<int32_t> &beside{ sc.beside };     // the other seats on the sliding face
  PodVector<uint16_t> &holding{ sc.holding };  // per pass, the routes shutting it
  PodVector<uint8_t> &kept{ sc.kept };         // per net, another net keeps apart from it
  kept.assign(in.nets.size(), 0);
  for (RouteNet const &other : in.nets) {
    if (other.apart < kept.size()) { kept[other.apart] = 1; }
  }
  int32_t const by{ imax(clear, pitch) };
  // `holding` counts each route through a pass `route_passes` names, `pass_h` then
  // `pass_v`; a pass is shut while held, and `sc.closed` lists every pass it shut.
  bool holds{ false };
  auto const hold = [&](uint32_t m, bool on) {
    scav_span const span{ out.net_points[m] };
    route_passes(g,
                 out.points.data() + span.off,
                 span.len,
                 [&](bool vertical, uint32_t i) {
                   PodVector<uint8_t> &pass{ vertical ? g.pass_v : g.pass_h };
                   uint16_t &count{ holding[(vertical ? g.pass_h.size() : 0) + i] };
                   if (!on) {
                     if (count == 0) { return; }
                     --count;
                     if (count == 0) { pass[i] = 1; }
                     return;
                   }
                   if ((count == 0) && (pass[i] == 0)) { return; }  // a box's or wall's
                   if (count == 0) { sc.closed.push_back((2 * i) + (vertical ? 1U : 0U)); }
                   pass[i] = 0;
                   ++count;
                 });
  };
  was = seat;
  for (uint32_t n = 0; affordable && (n < in.nets.size()); ++n) {
    RouteNet const &net{ in.nets[n] };
    uint32_t const slot{ 2 * n };
    uint32_t const box{ net.src_obstacle };
    if ((out.metrics[n].failed != RouteFailure::None) || (out.metrics[n].reseated != 0) ||
        (net.loop > 0) || (box >= in.obstacles.size()) || seats[slot].inscribed ||
        (net_lead[n].len != 1) || (net.apart != INVALID) || (kept[n] != 0) ||
        stubbed(net, 0)) {
      continue;
    }
    scav_rect const &r{ in.obstacles[box] };
    scav_point const ring{ anchors[net_anchors[n].off] };
    uint32_t const face{ face_of(seat[slot], r) };
    if (face >= 4) { continue; }
    bool const along_y{ face < 2 };
    FaceRun const run{
      face_run(r, face, clear, (box < in.corner.size()) ? in.corner[box] : 0)
    };
    int32_t const aim{ along_y ? toward[slot].y : toward[slot].x };
    if (onto_face(aim, run) == aim) { continue; }
    // No seat on the face beats a route as short as the face allows, bent only where a
    // straight line cannot reach its far end.
    scav_span const was_at{ out.net_points[n] };
    scav_point const *const was_pts{ out.points.data() + was_at.off };
    scav_point const far{ was_pts[was_at.len - 1] };
    int32_t const far_along{ along_y ? far.y : far.x };
    int32_t const far_across{ along_y ? far.x : far.y };
    int32_t const near_along{ onto_face(far_along, run) };
    int32_t const face_line{ along_y ? seat[slot].x : seat[slot].y };
    bool const ahead{ ((face == 0) || (face == 2)) ? (far_across < face_line)
                                                   : (far_across > face_line) };
    Wide const least{ distance(far_along, near_along) + distance(far_across, face_line) +
                      (((near_along == far_along) && ahead) ? Wide{ 0 } : bend) };
    if ((polyline_length(was_pts, was_at.len) +
         (bend * Wide{ polyline_bends(was_pts, was_at.len) })) <= least) {
      continue;
    }

    // The ring points along the face that the search may leave from.
    PodVector<int32_t> const &lines{ along_y ? g.ys : g.xs };
    PodVector<int32_t> const &across{ along_y ? g.xs : g.ys };
    int32_t const ring_at{ along_y ? ring.x : ring.y };
    int32_t const face_at{ along_y ? seat[slot].x : seat[slot].y };
    uint32_t const fixed{ ortho_index_of(across, ring_at) };
    if (across[fixed] != ring_at) { continue; }
    int32_t const now{ along_y ? seat[slot].y : seat[slot].x };
    beside.clear();
    for (uint32_t other = 0; other < seat.size(); ++other) {
      if ((other != slot) && (seats[other].box == box) &&
          (face_of(seat[other], r) == face)) {
        beside.push_back(along_y ? seat[other].y : seat[other].x);
      }
    }
    int32_t const lo{ imin(face_at, ring_at) };
    int32_t const hi{ imax(face_at, ring_at) };
    starts.clear();
    for (uint32_t i = ortho_index_of(lines, run.lo + run.inset); i < lines.size(); ++i) {
      int32_t const pos{ lines[i] };
      if (pos > ((run.lo + run.len) - run.inset)) { break; }
      if (pos < (run.lo + run.inset)) { continue; }
      bool open{ (pos == now) || !occupied_at(in.occupied, box, face, pos) };
      for (uint32_t k = 0; open && (pos != now) && (k < beside.size()); ++k) {
        open = distance(pos, beside[k]) >= by;
      }
      scav_point const out_at{ along_y ? scav_point{ .x = ring_at, .y = pos }
                                       : scav_point{ .x = pos, .y = ring_at } };
      for (uint32_t o = 0; open && (o < in.obstacles.size()); ++o) {
        scav_rect const &q{ in.obstacles[o] };
        open = (o == box) ||
               (!inside(out_at, q) && !(along_y ? ortho_blocks_h(q, pos, lo, hi)
                                                : ortho_blocks_v(q, pos, lo, hi)));
      }
      for (uint32_t w = 0; open && (w < walls.size()); ++w) {
        open = !inside(out_at, walls[w]);
      }
      if (open && in_region(out_at)) {
        starts.push_back(along_y ? g.vertex(fixed, i) : g.vertex(i, fixed));
      }
    }
    if (starts.size() < 2) { continue; }

    if (!holds) {
      holds = true;
      sc.closed.clear();
      // Zero past what an earlier call left, which its end reset.
      size_t const passes{ g.pass_h.size() + g.pass_v.size() };
      if (holding.size() < passes) { holding.resize(passes, uint16_t{ 0 }); }
      for (uint32_t m = 0; m < in.nets.size(); ++m) { hold(m, true); }
    }
    hold(n, false);
    bool const found{ search(n, g, &starts) };
    scav_span const had{ out.net_points[n] };
    scav_point const *const old{ out.points.data() + had.off };
    auto const len{ static_cast<uint32_t>(shape.size()) };
    Wide const after{ polyline_length(shape.data(), len) +
                      (bend * Wide{ polyline_bends(shape.data(), len) }) };
    Wide before{ polyline_length(old, had.len) +
                 (bend * Wide{ polyline_bends(old, had.len) }) };
    for (uint32_t m = 0; found && (after >= before) && (m < in.nets.size()); ++m) {
      scav_span const other{ out.net_points[m] };
      if (m != n) {
        before += bend * Wide{
          polyline_crossings(old, had.len, out.points.data() + other.off, other.len)
        };
      }
    }
    if (!found || (after >= before)) {
      hold(n, true);
      continue;
    }

    rebuilt.clear();
    for (uint32_t m = 0; m < in.nets.size(); ++m) {
      scav_span const other{ out.net_points[m] };
      auto const off{ static_cast<uint32_t>(rebuilt.size()) };
      if (m == n) {
        rebuilt.insert(rebuilt.end(), shape.begin(), shape.end());
      } else {
        rebuilt.insert(rebuilt.end(),
                       out.points.begin() + other.off,
                       out.points.begin() + other.off + other.len);
      }
      out.net_points[m] = { .off = off,
                            .len = static_cast<uint32_t>(rebuilt.size()) - off };
    }
    out.points.swap(rebuilt);
    seat[slot] = shape.front();
    hold(n, true);
  }
  if (holds) {
    reopen_route(g, sc.closed);
    for (uint32_t const e : sc.closed) {
      holding[(((e & 1U) != 0) ? g.pass_h.size() : 0) + (e >> 1U)] = 0;
    }
  }
  moved(SeatPass::Slide);

  // The ends on each box face where two shared a point before the spread.
  PodVector<uint32_t> &ends{ sc.ends };
  ends.clear();
  for (uint32_t slot = 0; slot < seats.size(); ++slot) {
    if ((seats[slot].box < in.obstacles.size()) && !seats[slot].inscribed) {
      ends.push_back(slot);
    }
  }
  auto const face_at_seat = [&](uint32_t slot) {
    return face_of(unspread[slot], in.obstacles[seats[slot].box]);
  };
  scav_stable_sort(ends, [&](uint32_t x, uint32_t y) {
    if (seats[x].box != seats[y].box) { return seats[x].box < seats[y].box; }
    if (face_at_seat(x) != face_at_seat(y)) { return face_at_seat(x) < face_at_seat(y); }
    if (unspread[x].x != unspread[y].x) { return unspread[x].x < unspread[y].x; }
    return unspread[x].y < unspread[y].y;
  });
  PodVector<uint32_t> &groups{ sc.groups };
  groups.clear();
  for (uint32_t i = 0; i < ends.size();) {
    uint32_t j{ i + 1 };
    bool shared{ false };
    while ((j < ends.size()) && (seats[ends[j]].box == seats[ends[i]].box) &&
           (face_at_seat(ends[j]) == face_at_seat(ends[i]))) {
      shared = shared || same(unspread[ends[j]], unspread[ends[j - 1]]);
      ++j;
    }
    if (shared) {
      for (uint32_t k = i; k < j; ++k) { groups.push_back(ends[k]); }
      groups.push_back(INVALID);
    }
    i = j;
  }
  was = seat;
  ortho_order_attachments(in.nets,
                          in.obstacles,
                          bumper,
                          groups,
                          out.points,
                          out.net_points,
                          seat);
  moved(SeatPass::Order);
}

}  // namespace scav
