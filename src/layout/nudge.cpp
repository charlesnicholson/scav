// Finds the lanes two or more nets share, one axis at a time, and spreads their
// members onto integer offsets.

#include "layout/nudge.h"

#include "layout/geom.h"
#include "scav_int.h"
#include "scav_stable_sort.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

using Wide = int64_t;

// Past any room the coordinate domain can offer, so folding it in with `imin`
// leaves the real limits alone.
constexpr Wide UNBOUNDED{ Wide{ 1 } << 40 };

// An interior axis-aligned segment: `lo`/`hi` span its own axis and `at` is its
// coordinate across it, which keys the lane.
struct Member {
  uint32_t point{ 0 };  // index of the segment's first point
  uint32_t net{ 0 };    // -> nets, so a move is weighed against the other nets
  int32_t at{ 0 }, lo{ 0 }, hi{ 0 };
  Wide toward{ 0 };         // across the lane, where the net came from; see below
  Wide up{ 0 }, down{ 0 };  // how far the two dragged legs let it travel
  int32_t offset{ 0 };
};

// The bumper the router searched against.
scav_rect grow(scav_rect const &r, int32_t by) {
  return { .x = r.x - by, .y = r.y - by, .w = r.w + (2 * by), .h = r.h + (2 * by) };
}

bool contains(scav_rect const &outer, scav_rect const &r) {
  return (r.x >= outer.x) && (r.y >= outer.y) && ((r.x + r.w) <= (outer.x + outer.w)) &&
         ((r.y + r.h) <= (outer.y + outer.h));
}

// What two axis-aligned segments share of one line: zero unless they are
// collinear and meet in more than a point.
Wide shared_run(scav_point a, scav_point b, scav_point c, scav_point d) {
  if ((a.y == b.y) && (c.y == d.y) && (a.y == c.y)) {
    return imax(Wide{ 0 },
                Wide{ imin(imax(a.x, b.x), imax(c.x, d.x)) } -
                    imax(imin(a.x, b.x), imin(c.x, d.x)));
  }
  if ((a.x == b.x) && (c.x == d.x) && (a.x == c.x)) {
    return imax(Wide{ 0 },
                Wide{ imin(imax(a.y, b.y), imax(c.y, d.y)) } -
                    imax(imin(a.y, b.y), imin(c.y, d.y)));
  }
  return 0;
}

// A leg reaching the lane from one side must still reach it from that side; a
// `before` of nothing is a leg with no extent on this axis to keep.
bool kept(Wide before, Wide after) {
  return (before > 0) ? (after > 0) : ((before < 0) && (after < 0));
}

}  // namespace

void nudge_lanes(scav_rect const &region,
                 scav_rect const &bounds,
                 std::vector<scav_rect> const &obstacles,
                 int32_t gap,
                 int32_t clear,
                 std::vector<scav_span> const &nets,
                 std::vector<scav_point> &points,
                 NudgeStats &stats) {
  if (gap <= 0) { return; }
  uint32_t const net_count{ static_cast<uint32_t>(nets.size()) };

  std::vector<Member> members;
  std::vector<uint32_t> lane;
  for (uint32_t axis = 0; axis < 2; ++axis) {
    bool const horizontal{ axis == 0 };
    // Rebuilt from the live points per axis: a horizontal displacement drags the
    // vertical legs either side of it.
    members.clear();
    for (uint32_t net = 0; net < net_count; ++net) {
      scav_span const span{ nets[net] };
      if (span.len < 4) { continue; }  // needs a neighbour at each end of some segment
      for (uint32_t k = 1; (k + 2) < span.len; ++k) {
        uint32_t const i{ span.off + k };
        scav_point const a{ points[i - 1] };
        scav_point const b{ points[i] };
        scav_point const cpt{ points[i + 1] };
        scav_point const d{ points[i + 2] };
        if ((b.x == cpt.x) == (b.y == cpt.y)) { continue; }  // diagonal, or zero length
        if (horizontal != (b.y == cpt.y)) { continue; }
        Member m{ .point = i, .net = net };
        m.at = horizontal ? b.y : b.x;
        m.lo = horizontal ? imin(b.x, cpt.x) : imin(b.y, cpt.y);
        m.hi = horizontal ? imax(b.x, cpt.x) : imax(b.y, cpt.y);
        // Where the net was before it reached the lane, read from whichever end is
        // the lower along the lane's own axis so every member is measured alike.
        bool const forward{ horizontal ? (b.x < cpt.x) : (b.y < cpt.y) };
        scav_point const from{ forward ? a : d };
        m.toward = horizontal ? Wide{ from.y } : Wide{ from.x };
        // The two dragged legs, signed across the lane; each caps the travel one
        // short of turning itself round.
        Wide const u{ Wide{ m.at } - (horizontal ? a.y : a.x) };
        Wide const v{ (horizontal ? Wide{ d.y } : Wide{ d.x }) - m.at };
        m.up = UNBOUNDED;
        m.down = UNBOUNDED;
        if (u >= 0) { m.up = imin(m.up, u - 1); }
        if (u <= 0) { m.down = imin(m.down, -u - 1); }
        if (v >= 0) { m.down = imin(m.down, v - 1); }
        if (v <= 0) { m.up = imin(m.up, -v - 1); }
        m.up = imax(m.up, Wide{ 0 });
        m.down = imax(m.down, Wide{ 0 });
        members.push_back(m);
      }
    }
    if (members.size() < 2) { continue; }

    // Keyed so a lane's members are adjacent and in a total order the input's own
    // ordering cannot disturb.
    scav_stable_sort(members, [](Member const &x, Member const &y) {
      if (x.at != y.at) { return x.at < y.at; }
      if (x.lo != y.lo) { return x.lo < y.lo; }
      if (x.hi != y.hi) { return x.hi < y.hi; }
      return x.point < y.point;
    });

    for (uint32_t start = 0; start < members.size();) {
      // A lane is a run of collinear members whose extents chain into one another,
      // which is the same grouping `hi` carries forward as the sweep advances.
      uint32_t end{ start + 1 };
      int32_t reach{ members[start].hi };
      while ((end < members.size()) && (members[end].at == members[start].at) &&
             (members[end].lo < reach)) {
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
      scav_stable_sort(lane, [&members](uint32_t x, uint32_t y) {
        if (members[x].toward != members[y].toward) {
          return members[x].toward < members[y].toward;
        }
        return members[x].point < members[y].point;
      });

      // The room the whole bundle has, measured over the lane's union extent so a
      // member cannot be displaced into something a shorter neighbour cleared.
      int32_t const at{ members[first].at };
      int32_t const lo{ members[first].lo };
      int32_t const hi{ reach };
      Wide room_down{ horizontal ? (Wide{ region.y } + region.h) - at
                                 : (Wide{ region.x } + region.w) - at };
      Wide room_up{ horizontal ? (Wide{ at } - region.y) : (Wide{ at } - region.x) };
      // The frame's own box bounds the room; `region` reaches past it.
      room_up = imin(room_up, Wide{ at } - (horizontal ? bounds.y : bounds.x));
      room_down = imin(
          room_down,
          (horizontal ? (Wide{ bounds.y } + bounds.h) : (Wide{ bounds.x } + bounds.w)) -
              at);

      scav_rect const bar{ horizontal
                               ? scav_rect{ .x = lo, .y = at, .w = hi - lo, .h = 0 }
                               : scav_rect{ .x = at, .y = lo, .w = 0, .h = hi - lo } };
      for (scav_rect const &raw : obstacles) {
        // Only a box the lane runs alongside limits it.
        if (overlaps(bar, raw)) { continue; }
        scav_rect const box{ grow(raw, clear) };
        int32_t const span_lo{ horizontal ? box.x : box.y };
        int32_t const span_hi{ horizontal ? (box.x + box.w) : (box.y + box.h) };
        if ((span_hi <= lo) || (span_lo >= hi)) { continue; }  // not beside this lane
        // Sided against the raw rect but limited by the grown one, so a lane
        // inside the bumper is left no room at all towards the box.
        int32_t const raw_lo{ horizontal ? raw.y : raw.x };
        int32_t const raw_hi{ horizontal ? (raw.y + raw.h) : (raw.x + raw.w) };
        if (raw_hi <= at) {
          room_up =
              imin(room_up, Wide{ at } - (horizontal ? (box.y + box.h) : (box.x + box.w)));
        }
        if (raw_lo >= at) {
          room_down = imin(room_down, Wide{ horizontal ? box.y : box.x } - at);
        }
      }

      // Sized to what every member can drag, so the bundle stays evenly spaced.
      for (uint32_t j = 0; j < count; ++j) {
        room_up = imin(room_up, members[lane[j]].up);
        room_down = imin(room_down, members[lane[j]].down);
      }
      room_up = imax(room_up, Wide{ 0 });
      room_down = imax(room_down, Wide{ 0 });
      Wide const window{ room_up + room_down };
      if (window <= 0) { continue; }

      // The bundle fills the window, then slides back towards the lane as far as
      // it will go; with room both sides that centres it.
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

        // The segment and the two legs it drags: inside the region, no leg turned
        // round, nothing newly entered, no new run along another net.
        std::array<scav_point, 4> const then{ a, b, cpt, d };
        std::array<scav_point, 4> const way{ a, nb, nc, d };
        std::array<scav_rect, 3> const was{ span_rect(a, b),
                                            span_rect(b, cpt),
                                            span_rect(cpt, d) };
        std::array<scav_rect, 3> const now{ span_rect(a, nb),
                                            span_rect(nb, nc),
                                            span_rect(nc, d) };
        bool ok{ true };
        for (scav_rect const &r : now) { ok = ok && contains(region, r); }
        // The arrowhead reads its direction off the end pair, so no leg collapses.
        ok = ok && !(same(a, nb) || same(nc, d));
        // Second guard on the leg extents already folded into the room.
        ok = ok && kept(horizontal ? (Wide{ b.y } - a.y) : (Wide{ b.x } - a.x),
                        horizontal ? (Wide{ nb.y } - a.y) : (Wide{ nb.x } - a.x));
        ok = ok && kept(horizontal ? (Wide{ d.y } - cpt.y) : (Wide{ d.x } - cpt.x),
                        horizontal ? (Wide{ d.y } - nc.y) : (Wide{ d.x } - nc.x));
        for (scav_rect const &raw : obstacles) {
          if (!ok) { break; }
          // The raw rect is a hard wall and its bumper a soft one, each exempted
          // for the one leg that was already inside it rather than for the member.
          scav_rect const box{ grow(raw, clear) };
          for (uint32_t r = 0; r < now.size(); ++r) {
            ok = ok && (overlaps(was[r], raw) || !overlaps(now[r], raw));
            ok = ok && (overlaps(was[r], box) || !overlaps(now[r], box));
          }
        }
        // A leg may keep only a shared run it already had, so the lane a member
        // leaves is not traded for one it lands on.
        for (uint32_t q = 0; ok && (q < net_count); ++q) {
          if (q == m.net) { continue; }
          scav_span const other{ nets[q] };
          for (uint32_t k = 0; ok && ((k + 1) < other.len); ++k) {
            scav_point const s{ points[other.off + k] };
            scav_point const e{ points[other.off + k + 1] };
            for (uint32_t r = 0; r < now.size(); ++r) {
              if (shared_run(then[r], then[r + 1], s, e) > 0) { continue; }
              ok = ok && (shared_run(way[r], way[r + 1], s, e) == 0);
            }
          }
        }
        if (!ok) { continue; }
        points[i] = nb;
        points[i + 1] = nc;
        ++stats.moved;
      }
    }
  }
}

}  // namespace scav
