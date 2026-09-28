// Strip matching over the finished routes: a leg's two sides are sliced into
// strips, the box slides along each, and the feasible candidate that reads as
// its own transition's, then nearest where centring would have put it, wins.

#include "layout/label.h"

#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/memo.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_stable_sort.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace scav {

// Bracketed with the search switchable, so a test can run all three over one
// input and require one answer; nothing shipping passes anything but
// `LabelSearch::Memoized`. Prototyped here because gcc's
// -Werror=missing-declarations refuses a namespace-scope definition with no
// declaration above it, which is what an internal function is under
// SCAV_TESTING. The prototype a test uses is its own; see scav_internal.h.
SCAV_INTERNAL_BEGIN
uint32_t place_labels_by(Chart const &c,
                         SizedLayout const &z,
                         scav_spaces const &s,
                         std::vector<scav_span> const &route,
                         std::vector<scav_point> const &points,
                         scav_profile const &p,
                         LabelSearch search,
                         std::vector<scav_rect> &out);
SCAV_INTERNAL_END

namespace {

// A submachine whose owner holds another live one beside it, so a divider runs
// between them.
bool shared_region(Chart const &c, SubmachineId m) {
  StateId const owner{ c.submachines[m.v].owner };
  if (owner.v == INVALID) { return false; }
  Span const subs{ c.states[owner.v].submachines };
  uint32_t live{ 0 };
  for (uint32_t k = 0; k < subs.len; ++k) {
    live += (c.submachines[c.submachine_ids[subs.off + k].v].live != 0) ? 1U : 0U;
  }
  return live > 1;
}

// `s` sits in `m` or in something nested inside it. The climb stops after one
// step per state, as `ancestor_or_self`'s does.
bool lies_in(Chart const &c, StateId s, SubmachineId m) {
  StateId at{ s };
  for (size_t up = 0; (up < c.states.size()) && (at.v < c.states.size()); ++up) {
    SubmachineId const parent{ c.states[at.v].parent };
    if (parent.v == m.v) { return true; }
    if (parent.v >= c.submachines.size()) { return false; }
    at = c.submachines[parent.v].owner;
  }
  return false;
}

// The eight points of the label's own rectangle the leader may attach to: its
// four corners, then the midpoints of its four sides. Corners because a
// polyline is kinked -- a label hung off a bend belongs on the diagonal, and an
// axis-aligned leader reaches a diagonal position only through a corner.
constexpr uint32_t ATTACH{ 8 };

// The four directions the leader runs in, so its length is exact in integers:
// a diagonal of the same length is not representable on a 1/16-pt grid.
constexpr uint32_t LEADS{ 4 };

// Where attachment point `which` sits inside a `w` by `h` rectangle.
//
// **Side midpoints before corners, and the bottom one first.** The key is
// lexicographic, so this order is what a tie resolves to, and the first
// combination it reaches -- the bottom midpoint with the leader running up --
// is the box centred above its leg, which is where a reader expects a label.
// Corners are what a bend needs and a straight leg does not, so they come
// last: with them first, every label took a diagonal offset it had no reason
// to take (11.9.4).
scav_point attach_at(uint32_t which, int32_t w, int32_t h) {
  switch (which) {
    case 0: return { .x = w / 2, .y = h };
    case 1: return { .x = w / 2, .y = 0 };
    case 2: return { .x = w, .y = h / 2 };
    case 3: return { .x = 0, .y = h / 2 };
    case 4: return { .x = 0, .y = 0 };
    case 5: return { .x = w, .y = 0 };
    case 6: return { .x = 0, .y = h };
    default: return { .x = w, .y = h };
  }
}

// The leader's own offset: `lead` 0..3 is up, down, left, right by `len`.
scav_point lead_by(uint32_t lead, int32_t len) {
  switch (lead) {
    case 0: return { .x = 0, .y = -len };
    case 1: return { .x = 0, .y = len };
    case 2: return { .x = -len, .y = 0 };
    default: return { .x = len, .y = 0 };
  }
}

// One route's legs, `first` onward in the list of every route's, and the box
// they all lie in: a region that misses the box overlaps none of them.
struct Pieces {
  scav_rect bounds;
  uint32_t first, count;
};

// A candidate's whole identity, so the winner is a lexicographic minimum over
// integers rather than an order of evaluation.
struct Key {
  Wide shortfall;
  Wide dist;
  uint32_t seg, attach, lead;
  int32_t mid;
};

bool better(Key const &a, Key const &b) {
  if (a.shortfall != b.shortfall) { return a.shortfall < b.shortfall; }
  if (a.dist != b.dist) { return a.dist < b.dist; }
  if (a.seg != b.seg) { return a.seg < b.seg; }
  if (a.attach != b.attach) { return a.attach < b.attach; }
  if (a.lead != b.lead) { return a.lead < b.lead; }
  return a.mid < b.mid;
}

constexpr Wide NO_LIMIT{ std::numeric_limits<Wide>::max() };

// How far inside `reach` the nearest foreign segment lies, `reach` being the
// candidate's gap to its own leg plus one line of its own text (11.6), and
// through `at` which segment that is, INVALID where none lies inside. The scan
// stops once the shortfall passes `stop`, since it only grows as the scan goes
// on; what it returns then is known only to be more than `stop`.
Wide shortfall_of(scav_rect const &cand,
                  Wide reach,
                  std::vector<scav_rect> const &foreign,
                  Wide stop,
                  uint32_t &at) {
  Wide nearest{ reach };
  at = INVALID;
  for (uint32_t i = 0; i < foreign.size(); ++i) {
    Wide const gap{ chebyshev_gap(cand, foreign[i]) };
    if (gap < nearest) {
      nearest = gap;
      at = i;
    }
    if ((nearest == 0) || ((reach - nearest) > stop)) { break; }
  }
  return reach - nearest;
}

// The midpoint of the longest horizontal leg, else of the longest leg: phase 1
// widened a rank boundary by this box (11.3) and the leg crossing it is long.
scav_point anchor_of(std::vector<scav_point> const &points, scav_span route) {
  scav_point mid{};
  Wide longest{ -1 };
  for (uint32_t pass = 0; (pass < 2) && (longest < 0); ++pass) {
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      scav_point const a{ points[route.off + k] };
      scav_point const b{ points[route.off + k + 1] };
      if ((pass == 0) && (a.y != b.y)) { continue; }
      Wide const span{ imax(Wide{ a.x } - b.x, Wide{ b.x } - a.x) +
                       imax(Wide{ a.y } - b.y, Wide{ b.y } - a.y) };
      if (span <= longest) { continue; }
      longest = span;
      mid = { .x = a.x + static_cast<int32_t>(floor_div(Wide{ b.x } - a.x, Wide{ 2 })),
              .y = a.y + static_cast<int32_t>(floor_div(Wide{ b.y } - a.y, Wide{ 2 })) };
    }
  }
  if (longest < 0) { return (route.len == 0) ? scav_point{} : points[route.off]; }
  return mid;
}

// Slid inside rather than hung off: the chart rect bounds everything laid out
// (11.7a), so a label half outside grows the canvas to hold whitespace.
scav_rect centred(scav_point mid, scav_path_box const &box, scav_rect const &chart) {
  int32_t x{ mid.x - floor_div(box.w, 2) };
  int32_t y{ mid.y - floor_div(box.h, 2) };
  if (box.w <= chart.w) { x = imin(imax(x, chart.x), (chart.x + chart.w) - box.w); }
  if (box.h <= chart.h) { y = imin(imax(y, chart.y), (chart.y + chart.h) - box.h); }
  return { .x = x, .y = y, .w = box.w, .h = box.h };
}

// Everything one box's search reads, so what it settles is a function of this
// and of nothing else, and the memo's key is this written out. For the pruned
// and memoized searches coordinates are relative to the route's first point
// and every rect is cut down to the region the candidates can reach, so a box
// whose surroundings moved together, or changed only out of its reach, is the
// box it was; the exhaustive search takes the chart's own.
struct Local {
  int32_t w{ 0 }, h{ 0 }, leader{ 0 };
  uint32_t chained{ 0 }, prior_seg{ 0 };
  int32_t prior_mid{ 0 };
  scav_rect holder{};
  std::vector<scav_point> route;
  std::vector<scav_rect> walls;    // states, bands and settled boxes
  std::vector<scav_rect> foreign;  // other transitions' legs
};

// What a search settles: the box, and the leg and slide a later box of its
// transition goes past. Nothing found leaves the box to the centred fallback.
struct Outcome {
  bool found{ false };
  scav_rect at{};
  uint32_t seg{ 0 };
  int32_t mid{ 0 };
};

// The candidates of one leg and one lead and attachment, which differ only in
// the slide: `centre` is the slide that would put the box's centre nearest the
// anchor, and `near` the least anchor distance any of them has.
struct Group {
  uint32_t k, offset, band;
  Wide centre, near;
};

// Per leg, the groups whose candidates share one line across it, and those of
// the leg's nearby segments that may lie within one of their reaches, in the
// leg's order. A segment left out is one no candidate of the band has nearer
// than its reach, so it is never the nearest and never ends a scan.
struct Band {
  bool built{ false };
  std::vector<scav_rect> near;
};

// A leg's bands: across it a lead is back, none or forward, and an attachment
// sits at 0, half the box or all of it.
constexpr uint32_t BANDS{ 9 };

// The band of offset `o`, from `lead_by` and `attach_at` read across the leg:
// two offsets with one band are one line across it.
uint32_t band_of_offset(uint32_t o, bool flat) {
  constexpr std::array<uint32_t, LEADS> LEAD_Y{ 0, 1, 2, 2 };
  constexpr std::array<uint32_t, LEADS> LEAD_X{ 0, 0, 1, 2 };
  constexpr std::array<uint32_t, ATTACH> IN_Y{ 0, 1, 2, 2, 1, 1, 0, 0 };
  constexpr std::array<uint32_t, ATTACH> IN_X{ 0, 0, 1, 2, 2, 1, 2, 1 };
  uint32_t const lead{ o / ATTACH };
  uint32_t const which{ o % ATTACH };
  return flat ? ((LEAD_Y[lead] * 3) + IN_Y[which]) : ((LEAD_X[lead] * 3) + IN_X[which]);
}

// A blocked rect's inclusive range of grid cells.
struct Cells {
  uint32_t c0, c1, r0, r1;
};

// One leg's groups, which start at `first`, and the least anchor distance any
// of them has.
struct Leg {
  uint32_t first;
  Wide near;
};

// A search's working vectors, kept across the boxes of one call. `nearby` is
// per leg, and `bands` holds `BANDS` a leg. The pruned search builds the grid over
// `blocked` only once a candidate first reaches it, and `gridded` says whether
// it has for this box.
struct Scratch {
  std::vector<scav_rect> own;
  std::vector<scav_rect> blocked;
  std::vector<std::vector<scav_rect>> nearby;
  std::vector<Group> groups;
  std::vector<Leg> legs;
  std::vector<Band> bands;
  RectGrid grid;
  std::vector<Cells> cells;
  std::vector<uint32_t> fill;
  scav_rect region{};
  bool gridded{ false };
};

// `grid_build` over the scratch's own vectors, so a box allocates nothing once
// the first few have grown them, and with each rect's cells found once for
// both the count and the fill. The cells, and so the grid, are `grid_build`'s.
RectGrid const &grid_of(Scratch &s, int32_t cell_w, int32_t cell_h) {
  RectGrid &g{ s.grid };
  if (s.gridded) { return g; }
  s.gridded = true;
  scav_rect const &region{ s.region };
  g.x0 = region.x;
  g.y0 = region.y;
  g.cw = imax(Wide{ imax(cell_w, 1) }, ceil_div(Wide{ region.w } + 1, Wide{ GRID_SIDE }));
  g.ch = imax(Wide{ imax(cell_h, 1) }, ceil_div(Wide{ region.h } + 1, Wide{ GRID_SIDE }));
  g.nx = static_cast<uint32_t>(imax(ceil_div(Wide{ region.w } + 1, g.cw), Wide{ 1 }));
  g.ny = static_cast<uint32_t>(imax(ceil_div(Wide{ region.h } + 1, g.ch), Wide{ 1 }));
  g.off.assign((static_cast<size_t>(g.nx) * g.ny) + 1, 0);
  s.cells.resize(s.blocked.size());
  for (uint32_t k = 0; k < s.blocked.size(); ++k) {
    scav_rect const &r{ s.blocked[k] };
    Cells const at{ .c0 = grid_cell(r.x, g.x0, g.cw, g.nx),
                    .c1 = grid_cell(Wide{ r.x } + r.w, g.x0, g.cw, g.nx),
                    .r0 = grid_cell(r.y, g.y0, g.ch, g.ny),
                    .r1 = grid_cell(Wide{ r.y } + r.h, g.y0, g.ch, g.ny) };
    s.cells[k] = at;
    for (uint32_t y = at.r0; y <= at.r1; ++y) {
      for (uint32_t x = at.c0; x <= at.c1; ++x) {
        ++g.off[(static_cast<size_t>(y) * g.nx) + x + 1];
      }
    }
  }
  for (size_t i = 1; i < g.off.size(); ++i) { g.off[i] += g.off[i - 1]; }
  g.item.resize(g.off.back());
  s.fill.assign(g.off.begin(), g.off.end() - 1);
  for (uint32_t k = 0; k < s.blocked.size(); ++k) {
    Cells const &at{ s.cells[k] };
    for (uint32_t y = at.r0; y <= at.r1; ++y) {
      for (uint32_t x = at.c0; x <= at.c1; ++x) {
        g.item[s.fill[(static_cast<size_t>(y) * g.nx) + x]++] = k;
      }
    }
  }
  return g;
}

// The coordinate a slide along leg `k` is measured in: x on a horizontal leg,
// y on any other.
int32_t along(std::vector<scav_point> const &points,
              scav_span r,
              uint32_t k,
              scav_point at) {
  bool const flat{ ((k + 1) < r.len) && (points[r.off + k].y == points[r.off + k + 1].y) };
  return flat ? at.x : at.y;
}

scav_rect relative_to(scav_rect const &r, scav_point origin) {
  return { .x = r.x - origin.x, .y = r.y - origin.y, .w = r.w, .h = r.h };
}

// Every candidate lies within the leader plus the box of the polyline, and the
// distance query reaches one box height past that.
scav_rect region_of(std::vector<scav_point> const &route,
                    int32_t w,
                    int32_t h,
                    int32_t leader) {
  int32_t x0{ route[0].x };
  int32_t x1{ x0 };
  int32_t y0{ route[0].y };
  int32_t y1{ y0 };
  for (scav_point const &pt : route) {
    x0 = imin(x0, pt.x);
    y0 = imin(y0, pt.y);
    x1 = imax(x1, pt.x);
    y1 = imax(y1, pt.y);
  }
  int32_t const reach_x{ w + leader + h };
  int32_t const reach_y{ h + leader + h };
  return { .x = x0 - reach_x,
           .y = y0 - reach_y,
           .w = (x1 - x0) + (2 * reach_x),
           .h = (y1 - y0) + (2 * reach_y) };
}

// The route's legs, and the walls and foreign legs in one list, which is what
// both searches start from.
void prepare(Local const &l, Scratch &s) {
  uint32_t const n{ static_cast<uint32_t>(l.route.size()) };
  s.own.assign(n - 1, {});
  for (uint32_t k = 0; (k + 1) < n; ++k) {
    s.own[k] = span_rect(l.route[k], l.route[k + 1]);
  }
  if (s.nearby.size() < s.own.size()) { s.nearby.resize(s.own.size()); }
  s.blocked.assign(l.walls.begin(), l.walls.end());
  s.blocked.insert(s.blocked.end(), l.foreign.begin(), l.foreign.end());
  s.region = region_of(l.route, l.w, l.h, l.leader);
  s.gridded = false;
}

// The foreign legs within the leader plus the box of leg `k`, which is as far
// as any of its candidates reaches, so the distance prefilter runs once per leg.
std::vector<scav_rect> const &nearby_of(Local const &l, Scratch &s, uint32_t k) {
  std::vector<scav_rect> &out{ s.nearby[k] };
  out.clear();
  for (scav_rect const &seg : l.foreign) {
    if (chebyshev_gap(s.own[k], seg) <= (l.leader + l.w + l.h)) { out.push_back(seg); }
  }
  return out;
}

// Every candidate of every leg keyed and tested in turn, the strict tier and
// the fallback's together: the placement as 11.9.4 states it, which the pruned
// search reproduces.
Outcome exhaustive(Local const &l, Scratch &s) {
  uint32_t const len{ static_cast<uint32_t>(l.route.size()) };
  prepare(l, s);
  grid_build(s.grid, s.region, s.blocked, l.w, l.h);
  scav_point const at{ anchor_of(l.route, { .off = 0, .len = len }) };
  // Half the label's height, so the anchor slides finer than the box it
  // carries; the floor is one grid unit, which is 1/16 pt (11.9.4).
  int32_t const step{ imax(l.h / 2, 1) };
  bool const chained{ l.chained != 0 };
  scav_rect best{};
  Key key{ .shortfall = 0, .dist = -1, .seg = 0, .attach = 0, .lead = 0, .mid = 0 };
  // **The fallback keeps the anchor and accepts a collision** (11.9.4). What
  // it used to keep is the centred placement, which sits *on* the leg -- so
  // every box with no clear candidate was a sliced label by construction, and
  // the slice count and the fallback count were one number. An anchored box
  // is a leader away from its own line and cannot be cut by it; a box over a
  // state still reads as its transition's, and a detached one does not.
  scav_rect lax{};
  Key lax_key{ .shortfall = 0, .dist = -1, .seg = 0, .attach = 0, .lead = 0, .mid = 0 };

  for (uint32_t k = chained ? l.prior_seg : 0U; (k + 1) < len; ++k) {
    scav_point const a{ l.route[k] };
    scav_point const b{ l.route[k + 1] };
    bool const flat{ a.y == b.y };
    // Equal means diagonal or degenerate: neither has a strip beside it.
    if (flat == (a.x == b.x)) { continue; }
    int32_t const lo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
    int32_t const hi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
    // The sign of the leg's traversal: `dir` times a difference of two of
    // its coordinates is how much further along the route the first lies.
    bool const ascending{ flat ? (a.x < b.x) : (a.y < b.y) };
    int32_t const dir{ ascending ? 1 : -1 };
    bool const bounded{ chained && (k == l.prior_seg) };
    std::vector<scav_rect> const &nearby{ nearby_of(l, s, k) };
    // Both ends of the leg are anchors whatever the step divides into, so
    // the last slot is clamped rather than skipped.
    int32_t const runs{ (hi - lo) / step };
    for (int32_t n = 0; n <= (runs + 1); ++n) {
      int32_t const mid{ imin(lo + (n * step), hi) };
      // Past the box before it is further along the route, which on a leg
      // running backwards is the smaller coordinate.
      if (bounded && (((mid - l.prior_mid) * dir) <= 0)) { continue; }
      scav_point const on{ flat ? scav_point{ .x = mid, .y = a.y }
                                : scav_point{ .x = a.x, .y = mid } };
      for (uint32_t lead = 0; lead < LEADS; ++lead) {
        scav_point const away{ lead_by(lead, l.leader) };
        for (uint32_t which = 0; which < ATTACH; ++which) {
          // The leader ends on the attachment point, so the box's origin is
          // that point less where the point sits inside the box.
          scav_point const in{ attach_at(which, l.w, l.h) };
          scav_rect const cand{ .x = (on.x + away.x) - in.x,
                                .y = (on.y + away.y) - in.y,
                                .w = l.w,
                                .h = l.h };
          Wide const dx{ (Wide{ cand.x } + floor_div(l.w, 2)) - at.x };
          Wide const dy{ (Wide{ cand.y } + floor_div(l.h, 2)) - at.y };
          Key here{ .shortfall = 0,
                    .dist = imax(dx, -dx) + imax(dy, -dy),
                    .seg = k,
                    .attach = which,
                    .lead = lead,
                    .mid = mid };
          // Keyed before tested, and zero is the floor of any shortfall: one
          // test with it standing in, then the real one, then the sweep.
          // Against the *strict* tier, which is the weaker bound: the loose
          // one holds the best of a superset, so pruning by it would drop
          // candidates the strict tier still wants.
          if ((key.dist >= 0) && !better(here, key)) { continue; }
          if (!nearby.empty()) {
            uint32_t nearest{ INVALID };
            here.shortfall = shortfall_of(cand,
                                          chebyshev_gap(cand, s.own[k]) + Wide{ l.h },
                                          nearby,
                                          NO_LIMIT,
                                          nearest);
            if ((key.dist >= 0) && !better(here, key)) { continue; }
          }
          if (!contains(l.holder, cand)) { continue; }
          // Every leg of its own route, this one included: an axis-aligned
          // leader off a corner can still put the label across the line it
          // is anchored to, which is the slice the anchor exists to stop
          // and is not structural (11.9.4). This one holds in both tiers --
          // a fallback that accepts a collision still may not be cut.
          bool uncut{ true };
          for (uint32_t j = 0; uncut && (j < s.own.size()); ++j) {
            if (overlaps(cand, s.own[j])) { uncut = false; }
          }
          if (!uncut) { continue; }
          if ((lax_key.dist < 0) || better(here, lax_key)) {
            lax_key = here;
            lax = cand;
          }
          if (grid_hits(s.grid, s.blocked, cand)) { continue; }
          key = here;
          best = cand;
        }
      }
    }
  }

  if ((key.dist < 0) && (lax_key.dist >= 0)) {
    key = lax_key;
    best = lax;
  }
  return { .found = key.dist >= 0, .at = best, .seg = key.seg, .mid = key.mid };
}

// One tier of the pruned search over one box: what the box fixes, and the
// incumbent. `last` is the wall that refused a candidate most recently, which
// the next is tried against before the grid.
struct Walk {
  Local const &l;
  Scratch &s;
  std::array<scav_point, size_t{ LEADS } * ATTACH> const &offset;
  scav_point at;
  int32_t step;
  bool strict;
  Key key{ .shortfall = 0, .dist = -1, .seg = 0, .attach = 0, .lead = 0, .mid = 0 };
  Outcome out{};
  uint32_t last{ INVALID };

  // Whether nothing at least `dist` from the anchor can beat the incumbent.
  [[nodiscard]] bool beyond(Wide dist) const {
    return (key.dist >= 0) && (key.shortfall == 0) && (dist > key.dist);
  }

  Band const &band_of(Group const &g);
  void group(Group const &g);
};

// Every candidate of the band lies in one rect: across the leg the band's
// line, and along it from the least start to the greatest end any lead and
// attachment put a candidate at. A candidate's gap to its own leg is at most
// the leader, since its attachment point is a leader from a point on the leg,
// so its reach is at most the leader plus its height, and a segment at least
// that far from the rect is at least that far from every candidate.
Band const &Walk::band_of(Group const &g) {
  Band &b{ s.bands[g.band] };
  if (b.built) { return b; }
  b.built = true;
  b.near.clear();
  uint32_t const k{ g.k };
  scav_point const a{ l.route[k] };
  scav_point const e{ l.route[k + 1] };
  bool const flat{ a.y == e.y };
  scav_point const d{ offset[g.offset] };
  // Along the leg an offset is a lead of 0 or the leader either way, less an
  // attachment at 0, half the box or all of it.
  int32_t const size{ flat ? l.w : l.h };
  Wide const lead_most{ imax(Wide{ l.leader }, -Wide{ l.leader }) };
  Wide const in_least{ imin(imin(0, size / 2), size) };
  Wide const in_most{ imax(imax(0, size / 2), size) };
  Wide const across_lo{ flat ? (Wide{ a.y } + d.y) : (Wide{ a.x } + d.x) };
  Wide const across_hi{ across_lo + (flat ? l.h : l.w) };
  Wide const along_lo{ (Wide{ flat ? imin(a.x, e.x) : imin(a.y, e.y) } - lead_most) -
                       in_most };
  Wide const along_hi{
    ((Wide{ flat ? imax(a.x, e.x) : imax(a.y, e.y) } + lead_most) - in_least) + size
  };
  Wide const reach{ lead_most + l.h };
  for (scav_rect const &seg : s.nearby[k]) {
    Wide const ac_lo{ flat ? seg.y : seg.x };
    Wide const ac_hi{ ac_lo + (flat ? seg.h : seg.w) };
    Wide const al_lo{ flat ? seg.x : seg.y };
    Wide const al_hi{ al_lo + (flat ? seg.w : seg.h) };
    Wide const gap{ imax(imax(imax(ac_lo - across_hi, across_lo - ac_hi),
                              imax(al_lo - along_hi, along_lo - al_hi)),
                         Wide{ 0 }) };
    if (gap < reach) { b.near.push_back(seg); }
  }
  return b;
}

// A group's candidates differ only in the slide, so each test is an interval
// of slides: across the leg a candidate never moves, and along it one that
// overlaps a rect keeps overlapping it until it has slid past the rect's far
// side. A refused slide is left by jumping to the first one past what refused
// it, and the walk ends where the rest of its side cannot win.
void Walk::group(Group const &g) {
  uint32_t const k{ g.k };
  scav_point const a{ l.route[k] };
  scav_point const b{ l.route[k + 1] };
  bool const flat{ a.y == b.y };
  int32_t const lo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
  int32_t const hi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
  bool const ascending{ flat ? (a.x < b.x) : (a.y < b.y) };
  int32_t const dir{ ascending ? 1 : -1 };
  bool const bounded{ (l.chained != 0) && (k == l.prior_seg) };
  scav_point const d{ offset[g.offset] };
  uint32_t const lead{ g.offset / ATTACH };
  uint32_t const which{ g.offset % ATTACH };
  int32_t const runs{ (hi - lo) / step };
  Wide const half_w{ floor_div(l.w, 2) };
  Wide const half_h{ floor_div(l.h, 2) };

  // Along the leg a candidate starts `shift` past its slide and spans `size`.
  Wide const shift{ flat ? d.x : d.y };
  Wide const size{ flat ? l.w : l.h };
  // A candidate's gap to its own leg is at least its gap across it, so its
  // reach is at least `least_reach` wherever it slides.
  Wide const lead_across{ flat ? d.y : d.x };
  Wide const least_reach{
    imax(Wide{ 0 }, imax(lead_across, -(lead_across + (flat ? l.h : l.w)))) + l.h
  };
  // `contains` split by axis: across the leg it holds for every slide or for
  // none, and along it for the slides from `hold_lo` to `hold_hi`.
  scav_rect const &hold{ l.holder };
  Wide const across{ flat ? (Wide{ a.y } + d.y) : (Wide{ a.x } + d.x) };
  Wide const across_lo{ flat ? hold.y : hold.x };
  Wide const across_hi{ flat ? (Wide{ hold.y } + hold.h) : (Wide{ hold.x } + hold.w) };
  if ((across < across_lo) || ((across + (flat ? l.h : l.w)) > across_hi)) { return; }
  Wide const hold_lo{ (flat ? Wide{ hold.x } : Wide{ hold.y }) - shift };
  Wide const hold_hi{
    ((flat ? (Wide{ hold.x } + hold.w) : (Wide{ hold.y } + hold.h)) - shift) - size
  };

  // Slide numbers kept to what the walk can hold: one past either end stops it.
  auto const clamp = [runs](Wide n) {
    return static_cast<int32_t>(imin(imax(n, Wide{ -1 }), Wide{ runs } + 2));
  };
  // The first slide past `n` in direction `sign` whose candidate is clear of
  // `r` along the leg, `r` being a rect the candidate at `n` overlaps.
  auto const past = [&](scav_rect const &r, int32_t n, int32_t sign) {
    Wide const r_lo{ flat ? r.x : r.y };
    Wide const r_hi{ r_lo + (flat ? r.w : r.h) };
    if (sign > 0) {
      return imax(n + 1, clamp(ceil_div((r_hi - shift) - lo, Wide{ step })));
    }
    return imin(n - 1, clamp(floor_div(((r_lo - shift) - size) - lo, Wide{ step })));
  };
  // Tests the candidate at slide `n` and returns the next slide to test.
  auto const visit = [&](int32_t n, int32_t sign) {
    int32_t const stop{ (sign > 0) ? (runs + 2) : -1 };
    int32_t const mid{ imin(lo + (n * step), hi) };
    scav_point const on{ flat ? scav_point{ .x = mid, .y = a.y }
                              : scav_point{ .x = a.x, .y = mid } };
    scav_rect const cand{ .x = on.x + d.x, .y = on.y + d.y, .w = l.w, .h = l.h };
    Wide const dx{ (Wide{ cand.x } + half_w) - at.x };
    Wide const dy{ (Wide{ cand.y } + half_h) - at.y };
    Key here{ .shortfall = 0,
              .dist = imax(dx, -dx) + imax(dy, -dy),
              .seg = k,
              .attach = which,
              .lead = lead,
              .mid = mid };
    if (beyond(here.dist)) { return stop; }
    if (bounded && (((mid - l.prior_mid) * dir) <= 0)) { return n + sign; }
    if ((key.dist >= 0) && !better(here, key)) { return n + sign; }
    if (mid < hold_lo) {
      return (sign > 0) ? imax(n + 1, clamp(ceil_div(hold_lo - lo, Wide{ step }))) : stop;
    }
    if (mid > hold_hi) {
      return (sign > 0) ? stop : imin(n - 1, clamp(floor_div(hold_hi - lo, Wide{ step })));
    }
    for (scav_rect const &leg : s.own) {
      if (overlaps(cand, leg)) { return past(leg, n, sign); }
    }
    if (strict) {
      if ((last != INVALID) && overlaps(cand, s.blocked[last])) {
        return past(s.blocked[last], n, sign);
      }
      uint32_t const hit{ grid_hit(grid_of(s, l.w, l.h), s.blocked, cand) };
      if (hit != INVALID) {
        last = hit;
        return past(s.blocked[hit], n, sign);
      }
    }
    std::vector<scav_rect> const &nearby{ band_of(g).near };
    if (!nearby.empty()) {
      uint32_t nearest{ INVALID };
      here.shortfall = shortfall_of(cand,
                                    chebyshev_gap(cand, s.own[k]) + Wide{ l.h },
                                    nearby,
                                    (key.dist >= 0) ? key.shortfall : NO_LIMIT,
                                    nearest);
      if ((key.dist >= 0) && !better(here, key)) {
        // A segment nearer than the least reach less the incumbent's shortfall
        // puts every slide that near it past the incumbent too, as a wall does
        // once it is grown by that much.
        Wide const within{ least_reach - key.shortfall };
        if ((here.shortfall > key.shortfall) && (nearest != INVALID) && (within > 0) &&
            (chebyshev_gap(cand, nearby[nearest]) < within)) {
          return past(grow(nearby[nearest], static_cast<int32_t>(within)), n, sign);
        }
        return n + sign;
      }
    }
    key = here;
    out = { .found = true, .at = cand, .seg = k, .mid = mid };
    return n + sign;
  };

  // The slide at or below `centre`, clamped onto the leg: the slides above it
  // lie further from `centre` as they go up, and it and those below it as they
  // go down.
  int32_t const first{ static_cast<int32_t>(
      imin(imax(floor_div(g.centre - lo, Wide{ step }), Wide{ 0 }), Wide{ runs } + 1)) };
  // A box straddling its own leg's line is cut by the leg wherever the two
  // overlap along it, and one of positive extent clears it only past the end
  // its lead and attachment put it beyond, if either: from `from` up, or up to
  // `to`.
  int32_t from{ 0 };
  int32_t to{ runs + 1 };
  if ((size > 0) && (lead_across < 0) && ((lead_across + (flat ? l.h : l.w)) > 0)) {
    Wide const left{ (Wide{ lo } - shift) - size };
    if (shift >= 0) {
      from = imax(0, clamp(ceil_div((Wide{ hi } - shift) - lo, Wide{ step })));
    } else if (left >= lo) {
      to = (hi <= left) ? (runs + 1)
                        : imin(clamp(floor_div(left - lo, Wide{ step })), runs);
    } else {
      return;
    }
  }
  for (int32_t n = imax(first + 1, from); n <= to;) { n = visit(n, 1); }
  for (int32_t n = imin(first, to); n >= from;) { n = visit(n, -1); }
}

// The exhaustive search's answer with most of its candidates never keyed. Its
// winner is the least key over a set, so the order candidates are met in is
// free, and so is skipping any that cannot win:
//
// - The tiers run one after the other, the fallback's only when the strict one
//   finds nothing, since its answer is used only then.
// - A candidate is tested cheapest first, and its shortfall is measured last
//   and only as far as can still beat the incumbent's.
// - The candidates are met in groups, legs nearest the anchor first, and each
//   group is walked outward from the slide nearest `centre`, along which the
//   anchor distance only grows. Once the incumbent's shortfall is zero, a
//   candidate further from the anchor than it cannot win, so neither can the
//   rest of its walk, nor a group or a leg whose nearest is further.
Outcome pruned(Local const &l, Scratch &s) {
  uint32_t const len{ static_cast<uint32_t>(l.route.size()) };
  prepare(l, s);
  scav_point const at{ anchor_of(l.route, { .off = 0, .len = len }) };
  Wide const half_w{ floor_div(l.w, 2) };
  Wide const half_h{ floor_div(l.h, 2) };

  // Each lead and attachment as one offset from the point on the leg to the
  // box's origin.
  std::array<scav_point, size_t{ LEADS } * ATTACH> offset{};
  for (uint32_t lead = 0; lead < LEADS; ++lead) {
    scav_point const away{ lead_by(lead, l.leader) };
    for (uint32_t which = 0; which < ATTACH; ++which) {
      scav_point const in{ attach_at(which, l.w, l.h) };
      offset[(lead * ATTACH) + which] = { .x = away.x - in.x, .y = away.y - in.y };
    }
  }

  // Along the leg the distance is the slide's from `centre`; across it, it is
  // fixed.
  s.groups.clear();
  s.legs.clear();
  for (uint32_t k = (l.chained != 0) ? l.prior_seg : 0U; (k + 1) < len; ++k) {
    scav_point const a{ l.route[k] };
    scav_point const b{ l.route[k + 1] };
    bool const flat{ a.y == b.y };
    if (flat == (a.x == b.x)) { continue; }
    nearby_of(l, s, k);
    Wide const lo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
    Wide const hi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
    Leg leg{ .first = static_cast<uint32_t>(s.groups.size()), .near = NO_LIMIT };
    uint32_t const bands{ static_cast<uint32_t>(s.legs.size()) * BANDS };
    if (s.bands.size() < (bands + BANDS)) { s.bands.resize(bands + BANDS); }
    for (uint32_t j = 0; j < BANDS; ++j) { s.bands[bands + j].built = false; }
    for (uint32_t o = 0; o < (LEADS * ATTACH); ++o) {
      scav_point const d{ offset[o] };
      Wide const across{ flat ? ((Wide{ a.y } + d.y + half_h) - at.y)
                              : ((Wide{ a.x } + d.x + half_w) - at.x) };
      Wide const centre{ flat ? ((Wide{ at.x } - d.x) - half_w)
                              : ((Wide{ at.y } - d.y) - half_h) };
      Wide const gap{ imin(imax(centre, lo), hi) - centre };
      Wide const near{ imax(across, -across) + imax(gap, -gap) };
      s.groups.push_back({ .k = k,
                           .offset = o,
                           .band = bands + band_of_offset(o, flat),
                           .centre = centre,
                           .near = near });
      leg.near = imin(leg.near, near);
    }
    s.legs.push_back(leg);
  }
  scav_insertion_sort(s.legs.data(),
                      s.legs.data() + s.legs.size(),
                      [](Leg const &a, Leg const &b) { return a.near < b.near; });

  auto const tier = [&](bool strict) {
    Walk w{ .l = l,
            .s = s,
            .offset = offset,
            .at = at,
            .step = imax(l.h / 2, 1),
            .strict = strict };
    for (Leg const &leg : s.legs) {
      if (w.beyond(leg.near)) { break; }
      for (uint32_t o = 0; o < (LEADS * ATTACH); ++o) {
        Group const &g{ s.groups[leg.first + o] };
        if (!w.beyond(g.near)) { w.group(g); }
      }
    }
    return w.out;
  };
  Outcome const strict{ tier(true) };
  return strict.found ? strict : tier(false);
}

// `memo_hash`'s round taken two words at a time: a box's key runs to a hundred
// words and more, and every box a search places hashes one.
uint64_t label_hash(std::vector<uint32_t> const &key) {
  uint64_t h{ 0 };
  size_t k{ 0 };
  for (; (k + 1) < key.size(); k += 2) {
    h = (h ^ (key[k] | (static_cast<uint64_t>(key[k + 1]) << 32U))) *
        UINT64_C(0x9E37'79B9'7F4A'7C15);
    h ^= h >> 29U;
  }
  if (k < key.size()) {
    h = (h ^ key[k]) * UINT64_C(0x9E37'79B9'7F4A'7C15);
    h ^= h >> 29U;
  }
  return h;
}

// Every box this thread has placed, by its whole problem. A search's
// candidates differ in one frame, so nearly every box one of them places
// another placed first, relative to its route -- on bottler, 0.93M distinct
// of 8.1M. Per thread, so no lookup waits on another, and bounded, since a
// pool thread and what it keeps here last as long as the process.
Memo &memo() {
  thread_local Memo m{ size_t{ 1 } << 20, label_hash };
  return m;
}

// Every field of `l`, counts before contents, so two problems with one key
// are one problem.
void key_of(Local const &l, std::vector<uint32_t> &key) {
  key.resize(13 + (2 * l.route.size()) + (4 * l.walls.size()) + (4 * l.foreign.size()));
  uint32_t *at{ key.data() };
  auto const word = [&at](int32_t v) { *at++ = static_cast<uint32_t>(v); };
  auto const rect = [&word](scav_rect const &r) {
    word(r.x);
    word(r.y);
    word(r.w);
    word(r.h);
  };
  word(l.w);
  word(l.h);
  word(l.leader);
  *at++ = l.chained;
  *at++ = l.prior_seg;
  word(l.prior_mid);
  rect(l.holder);
  *at++ = static_cast<uint32_t>(l.route.size());
  for (scav_point const &pt : l.route) {
    word(pt.x);
    word(pt.y);
  }
  *at++ = static_cast<uint32_t>(l.walls.size());
  for (scav_rect const &r : l.walls) { rect(r); }
  *at++ = static_cast<uint32_t>(l.foreign.size());
  for (scav_rect const &r : l.foreign) { rect(r); }
}

Outcome remembered(Local const &l, Scratch &s) {
  thread_local std::vector<uint32_t> key;
  thread_local std::vector<int32_t> value;
  key_of(l, key);
  Memo &m{ memo() };
  int32_t const *hit{ nullptr };
  uint32_t len{ 0 };
  if (m.find(key, hit, len)) {
    return { .found = hit[0] != 0,
             .at = { .x = hit[1], .y = hit[2], .w = l.w, .h = l.h },
             .seg = static_cast<uint32_t>(hit[3]),
             .mid = hit[4] };
  }
  Outcome const out{ pruned(l, s) };
  value.assign(
      { out.found ? 1 : 0, out.at.x, out.at.y, static_cast<int32_t>(out.seg), out.mid });
  m.insert(key, value);
  return out;
}

uint32_t place_labels_from(Chart const &c,
                           SizedLayout const &z,
                           scav_spaces const &s,
                           std::vector<scav_span> const &route,
                           std::vector<scav_point> const &points,
                           scav_profile const &p,
                           LabelSearch search,
                           std::vector<scav_rect> &out,
                           std::vector<LabelSettle> &how,
                           LabelBase const *was) {
  out.assign(s.n_path_box, {});
  how.assign(s.n_path_box, {});
  if ((s.path_box == nullptr) || (s.n_path_box == 0)) { return 0; }

  // Against a base: which routes moved, and the rects a box must not reach to
  // be the problem it was -- the legs of a moved route as they were and as
  // they are, then each box that settled elsewhere, where it was and where it
  // is. A problem reads a rect only where it overlaps the box's region.
  bool const based{ (was != nullptr) && (was->route != nullptr) &&
                    (was->points != nullptr) && (was->placed != nullptr) &&
                    (was->settled != nullptr) && (was->route->size() == route.size()) &&
                    (was->placed->size() == s.n_path_box) &&
                    (was->settled->size() == s.n_path_box) };
  std::vector<uint8_t> moved;
  std::vector<scav_rect> dirty;
  if (based) {
    moved.assign(route.size(), 0);
    for (uint32_t t = 0; t < route.size(); ++t) {
      scav_span const now{ route[t] };
      scav_span const had{ (*was->route)[t] };
      bool kept{ now.len == had.len };
      for (uint32_t k = 0; kept && (k < now.len); ++k) {
        kept = same(points[now.off + k], (*was->points)[had.off + k]);
      }
      if (kept) { continue; }
      moved[t] = 1;
      for (uint32_t k = 0; (k + 1) < had.len; ++k) {
        dirty.push_back(
            span_rect((*was->points)[had.off + k], (*was->points)[had.off + k + 1]));
      }
      for (uint32_t k = 0; (k + 1) < now.len; ++k) {
        dirty.push_back(span_rect(points[now.off + k], points[now.off + k + 1]));
      }
    }
  }

  std::vector<scav_rect> pieces;
  std::vector<Pieces> by_route(route.size(), Pieces{});
  for (uint32_t t = 0; t < route.size(); ++t) {
    Pieces &of{ by_route[t] };
    of.first = static_cast<uint32_t>(pieces.size());
    for (uint32_t k = 0; (k + 1) < route[t].len; ++k) {
      scav_rect const at{ span_rect(points[route[t].off + k],
                                    points[route[t].off + k + 1]) };
      pieces.push_back(at);
      if (k == 0) {
        of.bounds = at;
        continue;
      }
      int32_t const x0{ imin(of.bounds.x, at.x) };
      int32_t const y0{ imin(of.bounds.y, at.y) };
      int32_t const x1{ imax(of.bounds.x + of.bounds.w, at.x + at.w) };
      int32_t const y1{ imax(of.bounds.y + of.bounds.h, at.y + at.h) };
      of.bounds = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
    }
    of.count = static_cast<uint32_t>(pieces.size()) - of.first;
  }
  // The live states, so a box's sweep reads nothing of a tombstone.
  std::vector<uint32_t> live;
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if (c.states[st].live != 0) { live.push_back(st); }
  }

  // Transitions ascending, then `order`, which is what makes a later box of one
  // transition see the earlier one already placed.
  std::vector<uint32_t> queue(s.n_path_box);
  for (uint32_t i = 0; i < s.n_path_box; ++i) { queue[i] = i; }
  scav_stable_sort(queue, [&s](uint32_t a, uint32_t b) {
    if (s.path_box[a].subject != s.path_box[b].subject) {
      return s.path_box[a].subject < s.path_box[b].subject;
    }
    return s.path_box[a].order < s.path_box[b].order;
  });

  // The walk starts above the endpoint, not at it: a state *enclosing* an
  // endpoint is the composite the label lives inside, and charging it there
  // would make zero unreachable, but an endpoint encloses nothing. Marking the
  // endpoint let a candidate lying over the very box its transition names read
  // as feasible, so the placer chose one (11.9.3).
  std::vector<uint8_t> encloses(c.states.size(), 0);
  auto const mark = [&](StateId of, uint8_t v) {
    StateId at{ enclosing_state(c, of) };
    for (size_t step = 0; (step < c.states.size()) && (at.v != INVALID); ++step) {
      encloses[at.v] = v;
      at = enclosing_state(c, at);
    }
  };

  Local l;
  // Per thread, as the memo is, so a call starts from the capacity the last
  // one grew; every search resets what it reads before reading it.
  thread_local Scratch scratch;
  std::vector<uint32_t> settled;
  uint32_t fallbacks{ 0 };
  uint32_t prior_subject{ INVALID };
  uint32_t prior_seg{ 0 };
  int32_t prior_mid{ 0 };
  bool chained{ false };
  // Every earlier box of the current transition settled as it did in the
  // base, so this one is chained exactly as it was.
  bool chain_kept{ true };
  int32_t const leader{ label_leader(p) };
  // Relative coordinates and a holder cut down to the candidates' region are
  // what let one problem recur; the exhaustive search takes the chart's own.
  bool const relative{ search != LabelSearch::Exhaustive };

  for (uint32_t const i : queue) {
    scav_path_box const &box{ s.path_box[i] };
    scav_span const r{ (box.subject < route.size()) ? route[box.subject] : scav_span{} };
    if (box.subject != prior_subject) {
      prior_subject = box.subject;
      chained = false;
      chain_kept = true;
    }
    Outcome got{};
    bool kept{ false };

    if (r.len >= 2) {
      scav_point const origin{ relative ? points[r.off] : scav_point{} };
      l.w = box.w;
      l.h = box.h;
      l.leader = leader;
      l.chained = chained ? 1U : 0U;
      l.prior_seg = chained ? prior_seg : 0U;
      l.prior_mid = chained ? (prior_mid - along(points, r, prior_seg, origin)) : 0;
      l.route.clear();
      for (uint32_t k = 0; k < r.len; ++k) {
        l.route.push_back(
            { .x = points[r.off + k].x - origin.x, .y = points[r.off + k].y - origin.y });
      }
      scav_rect const local_region{ region_of(l.route, box.w, box.h, leader) };
      scav_rect const region{ .x = local_region.x + origin.x,
                              .y = local_region.y + origin.y,
                              .w = local_region.w,
                              .h = local_region.h };
      kept =
          based && chain_kept && (box.subject < moved.size()) && (moved[box.subject] == 0);
      for (uint32_t k = 0; kept && (k < dirty.size()); ++k) {
        kept = !overlaps(region, dirty[k]);
      }
      if (kept) {
        LabelSettle const &had{ (*was->settled)[i] };
        got = { .found = had.found != 0,
                .at = (*was->placed)[i],
                .seg = had.seg,
                .mid = had.mid };
      } else {
        if (box.subject < c.transitions.size()) {
          // Both ends, not either: a state enclosing one endpoint does not have
          // to hold the label -- the label belongs on the ancestral side of that
          // crossing -- so only a state enclosing *both* is exempt from its own
          // rect. `2` is the intersection; `1` is src's chain alone, which the
          // reset below clears along with it (11.9.3).
          mark(c.transitions[box.subject].src, 1);
          StateId above{ enclosing_state(c, c.transitions[box.subject].dst) };
          for (size_t up = 0; (up < c.states.size()) && (above.v != INVALID); ++up) {
            if (encloses[above.v] == 1) { encloses[above.v] = 2; }
            above = enclosing_state(c, above);
          }
        }
        // I3 as a test: a label inside a composite is bounded by it, not by the
        // chart. Enclosing states nest, so intersecting them all is the innermost
        // without having to order them by depth (11.9.3).
        scav_rect holder{ z.chart };
        for (uint32_t const st : live) {
          if (encloses[st] == 2) { holder = intersection(holder, z.state[st]); }
        }
        // And by the region both endpoints lie in, where that region shares its
        // state with another: the divider between them runs through a label
        // that strays across it, and `brew`'s `at temperature` did (11.10g).
        if (box.subject < c.transitions.size()) {
          Transition const &tr{ c.transitions[box.subject] };
          SubmachineId const m{ c.states[tr.src.v].parent };
          if ((m.v < c.submachines.size()) && shared_region(c, m) &&
              lies_in(c, tr.dst, m)) {
            holder = intersection(holder, z.sub[m.v]);
          }
        }
        // A box of positive extent lies strictly inside the region wherever it
        // goes, so what lies outside it is never overlapped, never nearest, and
        // never what a candidate is held in: cut down to the region, a wall, a
        // leg or a holder that reaches further is the one it was.
        bool const clip{ relative && (box.w > 0) && (box.h > 0) };
        auto const local_rect = [&](scav_rect const &at) {
          scav_rect const shifted{ relative_to(at, origin) };
          return clip ? intersection(shifted, local_region) : shifted;
        };
        l.holder = relative_to(holder, origin);
        if (clip) {
          // Unclamped, so a holder the region misses refuses every candidate, as
          // it would uncut.
          int32_t const x0{ imax(l.holder.x, local_region.x) };
          int32_t const y0{ imax(l.holder.y, local_region.y) };
          int32_t const x1{ imin(l.holder.x + l.holder.w,
                                 local_region.x + local_region.w) };
          int32_t const y1{ imin(l.holder.y + l.holder.h,
                                 local_region.y + local_region.h) };
          l.holder = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
        }

        l.walls.clear();
        for (uint32_t const st : live) {
          // A state enclosing both endpoints holds the label legitimately; the
          // bands it reserved for its own text do not.
          if (encloses[st] == 2) {
            if (overlaps(region, z.before[st])) {
              l.walls.push_back(local_rect(z.before[st]));
            }
            if (overlaps(region, z.after[st])) {
              l.walls.push_back(local_rect(z.after[st]));
            }
          } else if (overlaps(region, z.state[st])) {
            l.walls.push_back(local_rect(z.state[st]));
          }
        }
        for (uint32_t const j : settled) {
          if (overlaps(region, out[j])) { l.walls.push_back(local_rect(out[j])); }
        }
        l.foreign.clear();
        for (uint32_t t = 0; t < by_route.size(); ++t) {
          Pieces const &of{ by_route[t] };
          if ((t == box.subject) || (of.count == 0) || !overlaps(region, of.bounds)) {
            continue;
          }
          for (uint32_t k = of.first; k < (of.first + of.count); ++k) {
            if (overlaps(region, pieces[k])) {
              l.foreign.push_back(local_rect(pieces[k]));
            }
          }
        }
        if (box.subject < c.transitions.size()) {
          mark(c.transitions[box.subject].src, 0);
        }

        switch (search) {
          case LabelSearch::Exhaustive: got = exhaustive(l, scratch); break;
          case LabelSearch::Pruned: got = pruned(l, scratch); break;
          case LabelSearch::Memoized: got = remembered(l, scratch); break;
        }
        if (got.found) {
          got.at.x += origin.x;
          got.at.y += origin.y;
          got.mid += along(points, r, got.seg, origin);
        }
      }
    }

    if (got.found) {
      prior_seg = got.seg;
      prior_mid = got.mid;
      chained = true;
      out[i] = got.at;
      how[i] = { .seg = got.seg, .mid = got.mid, .found = 1 };
    } else {
      ++fallbacks;
      out[i] = centred(anchor_of(points, r), box, z.chart);
    }
    if (based && !kept) {
      LabelSettle const &had{ (*was->settled)[i] };
      scav_rect const &was_at{ (*was->placed)[i] };
      bool const same_box{ (how[i].found == had.found) && (how[i].seg == had.seg) &&
                           (how[i].mid == had.mid) && (out[i].x == was_at.x) &&
                           (out[i].y == was_at.y) && (out[i].w == was_at.w) &&
                           (out[i].h == was_at.h) };
      if (!same_box) {
        dirty.push_back(was_at);
        dirty.push_back(out[i]);
        chain_kept = false;
      }
    }
    settled.push_back(i);
  }
  return fallbacks;
}

}  // namespace

SCAV_INTERNAL_BEGIN

uint32_t place_labels_by(Chart const &c,
                         SizedLayout const &z,
                         scav_spaces const &s,
                         std::vector<scav_span> const &route,
                         std::vector<scav_point> const &points,
                         scav_profile const &p,
                         LabelSearch search,
                         std::vector<scav_rect> &out) {
  std::vector<LabelSettle> how;
  return place_labels_from(c, z, s, route, points, p, search, out, how, nullptr);
}

SCAV_INTERNAL_END

uint32_t place_labels(Chart const &c,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      std::vector<scav_span> const &route,
                      std::vector<scav_point> const &points,
                      scav_profile const &p,
                      std::vector<scav_rect> &out) {
  return place_labels_by(c, z, s, route, points, p, LabelSearch::Memoized, out);
}

uint32_t place_labels(Chart const &c,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      std::vector<scav_span> const &route,
                      std::vector<scav_point> const &points,
                      scav_profile const &p,
                      std::vector<scav_rect> &out,
                      std::vector<LabelSettle> &how,
                      LabelBase const *was) {
  return place_labels_from(c,
                           z,
                           s,
                           route,
                           points,
                           p,
                           LabelSearch::Memoized,
                           out,
                           how,
                           was);
}

}  // namespace scav
