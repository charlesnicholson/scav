// The `orthogonal` router: a visibility graph per frame, h-plane and v-plane
// copies joined by an edge weighing the bend penalty, and A* over that (11.5).

#include "layout/router_orthogonal.h"

#include "layout/geom.h"
#include "layout/router.h"
#include "scav_int.h"
#include "scav_stable_sort.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

// `f`, then `g`, then node. `node` carries vertex and plane, so no two states
// compare equal and the pop order cannot depend on the push order.
bool frontier_before(Wide fa, Wide ga, uint32_t na, Wide fb, Wide gb, uint32_t nb) {
  if (fa != fb) { return fa < fb; }
  if (ga != gb) { return ga < gb; }
  return na < nb;
}

void heap_swap(OrthoScratch &s, uint32_t a, uint32_t b) {
  Wide const f{ s.heap_f[a] };
  Wide const g{ s.heap_g[a] };
  uint32_t const node{ s.heap_node[a] };
  s.heap_f[a] = s.heap_f[b];
  s.heap_g[a] = s.heap_g[b];
  s.heap_node[a] = s.heap_node[b];
  s.heap_f[b] = f;
  s.heap_g[b] = g;
  s.heap_node[b] = node;
}

// `<algorithm>` is outside this library's header subset (6).
void heap_push(OrthoScratch &s, Wide f, Wide g, uint32_t node) {
  s.heap_f.push_back(f);
  s.heap_g.push_back(g);
  s.heap_node.push_back(node);
  uint32_t i{ static_cast<uint32_t>(s.heap_node.size()) - 1 };
  while (i > 0) {
    uint32_t const parent{ (i - 1) / 2 };
    if (!frontier_before(s.heap_f[i],
                         s.heap_g[i],
                         s.heap_node[i],
                         s.heap_f[parent],
                         s.heap_g[parent],
                         s.heap_node[parent])) {
      break;
    }
    heap_swap(s, i, parent);
    i = parent;
  }
}

void heap_pop(OrthoScratch &s, Wide &f, Wide &g, uint32_t &node) {
  f = s.heap_f[0];
  g = s.heap_g[0];
  node = s.heap_node[0];
  s.heap_f[0] = s.heap_f.back();
  s.heap_g[0] = s.heap_g.back();
  s.heap_node[0] = s.heap_node.back();
  s.heap_f.pop_back();
  s.heap_g.pop_back();
  s.heap_node.pop_back();
  uint32_t i{ 0 };
  auto const n{ static_cast<uint32_t>(s.heap_node.size()) };
  for (;;) {
    uint32_t const left{ (2 * i) + 1 };
    uint32_t best{ i };
    if ((left < n) && frontier_before(s.heap_f[left],
                                      s.heap_g[left],
                                      s.heap_node[left],
                                      s.heap_f[best],
                                      s.heap_g[best],
                                      s.heap_node[best])) {
      best = left;
    }
    if (((left + 1) < n) && frontier_before(s.heap_f[left + 1],
                                            s.heap_g[left + 1],
                                            s.heap_node[left + 1],
                                            s.heap_f[best],
                                            s.heap_g[best],
                                            s.heap_node[best])) {
      best = left + 1;
    }
    if (best == i) { break; }
    heap_swap(s, i, best);
    i = best;
  }
}

// How far outside `[lo, lo + len]` a coordinate lies, zero anywhere inside it.
// Measured from the span rather than from a border point, so a face's length
// does not count against its own perpendicular exit (11.5).
Wide beyond(int32_t v, int32_t lo, int32_t len) {
  Wide const hi{ Wide{ lo } + len };
  if (v < lo) { return Wide{ lo } - v; }
  if (Wide{ v } > hi) { return Wide{ v } - hi; }
  return Wide{ 0 };
}

// A coordinate onto the face `[lo, lo + len]`, held `clear` off each corner or
// at the middle where the face has no room for that: an end on a corner leaves
// along the face it did not pick.
int32_t onto_face(int32_t v, int32_t lo, int32_t len, int32_t clear) {
  int32_t const inset{ imin(clear, len / 2) };
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

void ortho_sort_unique(std::vector<int32_t> &v) {
  scav_stable_sort(v, [](int32_t a, int32_t b) { return a < b; });
  uint32_t kept{ 0 };
  for (uint32_t i = 0; i < v.size(); ++i) {
    if ((kept == 0) || (v[i] != v[kept - 1])) { v[kept++] = v[i]; }
  }
  v.resize(kept);
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
  // The dominant separation picks the axis, a tie going to the layering axis x.
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

scav_point ortho_attach_box(scav_point toward,
                            scav_rect const &r,
                            int32_t clear,
                            bool inscribed) {
  // Along the face, `toward`'s own projection onto it, so the end leaves aimed
  // at where it is going. A box centre carries no such information and every
  // net naming one face of one box would otherwise be handed the same point.
  // An inscribed glyph has one point per face and it is the midpoint, spelled
  // the way the reference builder spells the centre it draws the mark on: an
  // inset of half the face leaves two units to choose from on an odd one, and
  // the upper of them misses the tangent.
  scav_point aimed{ toward };
  if (ortho_escape_horizontal(toward, r)) {
    aimed.y = inscribed ? (r.y + (r.h / 2)) : onto_face(toward.y, r.y, r.h, clear);
  } else {
    aimed.x = inscribed ? (r.x + (r.w / 2)) : onto_face(toward.x, r.x, r.w, clear);
  }
  return ortho_escape_box(aimed, toward, r);
}

void ortho_reface_attachments(std::vector<RouteNet> const &nets,
                              std::vector<scav_rect> const &boxes,
                              std::vector<uint8_t> const &inscribed,
                              std::vector<scav_point> const &toward,
                              std::vector<scav_point> &at) {
  std::vector<Seat> seats;
  for (uint32_t n = 0; n < nets.size(); ++n) {
    for (uint32_t end = 0; end < 2; ++end) {
      uint32_t const box{ (end == 0) ? nets[n].src_obstacle : nets[n].dst_obstacle };
      if ((box >= boxes.size()) || (box >= inscribed.size()) || (inscribed[box] == 0)) {
        continue;
      }
      uint32_t const slot{ (2 * n) + end };
      uint32_t const face{ face_of(at[slot], boxes[box]) };
      if (face == INVALID) { continue; }
      seats.push_back({ .box = box, .face = face, .end = end, .slot = slot, .pos = 0 });
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
                             int32_t clear,
                             std::vector<scav_point> &at) {
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

    // What each face can seat: its whole run less the corner inset the
    // projection is held off by, or the one midpoint an inscribed glyph offers.
    int32_t lo{ 0 };
    int32_t hi{ 0 };
    Wide centres{ 0 };
    for (uint32_t k = 0; k < 2; ++k) {
      uint32_t const box{ (k == 0) ? src_box : dst_box };
      scav_rect const &r{ boxes[box] };
      int32_t const start{ along_y ? r.y : r.x };
      int32_t const len{ along_y ? r.h : r.w };
      bool const one_point{ (box < inscribed.size()) && (inscribed[box] != 0) };
      int32_t const inset{ one_point ? (len / 2) : imin(clear, len / 2) };
      int32_t const face_lo{ start + inset };
      int32_t const face_hi{ one_point ? face_lo : ((start + len) - inset) };
      lo = (k == 0) ? face_lo : imax(lo, face_lo);
      hi = (k == 0) ? face_hi : imin(hi, face_hi);
      centres += Wide{ start } + (len / 2);
    }
    if (lo > hi) { continue; }  // no coordinate both faces can seat

    // Halfway between the two centres, which is symmetric in the pair, so a net
    // and its reverse land on the same line rather than on two a step apart.
    int32_t const want{ static_cast<int32_t>(floor_div(centres, Wide{ 2 })) };
    int32_t const got{ imin(imax(want, lo), hi) };
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
                              int32_t clear,
                              std::vector<scav_point> &at) {
  if (clear <= 0) { return; }
  std::vector<Seat> seats;
  for (uint32_t n = 0; n < nets.size(); ++n) {
    for (uint32_t end = 0; end < 2; ++end) {
      uint32_t const box{ (end == 0) ? nets[n].src_obstacle : nets[n].dst_obstacle };
      bool const one_point{ (box < inscribed.size()) && (inscribed[box] != 0) };
      if ((box >= boxes.size()) || one_point) { continue; }
      uint32_t const slot{ (2 * n) + end };
      uint32_t const face{ face_of(at[slot], boxes[box]) };
      if (face == INVALID) { continue; }
      seats.push_back({ .box = box, .face = face, .end = end, .slot = slot, .pos = 0 });
    }
  }

  // One sweep of the face: everything arriving at a point is one fan-in and
  // everything leaving it is one fan-out, each a trunk a reader wants whole and
  // 11.5's bundles exist to keep. What no trunk explains is an arrival and a
  // departure on one point, where the head is inked along the other route's own
  // first leg -- so the run seats by direction and the members of a direction
  // keep the point they share.
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
      uint32_t const split{ seats[end - 1].end - seats[first].end };
      if (split == 0) { continue; }
      scav_rect const &r{ boxes[seats[first].box] };
      bool const along_y{ seats[first].face < 2 };
      int32_t const lo{ along_y ? r.y : r.x };
      int32_t const len{ along_y ? r.h : r.w };
      // Sized to the face, so a mark too small to seat both leaves them stacked
      // rather than sliding one off its own border.
      int32_t const step{ imin(clear, len / 3) };
      if (step <= 0) { continue; }
      for (uint32_t i = first; i < end; ++i) {
        Seat const &seat{ seats[i] };
        // Which way the net travels through the face rather than which end of
        // it this is: out through a right or a bottom face runs +, and in
        // through a left or a top face runs + as well. The one running + takes
        // the lower seat at both of its ends, so a pair seated apart at one box
        // is seated the same way apart at the other and stays two straight
        // lines; at any one face this is still exactly arrival against
        // departure, so a fan-in and a fan-out each keep their one point.
        bool const forward{ (seat.end == 0) == ((seat.face == 1) || (seat.face == 3)) };
        int32_t const want{ seat.pos + (forward ? -(step / 2) : (step - (step / 2))) };
        int32_t const got{ onto_face(want, lo, len, clear) };
        int32_t &held{ along_y ? at[seat.slot].y : at[seat.slot].x };
        if (held != got) {
          held = got;
          moved = true;
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
    if (!skip) { to.push_back(at); }
  }
}

bool ortho_grid(scav_rect const &region,
                std::vector<scav_rect> const &obstacles,
                std::vector<scav_point> const &anchors,
                int32_t clear,
                OrthoGrid &out) {
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
  // Grown sides only: a line on a box edge is a lane, and hugging is free. Ring
  // points sit here, so denying the true border costs no reachability.
  for (scav_rect const &r : obstacles) {
    for (int32_t const at : { r.x - clear, r.x + r.w + clear }) {
      if ((at > lo_x) && (at < hi_x)) { out.xs.push_back(at); }
    }
    for (int32_t const at : { r.y - clear, r.y + r.h + clear }) {
      if ((at > lo_y) && (at < hi_y)) { out.ys.push_back(at); }
    }
  }
  for (scav_point const &at : anchors) {
    // Both coordinates or neither: an anchor outside the region is rejected
    // anyway and should not seed half a crossing inside it.
    if ((at.x < lo_x) || (at.x > hi_x) || (at.y < lo_y) || (at.y > hi_y)) { continue; }
    out.xs.push_back(at.x);
    out.ys.push_back(at.y);
  }
  ortho_sort_unique(out.xs);
  ortho_sort_unique(out.ys);
  if ((Wide{ out.nx() } * out.ny()) > ORTHO_VERTEX_BUDGET) { return false; }

  out.pass_h.assign(static_cast<size_t>(out.ny()) * (out.nx() - 1), 1);
  out.pass_v.assign(static_cast<size_t>(out.ny() - 1) * out.nx(), 1);
  for (scav_rect const &box : obstacles) {
    // The bumper: blocking against it makes "no closer than `clear` to a box" a
    // property of the graph.
    scav_rect const r{ .x = box.x - clear,
                       .y = box.y - clear,
                       .w = box.w + (2 * clear),
                       .h = box.h + (2 * clear) };
    for (uint32_t iy = 0; iy < out.ny(); ++iy) {
      for (uint32_t ix = 0; (ix + 1) < out.nx(); ++ix) {
        if (ortho_blocks_h(r, out.ys[iy], out.xs[ix], out.xs[ix + 1])) {
          out.pass_h[(iy * (out.nx() - 1)) + ix] = 0;
        }
      }
    }
    for (uint32_t iy = 0; (iy + 1) < out.ny(); ++iy) {
      for (uint32_t ix = 0; ix < out.nx(); ++ix) {
        if (ortho_blocks_v(r, out.xs[ix], out.ys[iy], out.ys[iy + 1])) {
          out.pass_v[(iy * out.nx()) + ix] = 0;
        }
      }
    }
  }
  return true;
}

bool ortho_search(OrthoGrid const &g,
                  uint32_t from,
                  uint32_t to,
                  Wide bend,
                  OrthoScratch &s,
                  std::vector<uint32_t> &out) {
  out.clear();
  uint32_t const vertices{ g.nx() * g.ny() };
  if ((vertices == 0) || (from >= vertices) || (to >= vertices)) { return false; }
  if (g.pass_h.empty() && (g.nx() > 1)) { return false; }
  uint32_t const nodes{ vertices * 2 };
  if (s.stamp.size() != nodes) {
    s.stamp.assign(nodes, 0);
    s.best.assign(nodes, 0);
    s.parent.assign(nodes, INVALID);
    s.generation = 0;
  }
  ++s.generation;
  uint32_t const gen{ s.generation };
  scav_point const goal{ g.point(to) };

  // Manhattan plus one bend for an axis this plane cannot cover alone. Both are
  // lower bounds, so the sum admits.
  auto const heuristic = [&](uint32_t node) {
    scav_point const at{ g.point(node / 2) };
    Wide const dx{ (at.x < goal.x) ? (Wide{ goal.x } - at.x) : (Wide{ at.x } - goal.x) };
    Wide const dy{ (at.y < goal.y) ? (Wide{ goal.y } - at.y) : (Wide{ at.y } - goal.y) };
    bool const turn{ ((node % 2) == 0) ? (dy != 0) : (dx != 0) };
    return dx + dy + (turn ? bend : Wide{ 0 });
  };

  s.heap_f.clear();
  s.heap_g.clear();
  s.heap_node.clear();
  for (uint32_t plane = 0; plane < 2; ++plane) {
    uint32_t const node{ (from * 2) + plane };
    s.stamp[node] = gen;
    s.best[node] = 0;
    s.parent[node] = INVALID;
    heap_push(s, heuristic(node), 0, node);
  }

  uint32_t expansions{ 0 };
  uint32_t reached{ INVALID };
  while (!s.heap_node.empty()) {
    Wide top_f{ 0 };
    Wide top_g{ 0 };
    uint32_t node{ 0 };
    heap_pop(s, top_f, top_g, node);
    if ((s.stamp[node] != gen) || (top_g != s.best[node])) { continue; }
    if ((node / 2) == to) {
      reached = node;
      break;
    }
    if (++expansions > ORTHO_EXPANSION_BUDGET) { return false; }

    uint32_t const v{ node / 2 };
    uint32_t const plane{ node % 2 };
    uint32_t const ix{ v % g.nx() };
    uint32_t const iy{ v / g.nx() };

    auto const relax = [&](uint32_t next, Wide step) {
      Wide const g_next{ top_g + step };
      if ((s.stamp[next] == gen) && (s.best[next] <= g_next)) { return; }
      s.stamp[next] = gen;
      s.best[next] = g_next;
      s.parent[next] = node;
      heap_push(s, g_next + heuristic(next), g_next, next);
    };

    relax((v * 2) + (1 - plane), bend);  // the turn, then this plane's moves
    if (plane == 0) {
      if (((ix + 1) < g.nx()) && (g.pass_h[(iy * (g.nx() - 1)) + ix] != 0)) {
        relax(g.vertex(ix + 1, iy) * 2, Wide{ g.xs[ix + 1] } - g.xs[ix]);
      }
      if ((ix > 0) && (g.pass_h[(iy * (g.nx() - 1)) + (ix - 1)] != 0)) {
        relax(g.vertex(ix - 1, iy) * 2, Wide{ g.xs[ix] } - g.xs[ix - 1]);
      }
    } else {
      if (((iy + 1) < g.ny()) && (g.pass_v[(iy * g.nx()) + ix] != 0)) {
        relax((g.vertex(ix, iy + 1) * 2) + 1, Wide{ g.ys[iy + 1] } - g.ys[iy]);
      }
      if ((iy > 0) && (g.pass_v[((iy - 1) * g.nx()) + ix] != 0)) {
        relax((g.vertex(ix, iy - 1) * 2) + 1, Wide{ g.ys[iy] } - g.ys[iy - 1]);
      }
    }
  }
  if (reached == INVALID) { return false; }

  s.path.clear();
  for (uint32_t node = reached; node != INVALID; node = s.parent[node]) {
    s.path.push_back(node / 2);
  }
  for (auto i = static_cast<uint32_t>(s.path.size()); i-- > 0;) {
    if (out.empty() || (out.back() != s.path[i])) { out.push_back(s.path[i]); }
  }
  return true;
}

Wide ortho_bend_penalty(scav_profile const &p) {
  // Not 11.6's exchange rate, which is sixteen grid units and buys a staircase
  // wherever the grid offers one. A profile field for it is P9's calibration.
  return imax(Wide{ p.rank_sep }, Wide{ 1 });
}

int32_t ortho_clearance(scav_profile const &p) { return imax(p.node_sep / 3, 1); }

int32_t OrthogonalRouter::margin(scav_profile const &p) const {
  return ortho_clearance(p);
}

void OrthogonalRouter::route(RouteInput const &in, RouteOutput &out) const {
  out.points.clear();
  out.net_points.clear();
  out.metrics.clear();
  out.net_points.reserve(in.nets.size());
  out.metrics.reserve(in.nets.size());

  Wide const bend{ ortho_bend_penalty(in.profile) };
  int32_t const clear{ ortho_clearance(in.profile) };

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
  std::vector<scav_point> lead;
  std::vector<scav_point> anchors;
  std::vector<scav_span> net_anchors(in.nets.size(), scav_span{});
  std::vector<scav_span> net_lead(in.nets.size(), scav_span{});
  std::vector<scav_span> net_tail(in.nets.size(), scav_span{});

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
        lead.push_back(exact);
        attach = moved;
      }
      box = ortho_box_at(attach, in.obstacles);
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
      if (ok) {
        lead.push_back(attach);
        at = ring;
      }
    }
    return at;
  };

  // Every end's seat is chosen before any net is approached, because two ends
  // wanting one seat is a question about the pair and not about either.
  std::vector<scav_point> seat(2 * in.nets.size(), scav_point{});
  std::vector<scav_point> toward(2 * in.nets.size(), scav_point{});
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    scav_point const after{ (net.waypoint_len != 0) ? in.waypoints[net.waypoint_off]
                                                    : net.dst };
    scav_point const before{ (net.waypoint_len != 0)
                                 ? in.waypoints[net.waypoint_off + net.waypoint_len - 1]
                                 : net.src };
    auto const glyph = [&in](uint32_t box) {
      return (box < in.inscribed.size()) && (in.inscribed[box] != 0);
    };
    uint32_t const src_slot{ 2 * n };
    uint32_t const dst_slot{ src_slot + 1 };
    toward[src_slot] = after;
    toward[dst_slot] = before;
    seat[src_slot] = (net.src_obstacle < in.obstacles.size())
                         ? ortho_attach_box(after,
                                            in.obstacles[net.src_obstacle],
                                            clear,
                                            glyph(net.src_obstacle))
                         : after;
    seat[dst_slot] = (net.dst_obstacle < in.obstacles.size())
                         ? ortho_attach_box(before,
                                            in.obstacles[net.dst_obstacle],
                                            clear,
                                            glyph(net.dst_obstacle))
                         : before;
  }
  // Faces first, since a glyph moved onto another face is a different line for
  // the two passes below to line up and pull apart.
  ortho_reface_attachments(in.nets, in.obstacles, in.inscribed, toward, seat);
  ortho_align_attachments(in.nets, in.obstacles, in.inscribed, clear, seat);
  ortho_spread_attachments(in.nets, in.obstacles, in.inscribed, clear, seat);

  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    uint32_t const off{ static_cast<uint32_t>(anchors.size()) };
    uint32_t const lead_first{ static_cast<uint32_t>(lead.size()) };
    uint32_t const src_slot{ 2 * n };
    scav_point const from{ approach(net.src, net.src_obstacle, seat[src_slot]) };
    net_lead[n] = { .off = lead_first,
                    .len = static_cast<uint32_t>(lead.size()) - lead_first };

    anchors.push_back(from);
    for (uint32_t k = 0; k < net.waypoint_len; ++k) {
      anchors.push_back(in.waypoints[net.waypoint_off + k]);
    }
    uint32_t const tail_first{ static_cast<uint32_t>(lead.size()) };
    anchors.push_back(approach(net.dst, net.dst_obstacle, seat[src_slot + 1]));
    net_tail[n] = { .off = tail_first,
                    .len = static_cast<uint32_t>(lead.size()) - tail_first };
    net_anchors[n] = { .off = off, .len = static_cast<uint32_t>(anchors.size()) - off };
  }

  OrthoGrid g;
  bool const affordable{ ortho_grid(in.region, in.obstacles, anchors, clear, g) };

  // 11.5's re-seat, built only when needed: the same graph without bumpers. Two
  // boxes closer than twice the clearance seal the channel between them.
  OrthoGrid tight;
  bool tight_built{ false };
  bool tight_ok{ false };

  OrthoScratch scratch;
  std::vector<uint32_t> hop;
  std::vector<scav_point> piece;
  std::vector<scav_point> shape;
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

    auto const attempt = [&](OrthoGrid const &use) {
      // Through the simplifier like every other run, so a net whose ends resolve to
      // one place emits one point: a zero-length segment has no direction.
      shape.clear();
      piece.clear();
      for (uint32_t k = 0; k < net_lead[n].len; ++k) {
        piece.push_back(lead[net_lead[n].off + k]);
      }
      piece.push_back(anchors[at.off]);
      ortho_simplify(piece, shape);
      for (uint32_t k = 0; (k + 1) < at.len; ++k) {
        scav_point const a{ anchors[at.off + k] };
        scav_point const b{ anchors[at.off + k + 1] };
        uint32_t const v_from{ use.vertex(ortho_index_of(use.xs, a.x),
                                          ortho_index_of(use.ys, a.y)) };
        uint32_t const v_to{ use.vertex(ortho_index_of(use.xs, b.x),
                                        ortho_index_of(use.ys, b.y)) };
        if (!ortho_search(use, v_from, v_to, bend, scratch, hop)) {
          shape.clear();
          return false;
        }
        piece.clear();
        for (uint32_t const v : hop) { piece.push_back(use.point(v)); }
        ortho_simplify(piece, shape);
      }
      // The tail was built outward from the box, so it comes back inward.
      piece.clear();
      for (uint32_t k = net_tail[n].len; k-- > 0;) {
        piece.push_back(lead[net_tail[n].off + k]);
      }
      ortho_simplify(piece, shape);
      return true;
    };

    int32_t reseated{ 0 };
    bool ok{ (why == RouteFailure::None) && attempt(g) };
    if ((why == RouteFailure::None) && !ok) {
      if (!tight_built) {
        tight_built = true;
        tight_ok = ortho_grid(in.region, in.obstacles, anchors, 0, tight);
      }
      ok = tight_ok && attempt(tight);
      if (ok) { reseated = 1; }
      if (!ok) { why = RouteFailure::Unreachable; }
    }

    if (!ok) {
      // Deterministic degradation, never a silent overlap: the straight line is what
      // the cost vector then scores as a Tier-0 violation.
      shape.clear();
      shape.push_back(net.src);
      for (uint32_t k = 0; k < net.waypoint_len; ++k) {
        shape.push_back(in.waypoints[net.waypoint_off + k]);
      }
      shape.push_back(net.dst);
    }

    uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
    for (scav_point const &pt : shape) { out.points.push_back(pt); }
    scav_span const span{ .off = off,
                          .len = static_cast<uint32_t>(out.points.size()) - off };
    out.net_points.push_back(span);
    RouteMetrics m{ .bends = 0, .length = 0, .failed = why, .reseated = reseated };
    measure(out.points, span, m);
    out.metrics.push_back(m);
  }
}

}  // namespace scav
