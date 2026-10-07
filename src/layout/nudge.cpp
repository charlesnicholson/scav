// Finds lanes of two or more segments, one axis at a time, and spreads their bundles onto
// integer offsets.

#include "layout/nudge.h"

#include "layout/geom.h"
#include "layout/partition.h"
#include "layout/trace.h"
#include "scav_int.h"
#include "scav_stable_sort.h"
#include "scav_vector.h"

#include <array>
#include <cstdint>

namespace scav {

namespace {

using Wide = int64_t;

// Exceeds any room in the coordinate domain; the starting value for `imin` over room.
constexpr Wide UNBOUNDED{ Wide{ 1 } << 40 };

// The in-degree of a bundle already ordered; exceeds any real in-degree.
constexpr uint32_t PLACED{ 0xffffffffU };

// An interior axis-aligned segment: `lo`/`hi` span its axis, `at` is its cross coordinate.
// A lane's members may differ in `at`; `reach_*` and `offset_to` take lane positions.
struct Member {
  uint32_t point{ 0 };  // index of the segment's first point
  uint32_t net{ 0 };    // -> nets
  int32_t at{ 0 }, lo{ 0 }, hi{ 0 };
  Wide toward{ 0 };         // cross coordinate of the far end of the low end's leg
  Wide up{ 0 }, down{ 0 };  // how far the two dragged legs let it travel
  // Side the legs at the segment's low and high ends leave by: -1 towards the lower
  // coordinate across the lane, +1 towards the higher, 0 for a zero-length leg.
  int32_t low_dir{ 0 }, high_dir{ 0 };
  int32_t offset{ 0 };  // displacement from `at`

  // Travel up and down from lane position `root` that this member allows; may be negative.
  [[nodiscard]] Wide reach_up(int32_t root) const { return up - (Wide{ at } - root); }
  [[nodiscard]] Wide reach_down(int32_t root) const { return down + (Wide{ at } - root); }

  // The displacement that puts this member at a lane position.
  [[nodiscard]] int32_t offset_to(Wide position) const {
    return static_cast<int32_t>(position - at);
  }
};

// -1, 0 or +1: which side of the lane a leg leaves by.
int32_t sign(Wide v) {
  if (v < 0) { return -1; }
  return (v > 0) ? 1 : 0;
}

// True when `after` is nonzero with the sign of `before`; false when `before` is 0.
bool kept(Wide before, Wide after) {
  return (before > 0) ? (after > 0) : ((before < 0) && (after < 0));
}

// True when the nets of `x` and `y` match point for point from the segment's second point
// to the end, or from the start to its first point.
bool bundled(Vector<scav_point> const &points,
             Vector<scav_span> const &nets,
             Member const &x,
             Member const &y) {
  scav_span const a{ nets[x.net] };
  scav_span const b{ nets[y.net] };
  uint32_t const a_end{ a.off + a.len };
  uint32_t const b_end{ b.off + b.len };
  bool tail{ (a_end - x.point) == (b_end - y.point) };
  for (uint32_t k = 1; tail && ((x.point + k) < a_end); ++k) {
    tail = same(points[x.point + k], points[y.point + k]);
  }
  if (tail) { return true; }
  bool head{ (x.point - a.off) == (y.point - b.off) };
  for (uint32_t k = 0; head && ((a.off + k) <= x.point); ++k) {
    head = same(points[a.off + k], points[b.off + k]);
  }
  return head;
}

// A net's axis-aligned segment starting at `point`, on the line `key` names.
struct OnLine {
  uint64_t key;
  uint32_t point, net;
};

// The line through `a` and `b`: its axis in the high word, its coordinate in the low.
uint64_t line_key(scav_point a, scav_point b) {
  bool const flat{ a.y == b.y };
  return (uint64_t{ flat ? 1U : 0U } << 32U) | static_cast<uint32_t>(flat ? a.y : a.x);
}

// True when `a` to `b` is axis-aligned with nonzero length.
bool axial(scav_point a, scav_point b) { return (a.x == b.x) != (a.y == b.y); }

// First index with `v[i].key >= key` in `v`, sorted by key.
uint32_t line_start(Vector<OnLine> const &v, uint64_t key) {
  uint32_t lo{ 0 };
  uint32_t hi{ static_cast<uint32_t>(v.size()) };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (v[mid].key < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// Per-thread buffers for one call, reassigned in place; a call never waits on the pool.
struct NudgeScratch {
  Vector<Member> members;
  Vector<uint32_t> lane;
  Partition link;         // -> members, the lanes of one axis
  Partition parent;       // -> lane, the bundles of one lane
  Vector<uint32_t> slot;  // -> lane, each entry's bundle, then that bundle's position
  Vector<uint32_t> sizes;
  Vector<uint32_t> group;
  Vector<uint32_t> kin;
  Vector<int32_t> votes;  // groups x groups, antisymmetric
  Vector<uint32_t> degree;
  Vector<uint32_t> order;
  Vector<uint32_t> rank;  // -> order, inverted
  // -> members: the next of each one's lane, and the last so far of each root's.
  Vector<uint32_t> next_member, last_member;
  Vector<Member> member_merge;  // the sorts' merge buffers
  Vector<uint32_t> lane_merge;
  // Every net's segments by line as the axis pass began, and those it has since moved.
  Vector<OnLine> lines, moved, line_merge;
};

NudgeScratch &nudge_scratch() {
  thread_local NudgeScratch s;
  return s;
}

}  // namespace

void nudge_lanes(scav_rect const &region,
                 Vector<scav_rect> const &bounds,
                 Vector<scav_rect> const &obstacles,
                 int32_t gap,
                 int32_t clear,
                 int32_t band,
                 Vector<scav_span> const &nets,
                 Vector<scav_point> &points,
                 scav_path_clear const *keep,
                 uint32_t n_keep) {
  if (gap <= 0) { return; }
  int32_t const inside{ imax(band, 1) };
  int32_t const near{ inside - 1 };  // `along_border`'s reach for `band`
  uint32_t const net_count{ static_cast<uint32_t>(nets.size()) };

  NudgeScratch &sc{ nudge_scratch() };
  Vector<Member> &members{ sc.members };
  Vector<uint32_t> &lane{ sc.lane };
  Partition &link{ sc.link };
  Partition &parent{ sc.parent };
  Vector<uint32_t> &slot{ sc.slot };
  Vector<uint32_t> &sizes{ sc.sizes };
  Vector<uint32_t> &group{ sc.group };
  Vector<uint32_t> &kin{ sc.kin };
  Vector<int32_t> &votes{ sc.votes };
  Vector<uint32_t> &degree{ sc.degree };
  Vector<uint32_t> &order{ sc.order };
  Vector<uint32_t> &rank{ sc.rank };
  Vector<uint32_t> &next_member{ sc.next_member };
  Vector<uint32_t> &last_member{ sc.last_member };
  kin.clear();
  for (uint32_t axis = 0; axis < 2; ++axis) {
    bool const horizontal{ axis == 0 };
    members.clear();
    for (uint32_t net = 0; net < net_count; ++net) {
      scav_span const span{ nets[net] };
      if (span.len < 4) { continue; }  // no interior segment
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
        // `from` is past the low end along the lane's axis; `to` is past the high end.
        bool const forward{ horizontal ? (b.x < cpt.x) : (b.y < cpt.y) };
        scav_point const from{ forward ? a : d };
        scav_point const to{ forward ? d : a };
        m.toward = horizontal ? Wide{ from.y } : Wide{ from.x };
        Wide const low_leg{ (horizontal ? Wide{ from.y } : Wide{ from.x }) - m.at };
        Wide const high_leg{ (horizontal ? Wide{ to.y } : Wide{ to.x }) - m.at };
        m.low_dir = sign(low_leg);
        m.high_dir = sign(high_leg);
        // The two dragged legs, signed across the lane; each keeps at least one unit of
        // length, plus `keep` on a net's first or last leg.
        Wide const u{ Wide{ m.at } - (horizontal ? a.y : a.x) };
        Wide const v{ (horizontal ? Wide{ d.y } : Wide{ d.x }) - m.at };
        bool const kept_net{ (keep != nullptr) && (net < n_keep) };
        bool const first_leg{ kept_net && (k == 1) };
        bool const last_leg{ kept_net && ((k + 3) == span.len) };
        Wide const keep_u{ 1 + (first_leg ? imax(keep[net].src, 0) : 0) };
        Wide const keep_v{ 1 + (last_leg ? imax(keep[net].dst, 0) : 0) };
        m.up = UNBOUNDED;
        m.down = UNBOUNDED;
        if (u >= 0) { m.up = imin(m.up, u - keep_u); }
        if (u <= 0) { m.down = imin(m.down, -u - keep_u); }
        if (v >= 0) { m.down = imin(m.down, v - keep_v); }
        if (v <= 0) { m.up = imin(m.up, -v - keep_v); }
        m.up = imax(m.up, Wide{ 0 });
        m.down = imax(m.down, Wide{ 0 });
        members.push_back(m);
      }
    }
    if (members.size() < 2) { continue; }

    // Built on the pass's first `known_good`.
    Vector<OnLine> &lines{ sc.lines };
    Vector<OnLine> &moved{ sc.moved };
    bool indexed{ false };
    lines.clear();
    moved.clear();
    auto const index_lines = [&]() {
      indexed = true;
      for (uint32_t net = 0; net < net_count; ++net) {
        scav_span const span{ nets[net] };
        for (uint32_t k = 0; (k + 1) < span.len; ++k) {
          uint32_t const i{ span.off + k };
          if (!axial(points[i], points[i + 1])) { continue; }
          lines.push_back(
              { .key = line_key(points[i], points[i + 1]), .point = i, .net = net });
        }
      }
      scav_stable_sort(lines, sc.line_merge, [](OnLine const &x, OnLine const &y) {
        return (x.key != y.key) ? (x.key < y.key) : (x.point < y.point);
      });
    };

    scav_stable_sort(members, sc.member_merge, [](Member const &x, Member const &y) {
      if (x.at != y.at) { return x.at < y.at; }
      if (x.lo != y.lo) { return x.lo < y.lo; }
      if (x.hi != y.hi) { return x.hi < y.hi; }
      return x.point < y.point;
    });

    // True when `m` moved by its offset stays in `region` with its legs on their sides,
    // entering no new obstacle, bumper, border run, or run along a net outside `kin`.
    auto const known_good = [&](Member const &m) {
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
      // Both dragged legs keep nonzero length.
      ok = ok && !(same(a, nb) || same(nc, d));
      // Each dragged leg still meets the segment from its side.
      ok = ok && kept(horizontal ? (Wide{ b.y } - a.y) : (Wide{ b.x } - a.x),
                      horizontal ? (Wide{ nb.y } - a.y) : (Wide{ nb.x } - a.x));
      ok = ok && kept(horizontal ? (Wide{ d.y } - cpt.y) : (Wide{ d.x } - cpt.x),
                      horizontal ? (Wide{ d.y } - nc.y) : (Wide{ d.x } - nc.x));
      for (scav_rect const &raw : obstacles) {
        if (!ok) { break; }
        // A leg may overlap the obstacle or its bumper only if it already did.
        scav_rect const box{ grow(raw, clear) };
        for (uint32_t r = 0; r < now.size(); ++r) {
          ok = ok && (overlaps(was[r], raw) || !overlaps(now[r], raw));
          ok = ok && (overlaps(was[r], box) || !overlaps(now[r], box));
          // A leg may run within `band` of the obstacle's border only if it already did.
          ok = ok && (along_border(then[r], then[r + 1], raw, near) ||
                      !along_border(way[r], way[r + 1], raw, near));
        }
      }
      // A leg may share a run with another net only if it already did; the three segments
      // around each `kin` point move with the bundle and are exempt.
      if (!indexed) { index_lines(); }
      auto const shares = [&](uint32_t r, OnLine const &on) {
        scav_point const s0{ points[on.point] };
        scav_point const e0{ points[on.point + 1] };
        if ((on.net == m.net) || !axial(s0, e0) || (line_key(s0, e0) != on.key)) {
          return false;
        }
        for (uint32_t const p : kin) {
          if (((on.point + 1) >= p) && (on.point <= (p + 1))) { return false; }
        }
        return (shared_run(then[r], then[r + 1], s0, e0) == 0) &&
               (shared_run(way[r], way[r + 1], s0, e0) > 0);
      };
      for (uint32_t r = 0; ok && (r < now.size()); ++r) {
        if (!axial(way[r], way[r + 1])) { continue; }
        uint64_t const key{ line_key(way[r], way[r + 1]) };
        for (Vector<OnLine> const *v : { &lines, &moved }) {
          for (uint32_t j = line_start(*v, key);
               ok && (j < v->size()) && ((*v)[j].key == key);
               ++j) {
            ok = !shares(r, (*v)[j]);
          }
        }
      }
      return ok;
    };

    // Links pairs under `gap` apart across the axis and overlapping along it into lanes.
    link.reset(members.size());
    for (uint32_t i = 0; i < members.size(); ++i) {
      for (uint32_t j = i + 1;
           (j < members.size()) && ((Wide{ members[j].at } - members[i].at) < gap);
           ++j) {
        if ((members[j].lo < members[i].hi) && (members[i].lo < members[j].hi)) {
          link.join(i, j);
        }
      }
    }
    // Threads each lane's members in ascending order from its root, its least member.
    next_member.assign(members.size(), INVALID);
    last_member.resize(members.size());
    for (uint32_t i = 0; i < members.size(); ++i) {
      uint32_t const root{ link.root(i) };
      if (root != i) { next_member[last_member[root]] = i; }
      last_member[root] = i;
    }
    for (uint32_t first = 0; first < members.size(); ++first) {
      if (!link.leads(first)) { continue; }
      lane.clear();
      int32_t reach{ members[first].hi };
      int32_t least{ members[first].lo };
      for (uint32_t i = first; i != INVALID; i = next_member[i]) {
        lane.push_back(i);
        reach = imax(reach, members[i].hi);
        least = imin(least, members[i].lo);
      }
      uint32_t const count{ static_cast<uint32_t>(lane.size()) };
      if (count < 2) { continue; }

      scav_stable_sort(lane, sc.lane_merge, [&members](uint32_t x, uint32_t y) {
        if (members[x].toward != members[y].toward) {
          return members[x].toward < members[y].toward;
        }
        return members[x].point < members[y].point;
      });

      // Members whose nets already run as one form a bundle sharing one offset; bundles
      // are numbered by their first member, in `toward` order.
      parent.reset(count);
      for (uint32_t j = 0; j < count; ++j) {
        for (uint32_t q = j + 1; q < count; ++q) {
          if (bundled(points, nets, members[lane[j]], members[lane[q]])) {
            parent.join(j, q);
          }
        }
      }
      slot.resize(count);
      uint32_t groups{ 0 };
      for (uint32_t j = 0; j < count; ++j) {
        if (!parent.leads(j)) { continue; }
        slot[j] = groups;
        ++groups;
      }
      for (uint32_t j = 0; j < count; ++j) { slot[j] = slot[parent.root(j)]; }
      sizes.assign(groups, 0);
      for (uint32_t j = 0; j < count; ++j) { ++sizes[slot[j]]; }
      uint32_t merged{ 0 };
      for (uint32_t const n : sizes) { merged += (n > 1) ? 1U : 0U; }
      TraceLaneFound found{ .horizontal = horizontal ? 1U : 0U,
                            .at = members[first].at,
                            .members = count,
                            .bundles = groups,
                            .merged = merged,
                            .reordered = 0,
                            .spread = 0 };
      if (groups < 2) {
        trace_emit({ .kind = TraceKind::LaneFound, .found = found });
        continue;
      }

      // Each leg leaving the lane strictly inside another member's extent votes for the
      // order that keeps it off that member's segment; `votes` is antisymmetric.
      uint32_t const cells{ groups * groups };
      votes.assign(cells, 0);
      for (uint32_t j = 0; j < count; ++j) {
        for (uint32_t q = 0; q < count; ++q) {
          if (slot[j] == slot[q]) { continue; }
          Member const &m{ members[lane[j]] };
          Member const &n{ members[lane[q]] };
          int32_t v{ 0 };
          if ((m.lo > n.lo) && (m.lo < n.hi)) { v -= m.low_dir; }
          if ((m.hi > n.lo) && (m.hi < n.hi)) { v -= m.high_dir; }
          if ((n.lo > m.lo) && (n.lo < m.hi)) { v += n.low_dir; }
          if ((n.hi > m.lo) && (n.hi < m.hi)) { v += n.high_dir; }
          votes[(slot[j] * groups) + slot[q]] += v;
        }
      }

      // Kahn's algorithm over edges `u` -> `v` where `votes[u][v] > 0`, taking the
      // lowest-key ready bundle first; key order when there are no votes.
      degree.assign(groups, 0);
      for (uint32_t u = 0; u < groups; ++u) {
        for (uint32_t v = 0; v < groups; ++v) {
          if (votes[(u * groups) + v] > 0) { ++degree[v]; }
        }
      }
      order.clear();
      while (order.size() < groups) {
        uint32_t next{ groups };
        for (uint32_t b = 0; (next == groups) && (b < groups); ++b) {
          if (degree[b] == 0) { next = b; }
        }
        if (next == groups) { break; }  // every bundle left is preceded by another
        order.push_back(next);
        degree[next] = PLACED;
        for (uint32_t v = 0; v < groups; ++v) {
          if (votes[(next * groups) + v] > 0) { --degree[v]; }
        }
      }
      if (order.size() < groups) {
        // On a vote cycle, inserts bundles in key order, each at the position with the
        // least contradicted vote weight, the last such position on a tie.
        order.clear();
        for (uint32_t b = 0; b < groups; ++b) {
          uint32_t best{ 0 };
          Wide best_cost{ -1 };
          for (uint32_t at = 0; at <= order.size(); ++at) {
            Wide cost{ 0 };
            for (uint32_t q = 0; q < order.size(); ++q) {
              int32_t const w{ votes[(b * groups) + order[q]] };
              // Inserted at `at`, `b` precedes `order[q]` for `q >= at`; each vote
              // against that placement adds its weight to `cost`.
              if ((q >= at) == (w < 0)) { cost += (w < 0) ? -Wide{ w } : Wide{ w }; }
            }
            if ((best_cost < 0) || (cost <= best_cost)) {
              best_cost = cost;
              best = at;
            }
          }
          order.push_back(b);
          for (uint32_t k = static_cast<uint32_t>(order.size()) - 1; k > best; --k) {
            order[k] = order[k - 1];
          }
          order[best] = b;
        }
      }
      rank.assign(groups, 0);
      bool keyed{ true };
      for (uint32_t i = 0; i < groups; ++i) {
        rank[order[i]] = i;
        keyed = keyed && (order[i] == i);
      }
      found.reordered = keyed ? 0U : 1U;
      for (uint32_t j = 0; j < count; ++j) { slot[j] = rank[slot[j]]; }

      // The lane's room over its members' union extent `[lo, hi]`, measured from the root,
      // which has the lane's lowest `at`.
      int32_t const at{ members[first].at };
      int32_t const lo{ least };
      int32_t const hi{ reach };
      Wide room_down{ UNBOUNDED };
      Wide room_up{ UNBOUNDED };
      // Room stops one unit inside `region`, and `inside` within every member's `bounds`
      // frame and every box the lane passes through.
      auto const hold = [&](scav_rect const &r, int32_t by) {
        Wide const r_lo{ horizontal ? r.y : r.x };
        Wide const r_hi{ r_lo + (horizontal ? r.h : r.w) };
        room_up = imin(room_up, (Wide{ at } - r_lo) - by);
        room_down = imin(room_down, (r_hi - at) - by);
      };
      hold(region, 1);
      for (uint32_t j = 0; j < count; ++j) {
        uint32_t const net{ members[lane[j]].net };
        if (net < bounds.size()) { hold(bounds[net], inside); }
      }

      scav_rect const bar{ horizontal
                               ? scav_rect{ .x = lo, .y = at, .w = hi - lo, .h = 0 }
                               : scav_rect{ .x = at, .y = lo, .w = 0, .h = hi - lo } };
      for (scav_rect const &raw : obstacles) {
        if (overlaps(bar, raw)) {
          hold(raw, inside);
          continue;
        }
        scav_rect const box{ grow(raw, clear) };
        int32_t const span_lo{ horizontal ? box.x : box.y };
        int32_t const span_hi{ horizontal ? (box.x + box.w) : (box.y + box.h) };
        if ((span_hi <= lo) || (span_lo >= hi)) { continue; }  // not beside this lane
        // Picks the side by the raw rect and limits room by the `clear`-grown one; when
        // `clear` is 0 the room stops one unit short of the border.
        int32_t const raw_lo{ horizontal ? raw.y : raw.x };
        int32_t const raw_hi{ horizontal ? (raw.y + raw.h) : (raw.x + raw.w) };
        Wide const short_of{ (clear > 0) ? 0 : 1 };
        if (raw_hi <= at) {
          room_up = imin(
              room_up,
              Wide{ at } - (horizontal ? (box.y + box.h) : (box.x + box.w)) - short_of);
        }
        if (raw_lo >= at) {
          room_down = imin(room_down, Wide{ horizontal ? box.y : box.x } - at - short_of);
        }
      }

      // Clamps room at 0 where the lane lies inside a box's bumper.
      room_up = imax(room_up, Wide{ 0 });
      room_down = imax(room_down, Wide{ 0 });

      for (uint32_t j = 0; j < count; ++j) {
        Member const &m{ members[lane[j]] };
        room_up = imin(room_up, m.reach_up(at));
        room_down = imin(room_down, m.reach_down(at));
      }
      Wide const window{ room_up + room_down };

      // Bundles sit `step` apart, at most `gap`, centred on the lane where both sides have
      // room, else shifted to fit the window.
      Wide const step{ imin(Wide{ gap }, window / (groups - 1)) };
      bool any{ false };
      if (step > 0) {
        Wide const spread{ (groups - 1) * step };
        Wide const lowest{ imax(-room_up, imin(-(spread / 2), room_down - spread)) };
        for (uint32_t j = 0; j < count; ++j) {
          Member &m{ members[lane[j]] };
          m.offset = m.offset_to(Wide{ at } + lowest + (Wide{ slot[j] } * step));
          if (m.offset != 0) { any = true; }
        }
      }
      found.spread = any ? 1U : 0U;
      trace_emit({ .kind = TraceKind::LaneFound, .found = found });
      if (!any) { continue; }

      // Files the segment starting at `point` under its current line, once indexed.
      auto const record = [&](uint32_t point, uint32_t net) {
        if (!indexed || !axial(points[point], points[point + 1])) { return; }
        OnLine const on{ .key = line_key(points[point], points[point + 1]),
                         .point = point,
                         .net = net };
        uint32_t at_key{ line_start(moved, on.key) };
        while ((at_key < moved.size()) && (moved[at_key].key == on.key) &&
               (moved[at_key].point < on.point)) {
          ++at_key;
        }
        moved.insert(moved.begin() + at_key, { on });
      };
      for (uint32_t b = 0; b < groups; ++b) {
        group.clear();
        for (uint32_t j = 0; j < count; ++j) {
          if (slot[j] == b) { group.push_back(lane[j]); }
        }
        if (members[group[0]].offset == 0) { continue; }
        // Checks every member of the bundle before moving any; one failure leaves the
        // whole bundle in place.
        bool ok{ true };
        for (uint32_t const i : group) {
          kin.clear();
          for (uint32_t const other : group) {
            if (other != i) { kin.push_back(members[other].point); }
          }
          ok = ok && known_good(members[i]);
        }
        if (!ok) {
          Member const &m{ members[group[0]] };
          trace_emit({ .kind = TraceKind::BundleRefused,
                       .bundle = { .net = m.net,
                                   .lane = b,
                                   .members = static_cast<uint32_t>(group.size()),
                                   .to = m.at + m.offset } });
          continue;
        }
        for (uint32_t const i : group) {
          Member const &m{ members[i] };
          bool const low_axial{ axial(points[m.point - 1], points[m.point]) };
          bool const high_axial{ axial(points[m.point + 1], points[m.point + 2]) };
          if (horizontal) {
            points[m.point].y += m.offset;
            points[m.point + 1].y += m.offset;
          } else {
            points[m.point].x += m.offset;
            points[m.point + 1].x += m.offset;
          }
          // The moved segment's new line, and a dragged leg's if the move made it axial.
          record(m.point, m.net);
          if (!low_axial) { record(m.point - 1, m.net); }
          if (!high_axial) { record(m.point + 1, m.net); }
          trace_emit(
              { .kind = TraceKind::LaneAssigned,
                .lane = { .net = m.net,
                          .lane = b,
                          .at = horizontal ? points[m.point].y : points[m.point].x } });
        }
      }
    }
  }
}

}  // namespace scav
