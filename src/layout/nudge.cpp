// Nudging: find the lanes two or more nets share, order the members across the
// lane, and displace them onto integer offsets (11.5).

#include "layout/nudge.h"

#include "scav_int.h"
#include "scav_stable_sort.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

using Wide = int64_t;

// A segment that may move: interior to its net, axis-aligned, and longer than
// nothing. `lo`/`hi` are its extent along its own axis and `at` its position
// across it, which is the coordinate a lane is keyed on.
struct Member {
  uint32_t point{ 0 };  // index of the segment's first point
  uint32_t axis{ 0 };   // 0 = horizontal, so `at` is y; 1 = vertical
  int32_t at{ 0 }, lo{ 0 }, hi{ 0 };
  Wide toward{ 0 };  // across the lane, where the net came from; see below
  int32_t offset{ 0 };
};

bool same(scav_point a, scav_point b) { return (a.x == b.x) && (a.y == b.y); }

bool overlaps_rect(scav_rect const &a, scav_rect const &b) {
  return (a.x < (b.x + b.w)) && (b.x < (a.x + a.w)) && (a.y < (b.y + b.h)) &&
         (b.y < (a.y + a.h));
}

// A segment as a rect, so one predicate covers a segment and a box. Zero on the
// thin axis, which `overlaps_rect` treats as touching-not-overlapping -- exactly
// the reading a route lying along a border needs.
scav_rect span_rect(scav_point a, scav_point b) {
  int32_t const x{ imin(a.x, b.x) };
  int32_t const y{ imin(a.y, b.y) };
  return { .x = x, .y = y, .w = imax(a.x, b.x) - x, .h = imax(a.y, b.y) - y };
}

// Obstacles block against their rect grown by `clear`, the same bumper the
// router searches against (11.5), so a displaced segment cannot come to rest
// hugging a border the router was at pains to stand off from.
scav_rect grow(scav_rect const &r, int32_t by) {
  return { .x = r.x - by, .y = r.y - by, .w = r.w + (2 * by), .h = r.h + (2 * by) };
}

bool inside(scav_rect const &outer, scav_rect const &r) {
  return (r.x >= outer.x) && (r.y >= outer.y) && ((r.x + r.w) <= (outer.x + outer.w)) &&
         ((r.y + r.h) <= (outer.y + outer.h));
}

}  // namespace

void nudge_lanes(scav_rect const &region,
                 std::vector<scav_rect> const &obstacles,
                 int32_t gap,
                 int32_t clear,
                 std::vector<scav_span> const &nets,
                 std::vector<scav_point> &points,
                 NudgeStats &stats) {
  if (gap <= 0) { return; }

  std::vector<Member> members;
  for (scav_span const &net : nets) {
    if (net.len < 4) { continue; }  // needs a neighbour at each end of some segment
    for (uint32_t k = 1; (k + 2) < net.len; ++k) {
      uint32_t const i{ net.off + k };
      scav_point const a{ points[i] };
      scav_point const b{ points[i + 1] };
      if ((a.x == b.x) == (a.y == b.y)) { continue; }  // diagonal, or zero length
      bool const horizontal{ a.y == b.y };
      Member m{ .point = i, .axis = horizontal ? 0U : 1U };
      m.at = horizontal ? a.y : a.x;
      m.lo = horizontal ? imin(a.x, b.x) : imin(a.y, b.y);
      m.hi = horizontal ? imax(a.x, b.x) : imax(a.y, b.y);
      // Where the net was before it reached the lane, taken from whichever end
      // is the lower along the lane's own axis so every member is measured from
      // the same side. Ordering by it is 11.5's projection rule at its simplest:
      // a net arriving from above stays above, and the members do not swap and
      // pay a crossing for the privilege. Ordering by both ends instead reads
      // worse -- 145 corpus crossings against 129 -- because a net that changes
      // side has no consistent answer and ends up placed by its tie-break.
      bool const forward{ horizontal ? (a.x < b.x) : (a.y < b.y) };
      scav_point const from{ forward ? points[i - 1] : points[i + 2] };
      m.toward = horizontal ? Wide{ from.y } : Wide{ from.x };
      members.push_back(m);
    }
  }
  if (members.size() < 2) { return; }

  // Keyed so a lane's members are adjacent and in a total order the input's own
  // ordering cannot disturb.
  scav_stable_sort(members, [](Member const &a, Member const &b) {
    if (a.axis != b.axis) { return a.axis < b.axis; }
    if (a.at != b.at) { return a.at < b.at; }
    if (a.lo != b.lo) { return a.lo < b.lo; }
    if (a.hi != b.hi) { return a.hi < b.hi; }
    return a.point < b.point;
  });

  std::vector<uint32_t> lane;
  for (uint32_t start = 0; start < members.size();) {
    // A lane is a run of collinear members whose extents chain into one another,
    // which is the same grouping `hi` carries forward as the sweep advances.
    uint32_t end{ start + 1 };
    int32_t reach{ members[start].hi };
    while ((end < members.size()) && (members[end].axis == members[start].axis) &&
           (members[end].at == members[start].at) && (members[end].lo < reach)) {
      reach = imax(reach, members[end].hi);
      ++end;
    }
    uint32_t const first{ start };
    start = end;
    uint32_t const count{ end - first };
    if (count < 2) { continue; }
    ++stats.lanes;

    lane.clear();
    for (uint32_t i = first; i < end; ++i) { lane.push_back(i); }
    scav_stable_sort(lane, [&members](uint32_t a, uint32_t b) {
      if (members[a].toward != members[b].toward) {
        return members[a].toward < members[b].toward;
      }
      return members[a].point < members[b].point;
    });

    // The room the whole bundle has, measured over the lane's union extent so a
    // member cannot be displaced into something a shorter neighbour cleared.
    bool const horizontal{ members[first].axis == 0 };
    int32_t const at{ members[first].at };
    int32_t const lo{ members[first].lo };
    int32_t const hi{ reach };
    Wide room_down{ horizontal ? (Wide{ region.y } + region.h) - at
                               : (Wide{ region.x } + region.w) - at };
    Wide room_up{ horizontal ? (Wide{ at } - region.y) : (Wide{ at } - region.x) };
    scav_rect const bar{ horizontal
                             ? scav_rect{ .x = lo, .y = at, .w = hi - lo, .h = 0 }
                             : scav_rect{ .x = at, .y = lo, .w = 0, .h = hi - lo } };
    for (scav_rect const &raw : obstacles) {
      scav_rect const box{ grow(raw, clear) };
      // Only a box the lane runs alongside can limit it; one it already overlaps
      // is one the members were routed through legitimately (11.14) and says
      // nothing about the room either side.
      if (overlaps_rect(bar, box)) { continue; }
      int32_t const near_lo{ horizontal ? box.y : box.x };
      int32_t const near_hi{ horizontal ? (box.y + box.h) : (box.x + box.w) };
      int32_t const span_lo{ horizontal ? box.x : box.y };
      int32_t const span_hi{ horizontal ? (box.x + box.w) : (box.y + box.h) };
      if ((span_hi <= lo) || (span_lo >= hi)) { continue; }  // not beside this lane
      if (near_hi <= at) { room_up = imin(room_up, Wide{ at } - near_hi); }
      if (near_lo >= at) { room_down = imin(room_down, Wide{ near_lo } - at); }
    }
    room_up = imax(room_up, Wide{ 0 });
    room_down = imax(room_down, Wide{ 0 });
    Wide const window{ room_up + room_down };
    if (window <= 0) { continue; }

    // The window is not symmetric about the lane -- a box one side and open
    // space the other is the common case -- so the bundle is sized to the whole
    // of it and then slid back towards the lane as far as it will go. Centred
    // where there is room both sides, which is the same expression.
    Wide const step{ imin(Wide{ gap }, window / (count - 1)) };
    if (step <= 0) { continue; }
    Wide const bundle{ (count - 1) * step };
    Wide const lowest{ imax(-room_up, imin(-(bundle / 2), room_down - bundle)) };

    bool any{ false };
    for (uint32_t j = 0; j < count; ++j) {
      Member &m{ members[lane[j]] };
      m.offset = static_cast<int32_t>(lowest + (Wide{ j } * step));
      if (m.offset != 0) { any = true; }
    }
    if (!any) { continue; }
    ++stats.spread;

    for (uint32_t j = 0; j < count; ++j) {
      Member const &m{ members[lane[j]] };
      if (m.offset == 0) { continue; }
      uint32_t const i{ m.point };
      scav_point const a{ points[i - 1] };
      scav_point const b{ points[i] };
      scav_point const cpt{ points[i + 1] };
      scav_point const d{ points[i + 2] };
      scav_point nb{ b };
      scav_point nc{ cpt };
      if (horizontal) {
        nb.y += m.offset;
        nc.y += m.offset;
      } else {
        nb.x += m.offset;
        nc.x += m.offset;
      }

      // Known good or not at all: the segment and the two legs it drags may not
      // enter anything they were not already in, and may not leave the region.
      std::array<scav_rect, 3> const was{ span_rect(a, b),
                                          span_rect(b, cpt),
                                          span_rect(cpt, d) };
      std::array<scav_rect, 3> const now{ span_rect(a, nb),
                                          span_rect(nb, nc),
                                          span_rect(nc, d) };
      bool ok{ true };
      for (scav_rect const &r : now) { ok = ok && inside(region, r); }
      // A leg collapsed to nothing is a polyline with no direction at its end,
      // and the arrowhead reads its direction off exactly that pair of points.
      ok = ok && !(same(a, nb) || same(nc, d));
      for (scav_rect const &raw : obstacles) {
        if (!ok) { break; }
        scav_rect const box{ grow(raw, clear) };
        bool before{ false };
        for (scav_rect const &r : was) { before = before || overlaps_rect(r, box); }
        if (before) { continue; }
        for (scav_rect const &r : now) { ok = ok && !overlaps_rect(r, box); }
      }
      if (!ok) { continue; }
      points[i] = nb;
      points[i + 1] = nc;
      ++stats.moved;
    }
  }
}

}  // namespace scav
