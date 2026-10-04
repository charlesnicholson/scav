// The `orthogonal` router: a visibility graph per frame, h-plane and v-plane
// copies joined by an edge weighing the bend penalty, and A* over that (11.5).

#include "layout/router_orthogonal.h"

#include "layout/geom.h"
#include "layout/router.h"
#include "layout/trace.h"
#include "scav/scav_layout.h"
#include "scav_int.h"
#include "scav_stable_sort.h"
#include "scav_vec.h"

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

// `f`, then `g`, then node. No two entries compare equal: `node` carries vertex
// and plane, and a node's `g` only falls.
bool frontier_before(OrthoFrontierEntry const &a, OrthoFrontierEntry const &b) {
  if (a.f != b.f) { return a.f < b.f; }
  if (a.g != b.g) { return a.g < b.g; }
  return a.node < b.node;
}

// A 4-ary heap: children of `i` are `4i+1 .. 4i+4`. Both directions move a
// hole and write the travelling entry once.
constexpr uint32_t HEAP_ARITY{ 4 };

void heap_push(std::vector<OrthoFrontierEntry> &heap, OrthoFrontierEntry const &item) {
  vec_push_back(heap, item);
  uint32_t i{ static_cast<uint32_t>(heap.size()) - 1 };
  while (i > 0) {
    uint32_t const up{ (i - 1) / HEAP_ARITY };
    if (!frontier_before(item, heap[up])) { break; }
    heap[i] = heap[up];
    i = up;
  }
  heap[i] = item;
}

OrthoFrontierEntry heap_pop(std::vector<OrthoFrontierEntry> &heap) {
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

// Into `ties` at `last`, else the bucket of the highest bit it differs from `last` in.
void radix_place(OrthoRadixHeap &h, OrthoFrontierEntry const &e) {
  uint64_t const diff{ radix_key(e.f) ^ h.last };
  if (diff == 0) {
    heap_push(h.ties, e);
    return;
  }
  uint32_t const b{ static_cast<uint32_t>(std::bit_width(diff)) - 1U };
  vec_push_back(h.bucket[b], e);
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
    // Below the least: lower `last` and re-place everything, so each bucket's rule holds.
    h.spill.clear();
    for (OrthoFrontierEntry const &t : h.ties) { vec_push_back(h.spill, t); }
    for (uint64_t full{ h.full }; full != 0; full &= full - 1) {
      std::vector<OrthoFrontierEntry> &from{
        h.bucket[static_cast<uint32_t>(std::countr_zero(full))]
      };
      for (OrthoFrontierEntry const &t : from) { vec_push_back(h.spill, t); }
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
    // The lowest bucket holds the least `f`; it becomes `last` and the bucket re-places
    // into lower ones and `ties`.
    auto const b{ static_cast<uint32_t>(std::countr_zero(h.full)) };
    std::vector<OrthoFrontierEntry> &from{ h.bucket[b] };
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

// How far outside `[lo, lo + len]` a coordinate lies, zero inside it.
Wide beyond(int32_t v, int32_t lo, int32_t len) {
  Wide const hi{ Wide{ lo } + len };
  if (v < lo) { return Wide{ lo } - v; }
  if (Wide{ v } > hi) { return Wide{ v } - hi; }
  return Wide{ 0 };
}

// A coordinate onto the face `[lo, lo + len]`, held off each corner by the
// larger of the clearance and the arc drawn there, or at the middle where the
// face has no room for that: an end on a corner leaves along the face it did
// not pick, and an end on an arc points at canvas nothing was drawn on.
int32_t onto_face(int32_t v, int32_t lo, int32_t len, int32_t clear, int32_t corner) {
  int32_t const inset{ imin(imax(clear, corner), len / 2) };
  return imin(imax(v, lo + inset), (lo + len) - inset);
}

// One attachment of one net, keyed so the members of a face are adjacent and in
// an order the net order cannot disturb. `end` 0 leaves the box, 1 arrives at it,
// and `pos` is how far along the face it sits, which only the spread reads.
struct Seat {
  uint32_t box, face, end, slot;
  int32_t pos;
};

// Which face of `r` a seated point lies on: 0 left, 1 right, 2 top, 3 bottom, or
// INVALID for a point on none. Read in `ortho_ring`'s order, so the two agree
// about a corner.
// Whether two nets meet at a box -- a fan out of one state or into one.
bool shares_box(std::vector<RouteNet> const &nets, uint32_t a, uint32_t b) {
  std::array<uint32_t, 2> const one{ nets[a].src_obstacle, nets[a].dst_obstacle };
  std::array<uint32_t, 2> const two{ nets[b].src_obstacle, nets[b].dst_obstacle };
  for (uint32_t const x : one) {
    for (uint32_t const y : two) {
      if ((x != INVALID) && (x == y)) { return true; }
    }
  }
  return false;
}

uint32_t face_of(scav_point at, scav_rect const &r) {
  if (at.x == r.x) { return 0; }
  if (at.x == (r.x + r.w)) { return 1; }
  if (at.y == r.y) { return 2; }
  if (at.y == (r.y + r.h)) { return 3; }
  return INVALID;
}

// The one point an axis-aligned route meets an inscribed glyph at on `face`,
// spelled the way `ortho_attach_box` spells it.
scav_point face_middle(scav_rect const &r, uint32_t face) {
  switch (face) {
    case 0: return { .x = r.x, .y = r.y + (r.h / 2) };
    case 1: return { .x = r.x + r.w, .y = r.y + (r.h / 2) };
    case 2: return { .x = r.x + (r.w / 2), .y = r.y };
    default: return { .x = r.x + (r.w / 2), .y = r.y + r.h };
  }
}

// The nearer of the two faces `axis_is_y` names, by `ortho_escape_box`'s own
// rule and its tie: 2 or 3 for the y axis, 0 or 1 for the x.
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
  // `w > 0` is not implied by the straddle: a zero-width box has two sides a
  // segment passes between, and blocking there forbids what the scorer allows.
  return (r.w > 0) && (y > r.y) && (y < (r.y + r.h)) && (x0 < (r.x + r.w)) && (x1 > r.x);
}

bool ortho_blocks_v(scav_rect const &r, int32_t x, int32_t y0, int32_t y1) {
  return (r.h > 0) && (x > r.x) && (x < (r.x + r.w)) && (y0 < (r.y + r.h)) && (y1 > r.y);
}

namespace {

// First index with `v[i] > key`, and first with `v[i] >= key`, over a sorted
// unique array. A box blocks only the cells it covers, so these bound the
// blocking loops instead of every cell being tested against every box -- which
// was O(boxes * nx * ny), and nx and ny are each O(boxes) (11.10c).
uint32_t ortho_after(std::vector<int32_t> const &v, int32_t key) {
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

uint32_t ortho_from(std::vector<int32_t> const &v, int32_t key) {
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

void ortho_sort_unique(std::vector<int32_t> &v) {
  // LSD radix on the bits offset to unsigned, through a per-thread buffer, with
  // a byte every value shares skipped. Nothing here waits on the pool.
  size_t const n{ v.size() };
  if (n <= SCAV_SORT_SMALL) {
    scav_insertion_sort(v.data(), v.data() + n, [](int32_t a, int32_t b) {
      return a < b;
    });
  } else {
    constexpr uint32_t DIGITS{ 4 };
    constexpr uint32_t RADIX{ 256 };
    thread_local std::vector<int32_t> spare;
    vec_resize(spare, n);
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
  vec_resize(v, kept);
}

uint32_t ortho_index_of(std::vector<int32_t> const &v, int32_t at) {
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
  // The dominant separation picks the axis, a tie going to x.
  return beyond(toward.x, r.x, r.w) >= beyond(toward.y, r.y, r.h);
}

scav_point ortho_escape_box(scav_point at, scav_point toward, scav_rect const &r) {
  // The side is the nearer border, which is total for a `toward` inside the span.
  if (ortho_escape_horizontal(toward, r)) {
    bool const left{ (Wide{ toward.x } - r.x) <= ((Wide{ r.x } + r.w) - toward.x) };
    return { .x = left ? r.x : (r.x + r.w), .y = at.y };
  }
  bool const top{ (Wide{ toward.y } - r.y) <= ((Wide{ r.y } + r.h) - toward.y) };
  return { .x = at.x, .y = top ? r.y : (r.y + r.h) };
}

scav_point ortho_attach_face(scav_point toward,
                             scav_rect const &r,
                             int32_t clear,
                             bool inscribed,
                             int32_t corner,
                             uint32_t face) {
  // **A face with no room to put a seat on is not a face.** `onto_face` holds a
  // seat off both corners, so a face shorter than twice that inset clamps every
  // seat on it to one point -- and a fork's bar, wide and thin, has two of
  // them. Pinning one stacks every branch on a single seat, which is a thing
  // nothing in `Cost` prices, so the pin is declined rather than honoured
  // (11.10e). The rule below then answers, which is what an unpinned end does.
  int32_t const len{ (face < 2) ? r.h : r.w };
  if (!inscribed && (len <= (2 * imin(imax(clear, corner), len / 2)))) {
    return ortho_attach_box(toward, r, clear, inscribed, corner);
  }
  if (face < 2) {  // left or right: the position along it is a y
    int32_t const y{ inscribed ? (r.y + (r.h / 2))
                               : onto_face(toward.y, r.y, r.h, clear, corner) };
    return { .x = (face == 0) ? r.x : (r.x + r.w), .y = y };
  }
  int32_t const x{ inscribed ? (r.x + (r.w / 2))
                             : onto_face(toward.x, r.x, r.w, clear, corner) };
  return { .x = x, .y = (face == 2) ? r.y : (r.y + r.h) };
}

scav_point ortho_attach_box(scav_point toward,
                            scav_rect const &r,
                            int32_t clear,
                            bool inscribed,
                            int32_t corner) {
  // Along the face, `toward`'s own projection onto it, so the end leaves aimed
  // at where it is going. A box centre carries no such information and every
  // net naming one face of one box would otherwise be handed the same point.
  // An inscribed glyph has one point per face and it is the midpoint, spelled
  // the way the reference builder spells the centre it draws the mark on: an
  // inset of half the face leaves two units to choose from on an odd one, and
  // the upper of them misses the tangent.
  scav_point aimed{ toward };
  if (ortho_escape_horizontal(toward, r)) {
    aimed.y = inscribed ? (r.y + (r.h / 2)) : onto_face(toward.y, r.y, r.h, clear, corner);
  } else {
    aimed.x = inscribed ? (r.x + (r.w / 2)) : onto_face(toward.x, r.x, r.w, clear, corner);
  }
  return ortho_escape_box(aimed, toward, r);
}

void ortho_reface_attachments(std::vector<RouteNet> const &nets,
                              std::vector<scav_rect> const &boxes,
                              std::vector<uint8_t> const &inscribed,
                              std::vector<scav_point> const &toward,
                              std::vector<scav_point> &at) {
  thread_local std::vector<Seat> seats;
  seats.clear();
  for (uint32_t n = 0; n < nets.size(); ++n) {
    for (uint32_t end = 0; end < 2; ++end) {
      uint32_t const box{ (end == 0) ? nets[n].src_obstacle : nets[n].dst_obstacle };
      if ((box >= boxes.size()) || (box >= inscribed.size()) || (inscribed[box] == 0)) {
        continue;
      }
      uint32_t const slot{ (2 * n) + end };
      uint32_t const face{ face_of(at[slot], boxes[box]) };
      if (face == INVALID) { continue; }
      vec_push_back(seats,
                    { .box = box, .face = face, .end = end, .slot = slot, .pos = 0 });
    }
  }
  scav_stable_sort(seats, [](Seat const &a, Seat const &b) {
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

    // Which directions each of the four faces holds, so "mixed" is a question
    // about the box rather than about whichever seat asked.
    std::array<uint8_t, 4> holds{ 0, 0, 0, 0 };
    for (uint32_t i = first; i < stop; ++i) {
      uint32_t const face{ seats[i].face };
      holds[face] = static_cast<uint8_t>(holds[face] | (1U << seats[i].end));
    }
    for (uint32_t face = 0; face < 4; ++face) {
      if (holds[face] != 3) { continue; }
      bool const cross_is_y{ face < 2 };  // the axis the other two faces face
      uint32_t const cross_low{ cross_is_y ? 2U : 0U };  // top for y, left for x
      // Which direction moves: the one with the most to gain on the axis the
      // escape rule did not pick, since a second face is what that separation
      // is for. A tie goes to the departures, whose head is not the one at risk
      // of being inked along the other line.
      std::array<Wide, 2> gain{ 0, 0 };
      for (uint32_t i = first; i < stop; ++i) {
        if (seats[i].face != face) { continue; }
        scav_point const aim{ toward[seats[i].slot] };
        Wide const off{ cross_is_y ? beyond(aim.y, r.y, r.h) : beyond(aim.x, r.x, r.w) };
        gain[seats[i].end] = imax(gain[seats[i].end], off);
      }
      uint32_t const mover{ (gain[1] > gain[0]) ? 1U : 0U };
      // And which of the other axis's two faces, by where the movers lie on it.
      int32_t votes{ 0 };
      for (uint32_t i = first; i < stop; ++i) {
        if ((seats[i].face != face) || (seats[i].end != mover)) { continue; }
        bool const near{ nearer_face(toward[seats[i].slot], r, cross_is_y) == cross_low };
        votes += near ? 1 : -1;
      }
      // The near face, then the far one, then the one opposite the mixed face:
      // a fixed order, so a box whose other faces are taken still answers the
      // same way twice.
      uint32_t const nearest{ (votes >= 0) ? cross_low : (cross_low + 1) };
      std::array<uint32_t, 3> const tries{ nearest, nearest ^ 1U, face ^ 1U };
      uint32_t chosen{ INVALID };
      for (uint32_t const candidate : tries) {
        if ((holds[candidate] & (1U << (1U - mover))) == 0) {
          chosen = candidate;
          break;
        }
      }
      if (chosen == INVALID) { continue; }  // every other face is taken: stacked
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

void ortho_align_attachments(std::vector<RouteNet> const &nets,
                             std::vector<scav_rect> const &boxes,
                             std::vector<uint8_t> const &inscribed,
                             std::vector<int32_t> const &corner,
                             std::vector<scav_point> &at) {
  // Empty for zero throughout, the way `inscribed` is.
  auto const arc = [&corner](uint32_t box) {
    return (box < corner.size()) ? corner[box] : 0;
  };
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

    // What each face can seat: its whole run less the arc drawn at each corner,
    // or the one midpoint an inscribed glyph offers.
    //
    // **The arc and one unit, where `onto_face` holds a seat off by
    // `max(clear, arc)`.** Two reasons are folded into that number and only one
    // of them is the clearance's: a seat under an arc points at canvas nothing
    // was drawn on, and a seat at a face's very end leaves along the face it did
    // not pick. The first wants the arc; the second wants to be strictly inside
    // and nothing more, because two aligned seats have collinear approach legs
    // out of parallel faces and no adjacent face to be confused with. Spending
    // the router's obstacle clearance on it costs alignment: the corpus has
    // **thirteen** pairs whose boxes overlap by 163 units and whose seatable
    // runs then miss by 29, each paying a 29-unit jog on a run of a thousand.
    int32_t lo{ 0 };
    int32_t hi{ 0 };
    Wide centres{ 0 };
    for (uint32_t k = 0; k < 2; ++k) {
      uint32_t const box{ (k == 0) ? src_box : dst_box };
      scav_rect const &r{ boxes[box] };
      int32_t const start{ along_y ? r.y : r.x };
      int32_t const len{ along_y ? r.h : r.w };
      bool const one_point{ (box < inscribed.size()) && (inscribed[box] != 0) };
      int32_t const inset{ one_point ? (len / 2) : imin(imax(arc(box), 1), len / 2) };
      int32_t const face_lo{ start + inset };
      int32_t const face_hi{ one_point ? face_lo : ((start + len) - inset) };
      lo = (k == 0) ? face_lo : imax(lo, face_lo);
      hi = (k == 0) ? face_hi : imin(hi, face_hi);
      centres += Wide{ start } + (len / 2);
    }
    if (lo > hi) { continue; }  // no coordinate both faces can seat

    // Halfway between the two centres, which is symmetric in the pair; a net that
    // leans takes the lower end of the run both faces seat.
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

void ortho_spread_attachments(std::vector<RouteNet> const &nets,
                              std::vector<scav_rect> const &boxes,
                              std::vector<uint8_t> const &inscribed,
                              std::vector<int32_t> const &corner,
                              std::vector<scav_point> const &toward,
                              int32_t clear,
                              int32_t pitch,
                              std::vector<scav_point> &at) {
  // Empty for zero throughout, the way `inscribed` is.
  auto const arc = [&corner](uint32_t box) {
    return (box < corner.size()) ? corner[box] : 0;
  };
  if (clear <= 0) { return; }
  int32_t const apart{ imax(clear, pitch) };
  thread_local std::vector<Seat> seats;
  seats.clear();
  for (uint32_t n = 0; n < nets.size(); ++n) {
    for (uint32_t end = 0; end < 2; ++end) {
      uint32_t const box{ (end == 0) ? nets[n].src_obstacle : nets[n].dst_obstacle };
      bool const one_point{ (box < inscribed.size()) && (inscribed[box] != 0) };
      if ((box >= boxes.size()) || one_point) { continue; }
      uint32_t const slot{ (2 * n) + end };
      uint32_t const face{ face_of(at[slot], boxes[box]) };
      if (face == INVALID) { continue; }
      vec_push_back(seats,
                    { .box = box, .face = face, .end = end, .slot = slot, .pos = 0 });
    }
  }

  // The box at a seat's far end, which is past `boxes` for an end naming none.
  auto const far_box = [&](Seat const &seat) {
    RouteNet const &net{ nets[seat.slot / 2] };
    return (seat.end == 0) ? net.dst_obstacle : net.src_obstacle;
  };
  // Whether a seat's far end names no box and its aim lies level with it: a port's
  // straight leg, to the port or to the waypoint before it.
  auto const level = [&](Seat const &seat) {
    scav_point const aim{ toward[seat.slot] };
    return (far_box(seat) >= boxes.size()) &&
           (((seat.face < 2) ? aim.y : aim.x) == seat.pos);
  };
  auto const glyph_far = [&](Seat const &seat) {
    uint32_t const box{ far_box(seat) };
    return (box < inscribed.size()) && (inscribed[box] != 0);
  };

  // One sweep of the face: seats on one point part by direction. At a `level` seat,
  // its direction stays put, bar seats aimed at a glyph, and the rest move a whole step,
  // the other way where a corner holds them.
  auto const sweep = [&]() {
    for (Seat &seat : seats) {
      seat.pos = (seat.face < 2) ? at[seat.slot].y : at[seat.slot].x;
    }
    scav_stable_sort(seats, [](Seat const &a, Seat const &b) {
      if (a.box != b.box) { return a.box < b.box; }
      if (a.face != b.face) { return a.face < b.face; }
      if (a.pos != b.pos) { return a.pos < b.pos; }
      if (a.end != b.end) { return a.end < b.end; }
      return a.slot < b.slot;
    });
    bool moved{ false };
    for (uint32_t start = 0; start < seats.size();) {
      uint32_t end{ start + 1 };
      while ((end < seats.size()) && (seats[end].box == seats[start].box) &&
             (seats[end].face == seats[start].face) &&
             (seats[end].pos == seats[start].pos)) {
        ++end;
      }
      uint32_t const first{ start };
      uint32_t const count{ end - first };
      start = end;
      if (count < 2) { continue; }
      uint32_t keep{ INVALID };  // the direction of the first level seat
      for (uint32_t i = first; (i < end) && (keep == INVALID); ++i) {
        if (level(seats[i])) { keep = seats[i].end; }
      }
      uint32_t const split{ seats[end - 1].end - seats[first].end };
      if ((split == 0) && (keep == INVALID)) { continue; }
      scav_rect const &r{ boxes[seats[first].box] };
      bool const along_y{ seats[first].face < 2 };
      int32_t const lo{ along_y ? r.y : r.x };
      int32_t const len{ along_y ? r.h : r.w };
      // `apart` where the run `onto_face` seats on holds it; otherwise sized to
      // the face, so a mark too small to seat both leaves them stacked.
      int32_t const inset{ imin(imax(clear, arc(seats[first].box)), len / 2) };
      int32_t const step{ ((len - (2 * inset)) >= apart) ? apart : imin(clear, len / 3) };
      if (step <= 0) { continue; }
      for (uint32_t i = first; i < end; ++i) {
        Seat const &seat{ seats[i] };
        if ((keep != INVALID) &&
            (level(seat) || ((seat.end == keep) && !glyph_far(seat)))) {
          continue;
        }
        // The net's direction through the face: the one running + takes the lower seat at
        // both ends, so a pair apart at one box is apart the same way at the other.
        bool const forward{ (seat.end == 0) == ((seat.face == 1) || (seat.face == 3)) };
        int32_t const down_by{ (keep == INVALID) ? (step / 2) : step };
        int32_t const up_by{ (keep == INVALID) ? (step - (step / 2)) : step };
        int32_t const by{ forward ? -down_by : up_by };
        int32_t got{ onto_face(seat.pos + by, lo, len, clear, arc(seat.box)) };
        if ((keep != INVALID) && (got == seat.pos)) {  // a corner holds it: the other way
          got = onto_face(seat.pos - by, lo, len, clear, arc(seat.box));
        }
        int32_t &held{ along_y ? at[seat.slot].y : at[seat.slot].x };
        if (held == got) { continue; }
        held = got;
        moved = true;

        // The net moves, not the seat: the far end takes the same displacement where its
        // face can seat it exactly, and stays where the face would clamp.
        uint32_t const net{ seat.slot / 2 };
        uint32_t const twin{ (2 * net) + (1 - seat.end) };
        uint32_t const box{ (seat.end == 0) ? nets[net].dst_obstacle
                                            : nets[net].src_obstacle };
        bool const one_point{ (box < inscribed.size()) && (inscribed[box] != 0) };
        if ((box >= boxes.size()) || one_point) { continue; }
        uint32_t const face{ face_of(at[twin], boxes[box]) };
        if ((face == INVALID) || ((face < 2) != along_y)) { continue; }
        scav_rect const &far{ boxes[box] };
        int32_t &there{ along_y ? at[twin].y : at[twin].x };
        int32_t const aim{ there + (got - seat.pos) };
        if (onto_face(aim,
                      along_y ? far.y : far.x,
                      along_y ? far.h : far.w,
                      clear,
                      arc(box)) == aim) {
          there = aim;
        }
      }
    }
    return moved;
  };

  // A sweep can seat a group's arrivals on another group's departure, since
  // several projections clamp to the same corner inset, so it repeats until
  // nothing moves. A face has finitely many seats and each sweep separates at
  // least one group, so the seat count bounds it; a group with no room stops
  // moving and ends the run.
  for (uint32_t round = 0; round < seats.size(); ++round) {
    if (!sweep()) { break; }
  }
}

void ortho_separate_attachments(std::vector<RouteNet> const &nets,
                                std::vector<scav_rect> const &boxes,
                                std::vector<uint8_t> const &inscribed,
                                std::vector<int32_t> const &corner,
                                std::vector<scav_point> const &toward,
                                int32_t clear,
                                int32_t pitch,
                                std::vector<scav_point> &at) {
  auto const arc = [&corner](uint32_t box) {
    return (box < corner.size()) ? corner[box] : 0;
  };
  if ((pitch <= 0) || (clear <= 0)) { return; }

  // A seat on a left or right face leaves along x, so its leg sits at the
  // seat's y and runs to the other end's x; on a top or bottom face the axes
  // swap. `pos` is the coordinate the leg runs *at*, which is what a reader
  // sees two of, and `lo`/`hi` are what it runs *across*.
  struct Leg {
    uint32_t box, face, slot, net;
    int32_t pos, lo, hi;
  };
  thread_local std::vector<Leg> legs;
  legs.clear();
  // Whether a leg's far end names no box and its aim lies level with its seat: a port's
  // straight leg, as in the spread above.
  auto const level = [&](Leg const &leg) {
    RouteNet const &net{ nets[leg.net] };
    uint32_t const far{ ((leg.slot % 2) == 0) ? net.dst_obstacle : net.src_obstacle };
    scav_point const aim{ toward[leg.slot] };
    return (far >= boxes.size()) && (((leg.face < 2) ? aim.y : aim.x) == leg.pos);
  };
  for (uint32_t n = 0; n < nets.size(); ++n) {
    for (uint32_t end = 0; end < 2; ++end) {
      uint32_t const box{ (end == 0) ? nets[n].src_obstacle : nets[n].dst_obstacle };
      bool const one_point{ (box < inscribed.size()) && (inscribed[box] != 0) };
      if ((box >= boxes.size()) || one_point) { continue; }
      uint32_t const slot{ (2 * n) + end };
      uint32_t const face{ face_of(at[slot], boxes[box]) };
      if (face == INVALID) { continue; }
      scav_point const here{ at[slot] };
      scav_point const other{ at[(2 * n) + (1 - end)] };
      bool const along_y{ face < 2 };
      int32_t const run{ along_y ? other.x : other.y };
      int32_t const from{ along_y ? here.x : here.y };
      vec_push_back(legs,
                    { .box = box,
                      .face = face,
                      .slot = slot,
                      .net = n,
                      .pos = along_y ? here.y : here.x,
                      .lo = imin(from, run),
                      .hi = imax(from, run) });
    }
  }

  // One axis at a time: only legs of one orientation can crowd each other.
  auto const sweep = [&](bool along_y) {
    thread_local std::vector<uint32_t> here;
    here.clear();
    for (uint32_t i = 0; i < legs.size(); ++i) {
      if ((legs[i].face < 2) == along_y) { vec_push_back(here, i); }
    }
    scav_stable_sort(here, [](uint32_t a, uint32_t b) {
      if (legs[a].pos != legs[b].pos) { return legs[a].pos < legs[b].pos; }
      return legs[a].slot < legs[b].slot;
    });
    bool moved{ false };
    for (uint32_t k = 0; (k + 1) < here.size(); ++k) {
      Leg &low{ legs[here[k]] };
      Leg &high{ legs[here[k + 1]] };
      // One net's own two ends are what alignment just put on one coordinate,
      // and one face's crowd is the pass above's -- a trunk lives there.
      if (low.net == high.net) { continue; }
      if ((low.box == high.box) && (low.face == high.face)) { continue; }
      // Two routes that meet at a box are a fan, and 11.5's bundles keep one
      // whole. Pushing their far ends apart does not separate them, it bends
      // the fan and crowds the end they share: `gauntlet/fanin` reads five
      // crowded pairs that way against one.
      if (shares_box(nets, low.net, high.net)) { continue; }
      int32_t const apart{ high.pos - low.pos };
      if ((apart <= 0) || (apart >= pitch)) { continue; }
      if ((low.hi <= high.lo) || (high.hi <= low.lo)) { continue; }  // never alongside
      // Half the shortfall each, or all of it to the partner of a level leg.
      int32_t const want{ pitch - apart };
      std::array<bool, 2> const fixed{ level(low), level(high) };
      if (fixed[0] && fixed[1]) { continue; }
      std::array<int32_t, 2> got{ low.pos, high.pos };
      for (uint32_t which = 0; which < 2; ++which) {
        Leg const &leg{ (which == 0) ? low : high };
        if (fixed[which]) { continue; }
        scav_rect const &r{ boxes[leg.box] };
        int32_t const face_lo{ (leg.face < 2) ? r.y : r.x };
        int32_t const len{ (leg.face < 2) ? r.h : r.w };
        int32_t share{ (which == 0) ? (want / 2) : (want - (want / 2)) };
        if (fixed[1 - which]) { share = want; }
        int32_t const aim{ leg.pos + ((which == 0) ? -share : share) };
        got[which] = onto_face(aim, face_lo, len, clear, arc(leg.box));
      }
      for (uint32_t which = 0; which < 2; ++which) {
        Leg &leg{ (which == 0) ? low : high };
        if (got[which] == leg.pos) { continue; }
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

  // A pair pushed apart can land on a third, and a face with no room stops
  // moving, so the leg count bounds the run the way the spread above is bounded.
  for (uint32_t round = 0; round < legs.size(); ++round) {
    bool const y{ sweep(true) };
    bool const x{ sweep(false) };
    if (!y && !x) { break; }
  }
}

scav_point ortho_escape(scav_point at,
                        scav_point toward,
                        std::vector<scav_rect> const &boxes) {
  // Innermost by area, then by list order: the shortest stub out crosses the
  // fewest boxes it is not excused from crossing.
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

uint32_t ortho_box_at(scav_point at, std::vector<scav_rect> const &boxes) {
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

void ortho_simplify(std::vector<scav_point> const &from, std::vector<scav_point> &to) {
  for (scav_point const &at : from) {
    // Both rules to a fixed point: one pop can expose a duplicate underneath,
    // which is what a route doubling back on its own stub produces.
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
    if (!skip) { vec_push_back(to, at); }
  }
}

std::vector<scav_rect> ortho_enclosure_walls(scav_rect const &region,
                                             scav_rect const &enclosure,
                                             int32_t inset) {
  std::vector<scav_rect> out;
  ortho_enclosure_walls(region, enclosure, inset, out);
  return out;
}

void ortho_enclosure_walls(scav_rect const &region,
                           scav_rect const &enclosure,
                           int32_t inset,
                           std::vector<scav_rect> &out) {
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
    vec_push_back(out,
                  { .x = static_cast<int32_t>(x0),
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
                std::vector<scav_rect> const &obstacles,
                std::vector<scav_point> const &anchors,
                int32_t clear,
                OrthoGrid &out,
                std::vector<scav_rect> const *fixed) {
  int32_t const lo_x{ region.x };
  int32_t const hi_x{ region.x + region.w };
  int32_t const lo_y{ region.y };
  int32_t const hi_y{ region.y + region.h };

  out.xs.clear();
  out.ys.clear();
  out.pass_h.clear();
  out.pass_v.clear();
  vec_push_back(out.xs, lo_x);
  vec_push_back(out.xs, hi_x);
  vec_push_back(out.ys, lo_y);
  vec_push_back(out.ys, hi_y);
  // Grown sides only: a line on a box edge is a lane, and hugging is free. Ring
  // points sit here, so denying the true border costs no reachability.
  for (scav_rect const &r : obstacles) {
    for (int32_t const at : { r.x - clear, r.x + r.w + clear }) {
      if ((at > lo_x) && (at < hi_x)) { vec_push_back(out.xs, at); }
    }
    for (int32_t const at : { r.y - clear, r.y + r.h + clear }) {
      if ((at > lo_y) && (at < hi_y)) { vec_push_back(out.ys, at); }
    }
  }
  if (fixed != nullptr) {
    for (scav_rect const &r : *fixed) {
      for (int32_t const at : { r.x, r.x + r.w }) {
        if ((at > lo_x) && (at < hi_x)) { vec_push_back(out.xs, at); }
      }
      for (int32_t const at : { r.y, r.y + r.h }) {
        if ((at > lo_y) && (at < hi_y)) { vec_push_back(out.ys, at); }
      }
    }
  }
  for (scav_point const &at : anchors) {
    // Both coordinates or neither: an anchor outside the region is rejected
    // anyway and should not seed half a crossing inside it.
    if ((at.x < lo_x) || (at.x > hi_x) || (at.y < lo_y) || (at.y > hi_y)) { continue; }
    vec_push_back(out.xs, at.x);
    vec_push_back(out.ys, at.y);
  }
  ortho_sort_unique(out.xs);
  ortho_sort_unique(out.ys);
  if ((Wide{ out.nx() } * out.ny()) > ORTHO_VERTEX_BUDGET) { return false; }

  vec_assign(out.pass_h, static_cast<size_t>(out.ny()) * (out.nx() - 1), 1);
  vec_assign(out.pass_v, static_cast<size_t>(out.ny() - 1) * out.nx(), 1);
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
  // Blocking against the bumper makes "no closer than `clear` to a box" a
  // property of the graph. The bounds in `block` are `ortho_blocks_h` and
  // `ortho_blocks_v` solved for their index ranges rather than evaluated per
  // cell; the predicates stay as the statement of what is blocked.
  for (scav_rect const &box : obstacles) { block(grow(box, clear)); }
  if (fixed != nullptr) {
    for (scav_rect const &r : *fixed) { block(r); }
  }
  return true;
}

bool ortho_search(OrthoGrid const &g,
                  uint32_t from,
                  uint32_t to,
                  Wide bend,
                  OrthoScratch &s,
                  std::vector<uint32_t> &out,
                  uint32_t from_plane,
                  uint32_t to_plane) {
  out.clear();
  uint32_t const vertices{ g.nx() * g.ny() };
  if ((vertices == 0) || (from >= vertices) || (to >= vertices)) { return false; }
  if (g.pass_h.empty() && (g.nx() > 1)) { return false; }
  uint32_t const nodes{ vertices * 2 };
  // Only grown; every stamp in it predates this search's generation.
  if (s.state.size() < nodes) {
    vec_resize(s.state, nodes, OrthoNodeState{ .stamp = 0, .parent = INVALID, .best = 0 });
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

  // Weights in quarters: an end's turn is a bend less one, so of two paths equal in
  // length and bends the one turning at its ends wins, and no other pair swaps.
  constexpr Wide QUARTERS{ 4 };
  Wide const turn_w{ QUARTERS * bend };
  Wide const end_turn_w{ (bend > 0) ? (turn_w - 1) : turn_w };

  // Manhattan to the goal, a turn for an axis this plane cannot cover alone, and an
  // end turn to finish in a fixed goal plane: lower bounds, so the sum admits.
  bool const fixed_goal{ to_plane < 2 };
  auto const heuristic = [&](Wide dx, Wide dy, uint32_t plane) {
    bool const turn{ (plane == 0) ? (dy != 0) : (dx != 0) };
    uint32_t const last{ turn ? (1U - plane) : plane };
    bool const settle{ fixed_goal && (last != to_plane) };
    return (QUARTERS * (dx + dy)) + (turn ? turn_w : Wide{ 0 }) +
           (settle ? end_turn_w : Wide{ 0 });
  };

  ortho_open_clear(open);
  {
    Wide const dx{ distance(xs[from % nx], goal.x) };
    Wide const dy{ distance(ys[from / nx], goal.y) };
    // A fixed start seeds its lead's plane alone; the turn reaches the other.
    for (uint32_t plane = 0; plane < 2; ++plane) {
      if ((from_plane < 2) && (plane != from_plane)) { continue; }
      uint32_t const node{ (from * 2) + plane };
      state[node] = { .stamp = gen, .parent = INVALID, .best = 0 };
      ortho_open_push(open, { .f = heuristic(dx, dy, plane), .g = 0, .node = node });
    }
  }

  uint32_t expansions{ 0 };
  uint32_t reached{ INVALID };
  while (!ortho_open_empty(open)) {
    OrthoFrontierEntry const top{ ortho_open_pop(open) };
    uint32_t const node{ top.node };
    OrthoNodeState const &here{ state[node] };
    if ((here.stamp != gen) || (top.g != here.best)) { continue; }
    uint32_t const v{ node / 2 };
    uint32_t const plane{ node % 2 };
    if ((v == to) && (!fixed_goal || (plane == to_plane))) {
      reached = node;
      break;
    }
    if (++expansions > ORTHO_EXPANSION_BUDGET) { return false; }

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

    // The turn, an end's where it leaves a fixed start or settles into the goal's
    // plane, then this plane's moves.
    bool const end_turn{ ((v == from) && (plane == from_plane)) ||
                         ((v == to) && fixed_goal) };
    relax(node ^ 1U, end_turn ? end_turn_w : turn_w, heuristic(dx, dy, 1 - plane));
    if (plane == 0) {
      uint8_t const *const row{ pass_h + (static_cast<size_t>(iy) * (nx - 1)) };
      if (((ix + 1) < nx) && (row[ix] != 0)) {
        relax(node + 2,
              QUARTERS * (Wide{ xs[ix + 1] } - xs[ix]),
              heuristic(distance(xs[ix + 1], goal.x), dy, 0));
      }
      if ((ix > 0) && (row[ix - 1] != 0)) {
        relax(node - 2,
              QUARTERS * (Wide{ xs[ix] } - xs[ix - 1]),
              heuristic(distance(xs[ix - 1], goal.x), dy, 0));
      }
    } else {
      uint32_t const down{ 2 * nx };
      if (((iy + 1) < ny) && (pass_v[(static_cast<size_t>(iy) * nx) + ix] != 0)) {
        relax(node + down,
              QUARTERS * (Wide{ ys[iy + 1] } - ys[iy]),
              heuristic(dx, distance(ys[iy + 1], goal.y), 1));
      }
      if ((iy > 0) && (pass_v[(static_cast<size_t>(iy - 1) * nx) + ix] != 0)) {
        relax(node - down,
              QUARTERS * (Wide{ ys[iy] } - ys[iy - 1]),
              heuristic(dx, distance(ys[iy - 1], goal.y), 1));
      }
    }
  }
  if (reached == INVALID) { return false; }

  s.path.clear();
  for (uint32_t node = reached; node != INVALID; node = state[node].parent) {
    vec_push_back(s.path, node / 2);
  }
  for (auto i = static_cast<uint32_t>(s.path.size()); i-- > 0;) {
    if (out.empty() || (out.back() != s.path[i])) { vec_push_back(out, s.path[i]); }
  }
  return true;
}

Wide ortho_bend_penalty(scav_profile const &p) {
  // Not 11.6's exchange rate, which is sixteen grid units and buys a staircase
  // wherever the grid offers one. A profile field for it is P9's calibration.
  return imax(Wide{ p.rank_sep }, Wide{ 1 });
}

int32_t ortho_clearance(scav_profile const &p) { return route_clearance(p); }

int32_t OrthogonalRouter::margin(scav_profile const &p) const {
  return ortho_clearance(p);
}

namespace {

// Every buffer one `route` call uses, kept per thread and reassigned in place.
// `route` never waits on the pool, so a thread runs one call at a time.
struct RouteScratch {
  std::vector<scav_rect> walls;
  std::vector<scav_point> lead, anchors, seat, toward, was;
  std::vector<scav_span> net_anchors, net_lead, net_tail;
  OrthoGrid g, tight;
  OrthoScratch search;
  std::vector<uint32_t> hop;
  std::vector<scav_point> piece, shape;
};

RouteScratch &route_scratch() {
  thread_local RouteScratch s;
  return s;
}

// The search plane a leg from `a` to `b` runs in: 0 horizontal, 1 vertical,
// INVALID for a point or a diagonal.
uint32_t leg_plane(scav_point a, scav_point b) {
  if ((a.y == b.y) && (a.x != b.x)) { return 0; }
  if ((a.x == b.x) && (a.y != b.y)) { return 1; }
  return INVALID;
}

// What an end is aimed at: the first corridor point it runs through, or the
// other end where there is none.
scav_point aim_of(RouteInput const &in, RouteNet const &net, uint32_t end) {
  if (net.waypoint_len == 0) { return (end == 0) ? net.dst : net.src; }
  return in.waypoints[net.waypoint_off + ((end == 0) ? 0 : (net.waypoint_len - 1))];
}

// An end's seat before alignment and spreading: on `face` where one is named
// (INVALID for the separation rule's), and at the aim for an end naming no box.
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

}  // namespace

uint32_t OrthogonalRouter::effective_faces(RouteInput const &in,
                                           uint32_t net,
                                           uint32_t end) const {
  RouteNet const &nt{ in.nets[net] };
  if (((end == 0) ? nt.src_obstacle : nt.dst_obstacle) >= in.obstacles.size()) {
    return 0;
  }
  int32_t const clear{ ortho_clearance(in.profile) };
  scav_point const ruled{ seat_of(in, nt, end, INVALID, clear) };
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
  vec_reserve(out.net_points, in.nets.size());
  vec_reserve(out.metrics, in.nets.size());

  Wide const bend{ ortho_bend_penalty(in.profile) };
  int32_t const clear{ ortho_clearance(in.profile) };
  int32_t const inset{ border_band(in.profile) };
  RouteScratch &sc{ route_scratch() };
  std::vector<scav_rect> &walls{ sc.walls };
  ortho_enclosure_walls(in.region, in.enclosure, inset, walls);
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

  // A route never leaves the region, which makes "avoid this frame's obstacles"
  // mean "avoid every box". An anchor outside it degrades that one net.
  int32_t const lo_x{ in.region.x };
  int32_t const hi_x{ in.region.x + in.region.w };
  int32_t const lo_y{ in.region.y };
  int32_t const hi_y{ in.region.y + in.region.h };
  auto const in_region = [&](scav_point at) {
    return (at.x >= lo_x) && (at.x <= hi_x) && (at.y >= lo_y) && (at.y <= hi_y);
  };

  // Each end is up to three points: the caller's, the border it attaches to, and
  // the ring `clear` out. Only the ring is searched, so that leg is square.
  std::vector<scav_point> &lead{ sc.lead };
  std::vector<scav_point> &anchors{ sc.anchors };
  std::vector<scav_span> &net_anchors{ sc.net_anchors };
  std::vector<scav_span> &net_lead{ sc.net_lead };
  std::vector<scav_span> &net_tail{ sc.net_tail };
  lead.clear();
  anchors.clear();
  vec_assign(net_anchors, in.nets.size(), scav_span{});
  vec_assign(net_lead, in.nets.size(), scav_span{});
  vec_assign(net_tail, in.nets.size(), scav_span{});

  auto const approach = [&](scav_point exact, uint32_t named, scav_point seated) {
    uint32_t box{ named };
    scav_point attach{ exact };
    if (box < in.obstacles.size()) {
      // A named box means `exact` is its centre, so the centre is dropped for
      // the seat the pass below chose on that box's border.
      attach = seated;
    } else {
      scav_point const moved{ ortho_escape(exact, seated, in.obstacles) };
      if ((moved.x != exact.x) || (moved.y != exact.y)) {
        // An exact end inside a box is 11.14's carve-out: keep it, and stub out to
        // the border to make it reachable.
        vec_push_back(lead, exact);
        attach = moved;
      }
      // An end in the enclosure's band is the enclosure's, even where a box
      // outside it touches the border there too: a port on the border, or an
      // inner face just inside it. The stub is square to that border and
      // reaches the band's inner edge, where the search begins.
      uint32_t const side{ band_side(attach) };
      box = (side != INVALID) ? INVALID : ortho_box_at(attach, in.obstacles);
      if (side != INVALID) {
        vec_push_back(lead, attach);
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
      // Clamped, not refused: a box on the frame's edge still gets a square approach.
      // Refusing puts the search back on the border line the grid exists to deny.
      scav_point ring{ ortho_ring(attach, in.obstacles[box], clear) };
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
        vec_push_back(lead, attach);
        at = ring;
      }
    }
    return at;
  };

  // Every end's seat is chosen before any net is approached, because two ends
  // wanting one seat is a question about the pair and not about either.
  std::vector<scav_point> &seat{ sc.seat };
  std::vector<scav_point> &toward{ sc.toward };
  vec_assign(seat, 2 * in.nets.size(), scav_point{});
  vec_assign(toward, 2 * in.nets.size(), scav_point{});
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    uint32_t const src_slot{ 2 * n };
    toward[src_slot] = aim_of(in, net, 0);
    toward[src_slot + 1] = aim_of(in, net, 1);
    seat[src_slot] = seat_of(in, net, 0, net.src_face, clear);
    seat[src_slot + 1] = seat_of(in, net, 1, net.dst_face, clear);
  }
  // Diffed at the call site rather than emitted inside each pass: what a reader
  // wants is which pass moved a seat, and only here sees all five (11.16).
  std::vector<scav_point> &was{ sc.was };
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

  // Faces first, since a glyph moved onto another face is a different line for
  // the two passes below to line up and pull apart.
  ortho_reface_attachments(in.nets, in.obstacles, in.inscribed, toward, seat);
  moved(SeatPass::Reface);
  ortho_align_attachments(in.nets, in.obstacles, in.inscribed, in.corner, seat);
  moved(SeatPass::Align);
  int32_t const pitch{ label_line_height(in.profile) };
  ortho_spread_attachments(in.nets,
                           in.obstacles,
                           in.inscribed,
                           in.corner,
                           toward,
                           clear,
                           pitch,
                           seat);
  moved(SeatPass::Spread);
  // Last: the only pass that compares two boxes, over seats the passes above settled.
  ortho_separate_attachments(in.nets,
                             in.obstacles,
                             in.inscribed,
                             in.corner,
                             toward,
                             clear,
                             pitch,
                             seat);
  moved(SeatPass::Separate);

  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    uint32_t const off{ static_cast<uint32_t>(anchors.size()) };
    uint32_t const lead_first{ static_cast<uint32_t>(lead.size()) };
    uint32_t const src_slot{ 2 * n };
    scav_point const from{ approach(net.src, net.src_obstacle, seat[src_slot]) };
    net_lead[n] = { .off = lead_first,
                    .len = static_cast<uint32_t>(lead.size()) - lead_first };

    vec_push_back(anchors, from);
    for (uint32_t k = 0; k < net.waypoint_len; ++k) {
      vec_push_back(anchors, in.waypoints[net.waypoint_off + k]);
    }
    if ((net.loop > 0) && (net.src_obstacle < in.obstacles.size()) &&
        (net.dst_obstacle < in.obstacles.size())) {
      // A loop's corridor runs `loop` out from each seat, kept inside the region
      // and off the enclosure's border.
      for (uint32_t end = 0; end < 2; ++end) {
        scav_point at{ seat[src_slot + end] };
        uint32_t const box{ (end == 0) ? net.src_obstacle : net.dst_obstacle };
        switch (face_of(at, in.obstacles[box])) {
          case 0: at.x -= net.loop; break;
          case 1: at.x += net.loop; break;
          case 2: at.y -= net.loop; break;
          case 3: at.y += net.loop; break;
          default: break;
        }
        int32_t x0{ lo_x };
        int32_t y0{ lo_y };
        int32_t x1{ hi_x };
        int32_t y1{ hi_y };
        if ((enc.w > (2 * inset)) && (enc.h > (2 * inset))) {
          x0 = imax(x0, enc.x + inset + 1);
          y0 = imax(y0, enc.y + inset + 1);
          x1 = imin(x1, (enc.x + enc.w) - inset - 1);
          y1 = imin(y1, (enc.y + enc.h) - inset - 1);
        }
        at.x = imin(imax(at.x, x0), x1);
        at.y = imin(imax(at.y, y0), y1);
        vec_push_back(anchors, at);
      }
    }
    uint32_t const tail_first{ static_cast<uint32_t>(lead.size()) };
    vec_push_back(anchors, approach(net.dst, net.dst_obstacle, seat[src_slot + 1]));
    net_tail[n] = { .off = tail_first,
                    .len = static_cast<uint32_t>(lead.size()) - tail_first };
    net_anchors[n] = { .off = off, .len = static_cast<uint32_t>(anchors.size()) - off };
  }

  OrthoGrid &g{ sc.g };
  bool const affordable{ ortho_grid(in.region, in.obstacles, anchors, clear, g, &walls) };

  // 11.5's re-seat, built only when needed: the same graph without bumpers. Two
  // boxes closer than twice the clearance seal the channel between them.
  OrthoGrid &tight{ sc.tight };
  bool tight_built{ false };
  bool tight_ok{ false };

  OrthoScratch &scratch{ sc.search };
  std::vector<uint32_t> &hop{ sc.hop };
  std::vector<scav_point> &piece{ sc.piece };
  std::vector<scav_point> &shape{ sc.shape };
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

    // The plane each end's lead or stub runs in, which a net with no corridor
    // leaves and arrives in; a corridor's legs keep both ends free.
    bool const direct{ at.len == 2 };
    uint32_t const src_plane{ (direct && (net_lead[n].len > 0))
                                  ? leg_plane(lead[net_lead[n].off + net_lead[n].len - 1],
                                              anchors[at.off])
                                  : INVALID };
    uint32_t const dst_plane{ (direct && (net_tail[n].len > 0))
                                  ? leg_plane(anchors[at.off + 1],
                                              lead[net_tail[n].off + net_tail[n].len - 1])
                                  : INVALID };

    auto const attempt = [&](OrthoGrid const &use) {
      // Through the simplifier like every other run, so a net whose ends resolve to
      // one place emits one point: a zero-length segment has no direction.
      shape.clear();
      piece.clear();
      for (uint32_t k = 0; k < net_lead[n].len; ++k) {
        vec_push_back(piece, lead[net_lead[n].off + k]);
      }
      vec_push_back(piece, anchors[at.off]);
      ortho_simplify(piece, shape);
      for (uint32_t k = 0; (k + 1) < at.len; ++k) {
        scav_point const a{ anchors[at.off + k] };
        scav_point const b{ anchors[at.off + k + 1] };
        uint32_t const v_from{ use.vertex(ortho_index_of(use.xs, a.x),
                                          ortho_index_of(use.ys, a.y)) };
        uint32_t const v_to{ use.vertex(ortho_index_of(use.xs, b.x),
                                        ortho_index_of(use.ys, b.y)) };
        if (!ortho_search(use, v_from, v_to, bend, scratch, hop, src_plane, dst_plane)) {
          shape.clear();
          return false;
        }
        piece.clear();
        for (uint32_t const v : hop) { vec_push_back(piece, use.point(v)); }
        ortho_simplify(piece, shape);
      }
      // The tail was built outward from the box, so it comes back inward.
      piece.clear();
      for (uint32_t k = net_tail[n].len; k-- > 0;) {
        vec_push_back(piece, lead[net_tail[n].off + k]);
      }
      ortho_simplify(piece, shape);
      return true;
    };

    int32_t reseated{ 0 };
    bool ok{ (why == RouteFailure::None) && attempt(g) };
    if ((why == RouteFailure::None) && !ok) {
      if (!tight_built) {
        tight_built = true;
        tight_ok = ortho_grid(in.region, in.obstacles, anchors, 0, tight, &walls);
      }
      ok = tight_ok && attempt(tight);
      if (ok) { reseated = 1; }
      if (!ok) { why = RouteFailure::Unreachable; }
    }

    if (!ok) {
      // Deterministic degradation, never a silent overlap: the straight line is what
      // the cost vector then scores as a Tier-0 violation.
      shape.clear();
      vec_push_back(shape, net.src);
      for (uint32_t k = 0; k < net.waypoint_len; ++k) {
        vec_push_back(shape, in.waypoints[net.waypoint_off + k]);
      }
      vec_push_back(shape, net.dst);
    }

    uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
    for (scav_point const &pt : shape) { vec_push_back(out.points, pt); }
    scav_span const span{ .off = off,
                          .len = static_cast<uint32_t>(out.points.size()) - off };
    vec_push_back(out.net_points, span);
    RouteMetrics m{ .bends = 0, .length = 0, .failed = why, .reseated = reseated };
    measure(out.points, span, m);
    vec_push_back(out.metrics, m);
  }
}

}  // namespace scav
