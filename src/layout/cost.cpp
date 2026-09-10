// Scoring, over the rects and polylines the phases produced. Every predicate
// here is degree 2: an intersection is four `orient2d` sign tests and the
// point is never constructed, so nothing needs more than int64 (11.2).

#include "layout/cost.h"

#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/route.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_stable_sort.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
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
SCAV_INTERNAL_END

namespace {

// The cell array is the square of this, so a frame past a few thousand
// children shares cells rather than growing one.
constexpr uint32_t GRID_SIDE_MAX{ 64 };

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
  return static_cast<Wide>(isqrt(static_cast<uint64_t>((dx * dx) + (dy * dy))));
}

// What two axis-aligned segments share of one line: zero unless they are
// collinear and meet in more than a point.
Wide shared_run(scav_point a, scav_point b, scav_point c, scav_point d) {
  bool const flat{ (a.y == b.y) && (c.y == d.y) && (a.y == c.y) };
  bool const upright{ (a.x == b.x) && (c.x == d.x) && (a.x == c.x) };
  if (!(flat || upright)) { return 0; }
  Wide const alo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
  Wide const ahi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
  Wide const clo{ flat ? imin(c.x, d.x) : imin(c.y, d.y) };
  Wide const chi{ flat ? imax(c.x, d.x) : imax(c.y, d.y) };
  return imax(Wide{ 0 }, imin(ahi, chi) - imax(alo, clo));
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

// **The exemption is one shared kink into a common destination, and nothing
// else** (11.9.3). Two routes may read as one line where they are arriving at
// the same state, because the reader has one thing to follow them to; two
// routes leaving the same state read as one line going somewhere ambiguous,
// and the fan-out that used to be excused here is exactly that. So the head is
// no longer counted, and the tail is capped at the final leg -- two shared
// points -- rather than at however much suffix happens to coincide.
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

// Segment `k` of one route of the pair lies in the trunk: the final leg they
// arrive on as one, or the leg that merges into it. `len` counts that route's
// points.
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

}  // namespace

SCAV_INTERNAL_BEGIN

Ancestry cost_flatten_ancestry(Chart const &c) {
  Ancestry out;
  out.tin.assign(c.states.size(), 0);
  out.tout.assign(c.states.size(), 0);
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
      stack.push_back(
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
    stack.push_back({ .state = at.state, .open = 0, .buried = 0 });
    uint32_t const under{ ((at.buried != 0) || (c.states[at.state].live == 0)) ? 1U : 0U };
    Span const subs{ c.states[at.state].submachines };
    for (uint32_t i = subs.len; i-- > 0;) {
      push_children(c.submachine_ids[subs.off + i].v, under);
    }
  }

  for (uint32_t s = 0; s < c.states.size(); ++s) {
    if ((c.states[s].live != 0) && ((out.tin[s] == 0) || (buried[s] != 0))) {
      out.detached.push_back(s);
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

ChildGrid cost_child_grid(Chart const &c, SizedLayout const &z) {
  ChildGrid g;
  g.frame.assign(c.submachines.size(), ChildGrid::Frame{});

  uint32_t cells{ 0 };
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    ChildGrid::Frame &f{ g.frame[m] };
    uint32_t const first{ static_cast<uint32_t>(g.child.size()) };
    Wide x0{ 0 };
    Wide y0{ 0 };
    Wide x1{ 0 };
    Wide y1{ 0 };
    Span const kids{ c.submachines[m].children };
    for (uint32_t i = 0; i < kids.len; ++i) {
      uint32_t const st{ c.state_ids[kids.off + i].v };
      if ((st >= z.state.size()) || (c.states[st].live == 0)) { continue; }
      scav_rect const r{ z.state[st] };
      if (g.child.size() == first) {
        x0 = r.x;
        y0 = r.y;
        x1 = Wide{ r.x } + r.w;
        y1 = Wide{ r.y } + r.h;
      } else {
        x0 = imin(x0, Wide{ r.x });
        y0 = imin(y0, Wide{ r.y });
        x1 = imax(x1, Wide{ r.x } + r.w);
        y1 = imax(y1, Wide{ r.y } + r.h);
      }
      g.child.push_back(st);
    }
    f.children = make_span(first, static_cast<uint32_t>(g.child.size()) - first);
    if (f.children.len == 0) { continue; }
    f.side = grid_side(f.children.len);
    f.x0 = static_cast<int32_t>(x0);
    f.y0 = static_cast<int32_t>(y0);
    f.cell_w = imax(Wide{ 1 }, ceil_div(x1 - x0, Wide{ f.side }));
    f.cell_h = imax(Wide{ 1 }, ceil_div(y1 - y0, Wide{ f.side }));
    f.bucket = cells;
    cells += f.side * f.side;
  }

  // Count into the cell after each, prefix-sum, then place through a cursor
  // copy: the usual two passes, so a child spanning cells is stored in each.
  g.bucket_off.assign(cells + 1, 0);
  auto const spread = [&g, &z](uint32_t m, auto step) {
    ChildGrid::Frame const &f{ g.frame[m] };
    for (uint32_t i = 0; i < f.children.len; ++i) {
      scav_rect const r{ z.state[g.child[f.children.off + i]] };
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
  };
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (g.frame[m].children.len != 0) {
      spread(m, [&g](uint32_t cell, uint32_t) { ++g.bucket_off[cell + 1]; });
    }
  }
  for (uint32_t i = 1; i < g.bucket_off.size(); ++i) {
    g.bucket_off[i] += g.bucket_off[i - 1];
  }
  g.bucket_at.assign(g.bucket_off.back(), 0);
  std::vector<uint32_t> cursor{ g.bucket_off };
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (g.frame[m].children.len != 0) {
      spread(m, [&g, &cursor](uint32_t cell, uint32_t child) {
        g.bucket_at[cursor[cell]] = child;
        ++cursor[cell];
      });
    }
  }
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
  if (out.stamp.size() < g.child.size()) { out.stamp.assign(g.child.size(), 0); }
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
        out.hit.push_back(child);
      }
    }
  }
}

// Siblings may not overlap; a nested box legitimately does, so the pairs are
// taken within one submachine's children and through that frame's own grid.
int32_t cost_box_overlaps(Chart const &c, SizedLayout const &z, ChildGrid const &g) {
  int32_t total{ 0 };
  GridQuery q;
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    Span const kids{ g.frame[m].children };
    for (uint32_t i = 0; i < kids.len; ++i) {
      uint32_t const a{ g.child[kids.off + i] };
      cost_grid_query(g, m, z.state[a], q);
      for (uint32_t const at : q.hit) {
        // Span order inside a frame, so a pair is charged from its first
        // member alone however many cells the two share.
        if (at <= (kids.off + i)) { continue; }
        if (overlaps(z.state[a], z.state[g.child[at]])) { ++total; }
      }
    }
  }
  return total;
}

// Tier 0, top down. A piece meets a box's interior only if it meets every box
// enclosing it, so a frame whose child the piece misses prunes that child's
// whole subtree; the test that prunes is the piece's bounding box against the
// rect, which unlike `enters` cannot answer false for an enclosing box.
int32_t cost_through_boxes(Chart const &c,
                           SizedLayout const &z,
                           Ancestry const &an,
                           ChildGrid const &g,
                           std::vector<Piece> const &pieces) {
  std::vector<uint32_t> roots;
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].owner.v == INVALID) { roots.push_back(m); }
  }

  int32_t total{ 0 };
  GridQuery q;
  std::vector<uint32_t> stack;
  for (Piece const &piece : pieces) {
    Transition const &tr{ c.transitions[piece.trans] };
    scav_rect const reach{ span_rect(piece.a, piece.b) };
    // An edge may occupy the interior of a state it is an endpoint of or a
    // descendant of, and only that one: 11.14's carve-out.
    auto const charge = [&](uint32_t st) {
      if (enters(piece.a, piece.b, z.state[st]) && !cost_ancestor(c, an, { st }, tr.src) &&
          !cost_ancestor(c, an, { st }, tr.dst)) {
        ++total;
      }
    };
    for (uint32_t const st : an.detached) { charge(st); }

    stack.assign(roots.begin(), roots.end());
    while (!stack.empty()) {
      uint32_t const frame{ stack.back() };
      stack.pop_back();
      cost_grid_query(g, frame, reach, q);
      for (uint32_t const at : q.hit) {
        uint32_t const st{ g.child[at] };
        if (!overlaps(reach, z.state[st])) { continue; }
        charge(st);
        Span const subs{ c.states[st].submachines };
        for (uint32_t i = 0; i < subs.len; ++i) {
          stack.push_back(c.submachine_ids[subs.off + i].v);
        }
      }
    }
  }
  return total;
}

// Two axis-parallel segments are collinear or never meet, so neither pair of
// horizontals nor pair of verticals can cross: the sweep is each vertical
// against the band of horizontals whose y lies strictly inside its span. A
// degraded net's diagonal is in neither set and goes against everything.
int64_t cost_crossings(std::vector<Piece> const &pieces,
                       std::vector<uint32_t> &per_trans) {
  std::vector<uint32_t> flat;
  std::vector<uint32_t> upright;
  std::vector<uint32_t> loose;
  std::vector<uint8_t> is_loose(pieces.size(), 0);
  for (uint32_t i = 0; i < pieces.size(); ++i) {
    int32_t at{ 0 };
    switch (piece_axis(pieces[i], at)) {
      case 0: flat.push_back(i); break;
      case 1: upright.push_back(i); break;
      default:
        loose.push_back(i);
        is_loose[i] = 1;
        break;
    }
  }
  scav_stable_sort(flat, [&pieces](uint32_t x, uint32_t y) {
    return pieces[x].a.y < pieces[y].a.y;
  });

  int64_t total{ 0 };
  auto const charge = [&](uint32_t i, uint32_t j) {
    ++total;
    ++per_trans[pieces[i].trans];
    ++per_trans[pieces[j].trans];
  };

  for (uint32_t const v : upright) {
    Piece const &pv{ pieces[v] };
    int32_t const lo{ imin(pv.a.y, pv.b.y) };
    int32_t const hi{ imax(pv.a.y, pv.b.y) };
    // First horizontal past `lo`: the band is open at both ends, a horizontal
    // through one of the vertical's own endpoints being a touch.
    uint32_t low{ 0 };
    uint32_t high{ static_cast<uint32_t>(flat.size()) };
    while (low < high) {
      uint32_t const mid{ low + ((high - low) >> 1U) };
      if (pieces[flat[mid]].a.y <= lo) {
        low = mid + 1;
      } else {
        high = mid;
      }
    }
    for (uint32_t at = low; (at < flat.size()) && (pieces[flat[at]].a.y < hi); ++at) {
      uint32_t const h{ flat[at] };
      if (pieces[h].trans == pv.trans) { continue; }
      if (crosses(pv.a, pv.b, pieces[h].a, pieces[h].b)) { charge(v, h); }
    }
  }

  for (uint32_t const d : loose) {
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

// Corridor is the length one pair shares along one line, so a run three nets
// lie on is charged over its three pairs. A shared run implies collinearity,
// so the pairs are taken inside a bucket keyed by the line's axis and
// coordinate. Two routes fanning into one trunk are not that run.
Wide cost_corridor(Routes const &r, std::vector<Piece> const &pieces) {
  struct Lane {
    uint32_t axis;
    int32_t at;
    uint32_t piece;
  };
  std::vector<Lane> lanes;
  lanes.reserve(pieces.size());
  for (uint32_t i = 0; i < pieces.size(); ++i) {
    int32_t at{ 0 };
    uint32_t const axis{ piece_axis(pieces[i], at) };
    if (axis < 2) { lanes.push_back({ .axis = axis, .at = at, .piece = i }); }
  }
  scav_stable_sort(lanes, [](Lane const &x, Lane const &y) {
    return (x.axis != y.axis) ? (x.axis < y.axis) : (x.at < y.at);
  });

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
      }
    }
    lo = hi;
  }
  return total;
}

SCAV_INTERNAL_END

CostTerms cost_terms(Chart const &c,
                     SplitGraph const &g,
                     SizedLayout const &z,
                     Routes const &r,
                     scav_spaces const &s,
                     scav_profile const &p) {
  CostTerms t;
  if (!geometry_complete(c, z, r)) { return t; }
  t.aspect = (Wide{ z.chart.w } * p.dar_den) - (Wide{ z.chart.h } * p.dar_num);
  if (t.aspect < 0) { t.aspect = -t.aspect; }
  t.area = Wide{ z.chart.w } * z.chart.h;

  // Every route segment once, with the transition it belongs to, so the
  // sweeps below are over one flat list.
  std::vector<Piece> pieces;
  std::vector<uint32_t> crossings_of(c.transitions.size(), 0);
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    scav_span const route{ r.route[tr] };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      pieces.push_back({ .a = r.points[route.off + k],
                         .b = r.points[route.off + k + 1],
                         .trans = tr,
                         .k = k });
      if ((k + 2) < route.len) {
        if (direction(r.points[route.off + k], r.points[route.off + k + 1]) !=
            direction(r.points[route.off + k + 1], r.points[route.off + k + 2])) {
          ++t.bends;
        }
      }
    }
  }
  t.crossings = cost_crossings(pieces, crossings_of);
  t.corridor = cost_corridor(r, pieces);

  // `min_len` is the direct distance between the endpoints, or the boxes the
  // route has to carry if those are longer; only the excess is charged, since
  // charging raw length makes the optimiser fight the constraint (11.9).
  for (uint32_t tr = 0; tr < c.transitions.size(); ++tr) {
    scav_span const route{ r.route[tr] };
    if (route.len < 2) { continue; }
    Wide actual{ 0 };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      actual += length_of(r.points[route.off + k], r.points[route.off + k + 1]);
    }
    Wide boxes{ 0 };
    for (uint32_t i = 0; i < s.n_path_box; ++i) {
      if (s.path_box[i].subject == tr) { boxes += s.path_box[i].w; }
    }
    Wide const direct{ length_of(r.points[route.off],
                                 r.points[route.off + route.len - 1]) };
    Wide const excess{ actual - imax(direct, boxes) };
    if (excess > 0) { t.excess_len += excess * (1 + crossings_of[tr]); }
  }

  // Placed boxes against each other, against the states, and against the routes
  // they do not belong to. A state enclosing an endpoint is the composite the
  // label lives inside, so only the text bands it reserved are out of bounds.
  //
  // **The walk starts above the endpoint, not at it.** An endpoint encloses
  // nothing, so the carve-out's own reason -- a label inside the composite its
  // transition runs in is where it belongs, and charging it there makes zero
  // unreachable -- does not reach it. Marking the endpoint exempted its whole
  // rect and left `label` blind to a label lying over the very box it names:
  // 3 of 203 on the corpus against 26 once the walk starts one level up.
  std::vector<uint8_t> encloses(c.states.size(), 0);
  auto const mark = [&](StateId of, uint8_t v) {
    StateId at{ enclosing_state(c, of) };
    for (size_t step = 0; (step < c.states.size()) && (at.v != INVALID); ++step) {
      encloses[at.v] = v;
      at = enclosing_state(c, at);
    }
  };
  for (uint32_t i = 0; i < r.placed.size(); ++i) {
    for (uint32_t j = i + 1; j < r.placed.size(); ++j) {
      if (overlaps(r.placed[i], r.placed[j])) { ++t.label; }
    }
    uint32_t subject{ INVALID };
    Wide height{ 0 };
    if ((s.path_box != nullptr) && (i < s.n_path_box) &&
        (s.path_box[i].subject < c.transitions.size())) {
      subject = s.path_box[i].subject;
      height = s.path_box[i].h;
      // Both ends, not either: a state enclosing one endpoint does not have to
      // hold the label -- the label belongs on the ancestral side of that
      // crossing -- so only a state enclosing *both* is exempt from its own
      // rect. `2` is the intersection; `1` is src's chain alone, which the
      // reset below clears along with it (11.9.3).
      mark(c.transitions[subject].src, 1);
      StateId up{ enclosing_state(c, c.transitions[subject].dst) };
      for (size_t step = 0; (step < c.states.size()) && (up.v != INVALID); ++step) {
        if (encloses[up.v] == 1) { encloses[up.v] = 2; }
        up = enclosing_state(c, up);
      }
    }
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if (c.states[st].live == 0) { continue; }
      if (encloses[st] == 2) {
        if (overlaps(r.placed[i], z.before[st]) || overlaps(r.placed[i], z.after[st])) {
          ++t.label;
        }
      } else if (overlaps(r.placed[i], z.state[st])) {
        ++t.label;
      }
    }
    // -1 until a leg of that kind is seen, so a routeless subject and a chart
    // with one transition both fall out of `label_near` rather than into it.
    Wide own{ -1 };
    Wide other{ -1 };
    for (Piece const &piece : pieces) {
      scav_rect const seg{ span_rect(piece.a, piece.b) };
      Wide const away{ chebyshev_gap(r.placed[i], seg) };
      if (piece.trans == subject) {
        own = (own < 0) ? away : imin(own, away);
        continue;
      }
      other = (other < 0) ? away : imin(other, away);
      if (overlaps(r.placed[i], seg)) { ++t.label; }
    }
    if ((own >= 0) && (other >= 0)) {
      Wide const shortfall{ (own + height) - other };
      if (shortfall > 0) { t.label_near += shortfall; }
    }
    if (subject != INVALID) { mark(c.transitions[subject].src, 0); }
  }

  // A direct arrow between two concurrent submachines wants them adjacent;
  // fork and join fan-outs are excluded, because adjacency above two is not
  // achievable and pricing an unsatisfiable constraint distorts the rest.
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
  Ancestry const an{ cost_flatten_ancestry(c) };
  ChildGrid const grid{ cost_child_grid(c, z) };
  t.box_overlap = cost_box_overlaps(c, z, grid);
  t.through_box = cost_through_boxes(c, z, an, grid, pieces);
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
    out.resize(column_count(c, id));
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
  rows("scav.geom.sub", z.sub);
  std::vector<scav_rect> chart;
  rows("scav.geom.chart", chart);
  if (!chart.empty()) { z.chart = chart[0]; }
  Routes r;
  rows("scav.geom.point", r.points);
  rows("scav.geom.route", r.route);
  r.route.resize(c.transitions.size());
  r.placed = placed;
  return cost_terms(c, g, z, r, s, p);
}

namespace {

// Each term in the unit the profile already names it in, then weighted: counts
// stay counts, lengths become ems of `font_size_grid`, area ems squared. The
// division is ceiling, so a term nonzero in grid units is nonzero here (11.6),
// and the em is floored at 1 because a profile the validator never saw could
// carry a zero the bound [1, SPACE_MAX] forbids.
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
           Wide{ p.w_area } * ceil_div(t.area, em2) };
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
  out.t0_violations = t.through_box + t.box_overlap;
  // Area is the largest term at (2 * COORD_MAX)^2 < 2^40, its em^2 only divides
  // it down, and nine of those under a weight capped at 2^10 stay below 2^54.
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
