// Strip matching: each box slides along strips beside each leg of its route.

#include "layout/label.h"

#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/memo.h"
#include "layout/size.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_pod_vector.h"
#include "scav_stable_sort.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace scav {

// `place_labels` with the search named; every search returns the same boxes.
SCAV_INTERNAL_BEGIN
uint32_t place_labels_by(Chart const &c,
                         SplitGraph const &g,
                         SizedLayout const &z,
                         scav_spaces const &s,
                         PodVector<scav_span> const &route,
                         PodVector<scav_point> const &points,
                         scav_profile const &p,
                         LabelSearch search,
                         PodVector<scav_rect> &out);
SCAV_INTERNAL_END

namespace {

// True when `m`'s owner holds another live submachine beside it.
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

struct Legs {
  uint32_t first, last;
};

// Legs `[first, last)` of `r`, less leading legs with midpoints strictly inside
// `lca.child[0]` and trailing ones inside `lca.child[1]`; all legs if none remain.
Legs legs_in(SizedLayout const &z,
             CommonAncestor const &lca,
             PodVector<scav_point> const &points,
             scav_span r) {
  uint32_t const legs{ (r.len >= 2) ? (r.len - 1) : 0U };
  auto const inside = [&](uint32_t k, StateId st) {
    if (st.v >= z.state.size()) { return false; }
    scav_rect const &box{ z.state[st.v] };
    scav_point const a{ points[r.off + k] };
    scav_point const b{ points[r.off + k + 1] };
    Wide const x2{ Wide{ a.x } + b.x };
    Wide const y2{ Wide{ a.y } + b.y };
    return (x2 > (Wide{ 2 } * box.x)) && (x2 < (Wide{ 2 } * (Wide{ box.x } + box.w))) &&
           (y2 > (Wide{ 2 } * box.y)) && (y2 < (Wide{ 2 } * (Wide{ box.y } + box.h)));
  };
  uint32_t first{ 0 };
  while ((first < legs) && inside(first, lca.child[0])) { ++first; }
  uint32_t last{ legs };
  while ((last > first) && inside(last - 1, lca.child[1])) { --last; }
  if ((lca.frame.v == INVALID) || (first >= last)) { return { .first = 0, .last = legs }; }
  return { .first = first, .last = last };
}

// Points of the label's rect a leader may attach to: four side midpoints, four corners.
constexpr uint32_t ATTACH{ 8 };

// Axis directions a leader runs in; its length is exact in integers.
constexpr uint32_t LEADS{ 4 };

// Point `which` of a `w` by `h` rect: side midpoints bottom, top, right, left, then
// corners; on a tie the lowest index wins.
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

// The leader's offset: `lead` 0..3 is up, down, left, right by `len`.
scav_point lead_by(uint32_t lead, int32_t len) {
  switch (lead) {
    case 0: return { .x = 0, .y = -len };
    case 1: return { .x = 0, .y = len };
    case 2: return { .x = -len, .y = 0 };
    default: return { .x = len, .y = 0 };
  }
}

// A route's leg rects, `count` from `first` in the shared list, and their bounding box.
struct Pieces {
  scav_rect bounds;
  uint32_t first, count;
};

// A candidate's ranking key; `better` compares lexicographically and the least wins.
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

// `reach` less the nearest gap from `cand` to `foreign`, its index in `at` (INVALID if
// none is within `reach`); past `stop` the scan ends, returning a lower bound.
Wide shortfall_of(scav_rect const &cand,
                  Wide reach,
                  PodVector<scav_rect> const &foreign,
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

// The midpoint of the longest horizontal leg, else of the longest leg.
scav_point anchor_of(PodVector<scav_point> const &points, scav_span route) {
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

// A `box`-sized rect centred on `mid`, clamped into `chart` on each axis where it fits.
scav_rect centred(scav_point mid, scav_path_box const &box, scav_rect const &chart) {
  int32_t x{ mid.x - floor_div(box.w, 2) };
  int32_t y{ mid.y - floor_div(box.h, 2) };
  if (box.w <= chart.w) { x = imin(imax(x, chart.x), (chart.x + chart.w) - box.w); }
  if (box.h <= chart.h) { y = imin(imax(y, chart.y), (chart.y + chart.h) - box.h); }
  return { .x = x, .y = y, .w = box.w, .h = box.h };
}

// One box's search input, relative to the route's first point and clipped to its region;
// `key_of` writes it out as the memo key.
struct Local {
  int32_t w{ 0 }, h{ 0 }, leader{ 0 };
  uint32_t chained{ 0 }, prior_seg{ 0 };
  int32_t prior_mid{ 0 };
  uint32_t first{ 0 }, last{ 0 };  // the legs candidates sit on, from `legs_in`
  scav_rect holder{};
  PodVector<scav_point> route;
  PodVector<scav_rect> walls;    // states, bands and settled boxes
  PodVector<scav_rect> foreign;  // other transitions' legs
  PodVector<scav_rect> below;    // the states the route leaves and enters by
};

// The placed box and its leg and slide; a later box of its transition lies past them.
struct Outcome {
  bool found{ false };
  scav_rect at{};
  uint32_t seg{ 0 };
  int32_t mid{ 0 };
};

// The candidates of one leg, lead and attachment, which differ only in the slide.
// `centre` is the slide nearest the anchor; `near` is the least anchor distance of any.
struct Group {
  uint32_t k, offset, band;
  Wide centre, near;
};

// The groups of a leg on one line across it; `near` holds the leg's nearby segments
// within reach of any of their candidates.
struct Band {
  bool built{ false };
  PodVector<scav_rect> near;
};

// A leg's bands: across it a lead is back, none or forward, and an attachment
// sits at 0, half the box or all of it.
constexpr uint32_t BANDS{ 9 };

// The band of offset `o`, from `lead_by` and `attach_at` read across the leg; offsets in
// one band put the box on one line across it.
uint32_t band_of_offset(uint32_t o, bool flat) {
  constexpr std::array<uint32_t, LEADS> LEAD_Y{ 0, 1, 2, 2 };
  constexpr std::array<uint32_t, LEADS> LEAD_X{ 0, 0, 1, 2 };
  constexpr std::array<uint32_t, ATTACH> IN_Y{ 0, 1, 2, 2, 1, 1, 0, 0 };
  constexpr std::array<uint32_t, ATTACH> IN_X{ 0, 0, 1, 2, 2, 1, 2, 1 };
  uint32_t const lead{ o / ATTACH };
  uint32_t const which{ o % ATTACH };
  return flat ? ((LEAD_Y[lead] * 3) + IN_Y[which]) : ((LEAD_X[lead] * 3) + IN_X[which]);
}

// One leg's groups, from `first`, and the least anchor distance of any of them.
struct Leg {
  uint32_t first;
  Wide near;
};

// Working vectors kept across one call's boxes; `bands` holds `BANDS` per leg. `gridded`
// is set once `grid` is built over `blocked`.
struct Scratch {
  PodVector<scav_rect> own;
  PodVector<scav_rect> blocked;
  std::vector<PodVector<scav_rect>> nearby;
  PodVector<Group> groups;
  PodVector<Leg> legs;
  std::vector<Band> bands;
  RectGrid grid;
  PodVector<uint32_t> fill;
  scav_rect region{};
  bool gridded{ false };
};

// The grid over `blocked`, built on first use.
RectGrid &grid_of(Scratch &s, int32_t cell_w, int32_t cell_h) {
  if (!s.gridded) {
    s.gridded = true;
    grid_build(s.grid, s.region, s.blocked, cell_w, cell_h, s.fill);
  }
  return s.grid;
}

// The coordinate a slide along leg `k` is measured in: x on a horizontal leg,
// y on any other.
int32_t along(PodVector<scav_point> const &points,
              scav_span r,
              uint32_t k,
              scav_point at) {
  bool const flat{ ((k + 1) < r.len) && (points[r.off + k].y == points[r.off + k + 1].y) };
  return flat ? at.x : at.y;
}

scav_rect relative_to(scav_rect const &r, scav_point origin) {
  return { .x = r.x - origin.x, .y = r.y - origin.y, .w = r.w, .h = r.h };
}

// The route's bounds grown by the leader, the box and one more box height: the reach of
// every candidate and its distance query.
scav_rect region_of(PodVector<scav_point> const &route,
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

// Fills `s.own` with the route's legs and `s.blocked` with walls then foreign legs.
void prepare(Local const &l, Scratch &s) {
  uint32_t const n{ static_cast<uint32_t>(l.route.size()) };
  s.own.assign(n - 1, {});
  for (uint32_t k = 0; (k + 1) < n; ++k) {
    s.own[k] = span_rect(l.route[k], l.route[k + 1]);
  }
  if (s.nearby.size() < s.own.size()) { vec_resize(s.nearby, s.own.size()); }
  s.blocked.assign(l.walls.begin(), l.walls.end());
  s.blocked.insert(s.blocked.end(), l.foreign.begin(), l.foreign.end());
  s.region = region_of(l.route, l.w, l.h, l.leader);
  s.gridded = false;
}

// The foreign legs within `leader + w + h` of leg `k`, into `s.nearby[k]`.
PodVector<scav_rect> const &nearby_of(Local const &l, Scratch &s, uint32_t k) {
  PodVector<scav_rect> &out{ s.nearby[k] };
  out.clear();
  for (scav_rect const &seg : l.foreign) {
    if (chebyshev_gap(s.own[k], seg) <= (l.leader + l.w + l.h)) { out.push_back(seg); }
  }
  return out;
}

#ifdef SCAV_TESTING
// Keys and tests every candidate of every leg, both tiers at once; `pruned` returns the
// same answer.
Outcome exhaustive(Local const &l, Scratch &s) {
  uint32_t const len{ static_cast<uint32_t>(l.route.size()) };
  prepare(l, s);
  scav_point const at{ anchor_of(l.route,
                                 { .off = l.first, .len = (l.last - l.first) + 1 }) };
  // The slide step: half the label's height, at least 1.
  int32_t const step{ imax(l.h / 2, 1) };
  bool const chained{ l.chained != 0 };
  scav_rect best{};
  Key key{ .shortfall = 0, .dist = -1, .seg = 0, .attach = 0, .lead = 0, .mid = 0 };
  // The fallback tier's best, which skips the test against `blocked`.
  scav_rect lax{};
  Key lax_key{ .shortfall = 0, .dist = -1, .seg = 0, .attach = 0, .lead = 0, .mid = 0 };

  for (uint32_t k = imax(chained ? l.prior_seg : 0U, l.first);
       (k < l.last) && ((k + 1) < len);
       ++k) {
    scav_point const a{ l.route[k] };
    scav_point const b{ l.route[k + 1] };
    bool const flat{ a.y == b.y };
    if (flat == (a.x == b.x)) { continue; }  // diagonal or zero length
    int32_t const lo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
    int32_t const hi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
    // `dir * (p - q)` is how far along the route `p` lies past `q`.
    bool const ascending{ flat ? (a.x < b.x) : (a.y < b.y) };
    int32_t const dir{ ascending ? 1 : -1 };
    bool const bounded{ chained && (k == l.prior_seg) };
    PodVector<scav_rect> const &nearby{ nearby_of(l, s, k) };
    // Slides step from `lo`; the last is clamped to `hi`, so both ends are anchors.
    int32_t const runs{ (hi - lo) / step };
    for (int32_t n = 0; n <= (runs + 1); ++n) {
      int32_t const mid{ imin(lo + (n * step), hi) };
      // A chained box lies further along the route than its predecessor; on a leg
      // running backwards that is the smaller coordinate.
      if (bounded && (((mid - l.prior_mid) * dir) <= 0)) { continue; }
      scav_point const on{ flat ? scav_point{ .x = mid, .y = a.y }
                                : scav_point{ .x = a.x, .y = mid } };
      for (uint32_t lead = 0; lead < LEADS; ++lead) {
        scav_point const away{ lead_by(lead, l.leader) };
        for (uint32_t which = 0; which < ATTACH; ++which) {
          // The box's origin: the leader's end less the attachment point's offset.
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
          // Prunes by key against the strict tier's incumbent before any test.
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
          // Both tiers reject a box overlapping its own route's legs or `below`.
          bool uncut{ true };
          for (uint32_t j = 0; uncut && (j < s.own.size()); ++j) {
            if (overlaps(cand, s.own[j])) { uncut = false; }
          }
          for (uint32_t j = 0; uncut && (j < l.below.size()); ++j) {
            if (overlaps(cand, l.below[j])) { uncut = false; }
          }
          if (!uncut) { continue; }
          if ((lax_key.dist < 0) || better(here, lax_key)) {
            lax_key = here;
            lax = cand;
          }
          if (grid_visit(grid_of(s, l.w, l.h), cand, 0, [&](uint32_t j) {
                return overlaps(cand, s.blocked[j]);
              })) {
            continue;
          }
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
#endif

// One tier of the pruned search over one box, and its incumbent. `last` is the wall that
// most recently refused a candidate, tried before the grid.
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

  // True when no candidate at least `dist` from the anchor can beat the incumbent.
  [[nodiscard]] bool beyond(Wide dist) const {
    return (key.dist >= 0) && (key.shortfall == 0) && (dist > key.dist);
  }

  Band const &band_of(Group const &g);
  void group(Group const &g);
};

// The segments of `s.nearby[k]` within the leader plus one box height of the rect holding
// every candidate of `g`'s band; built once per band.
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

// Walks `g`'s slides out from `centre` both ways; a refused slide jumps past the rect that
// refused it, and a side ends where no later slide can win.
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
  // Lower bound on a candidate's reach at any slide: its gap across the leg plus `l.h`.
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

  // Clamps a slide number to `[-1, runs + 2]`, one past either end of the walk.
  auto const clamp = [runs](Wide n) {
    return static_cast<int32_t>(imin(imax(n, Wide{ -1 }), Wide{ runs } + 2));
  };
  // The first slide past `n` in direction `sign` whose candidate clears `r` along the leg.
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
    for (scav_rect const &inner : l.below) {
      if (overlaps(cand, inner)) { return past(inner, n, sign); }
    }
    if (strict) {
      if ((last != INVALID) && overlaps(cand, s.blocked[last])) {
        return past(s.blocked[last], n, sign);
      }
      uint32_t hit{ INVALID };
      if (grid_visit(grid_of(s, l.w, l.h), cand, 0, [&](uint32_t j) {
            hit = j;
            return overlaps(cand, s.blocked[j]);
          })) {
        last = hit;
        return past(s.blocked[hit], n, sign);
      }
    }
    PodVector<scav_rect> const &nearby{ band_of(g).near };
    if (!nearby.empty()) {
      uint32_t nearest{ INVALID };
      here.shortfall = shortfall_of(cand,
                                    chebyshev_gap(cand, s.own[k]) + Wide{ l.h },
                                    nearby,
                                    (key.dist >= 0) ? key.shortfall : NO_LIMIT,
                                    nearest);
      if ((key.dist >= 0) && !better(here, key)) {
        // A segment nearer than the least reach less the incumbent's shortfall refuses
        // every slide that near it, as a wall grown by that much.
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

  // The slide at or below `centre`, clamped onto the leg; the anchor distance grows
  // outward from it both ways.
  int32_t const first{ static_cast<int32_t>(
      imin(imax(floor_div(g.centre - lo, Wide{ step }), Wide{ 0 }), Wide{ runs } + 1)) };
  // A box straddling its own leg's line clears the leg only past the end its lead and
  // attachment put it beyond: from `from` up, or up to `to`.
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

// Returns `exhaustive`'s answer; the fallback tier runs only when the strict tier finds
// nothing, and legs and groups go nearest the anchor first.
Outcome pruned(Local const &l, Scratch &s) {
  uint32_t const len{ static_cast<uint32_t>(l.route.size()) };
  prepare(l, s);
  scav_point const at{ anchor_of(l.route,
                                 { .off = l.first, .len = (l.last - l.first) + 1 }) };
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

  // A group's anchor distance is fixed across the leg and grows with the slide's distance
  // from `centre` along it.
  s.groups.clear();
  s.legs.clear();
  for (uint32_t k = imax((l.chained != 0) ? l.prior_seg : 0U, l.first);
       (k < l.last) && ((k + 1) < len);
       ++k) {
    scav_point const a{ l.route[k] };
    scav_point const b{ l.route[k + 1] };
    bool const flat{ a.y == b.y };
    if (flat == (a.x == b.x)) { continue; }
    nearby_of(l, s, k);
    Wide const lo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
    Wide const hi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
    Leg leg{ .first = static_cast<uint32_t>(s.groups.size()), .near = NO_LIMIT };
    uint32_t const bands{ static_cast<uint32_t>(s.legs.size()) * BANDS };
    if (s.bands.size() < (bands + BANDS)) { vec_resize(s.bands, bands + BANDS); }
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

// `memo_hash`'s round taken two words at a time.
uint64_t label_hash(PodVector<uint32_t> const &key) {
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

// Every box this thread has placed, keyed by `key_of`; capped at 2^20 words.
Memo &memo() {
  thread_local Memo m{ size_t{ 1 } << 20, label_hash };
  return m;
}

// Every field of `l`, counts before contents, so distinct problems get distinct keys.
void key_of(Local const &l, PodVector<uint32_t> &key) {
  key.resize(16 + (2 * l.route.size()) + (4 * l.walls.size()) + (4 * l.foreign.size()) +
             (4 * l.below.size()));
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
  *at++ = l.first;
  *at++ = l.last;
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
  *at++ = static_cast<uint32_t>(l.below.size());
  for (scav_rect const &r : l.below) { rect(r); }
}

Outcome remembered(Local const &l, Scratch &s) {
  thread_local PodVector<uint32_t> key;
  thread_local PodVector<int32_t> value;
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

// Per-thread buffers for one `place_labels_by` call; the call never waits on the pool.
struct CallBuffers {
  PodVector<uint8_t> encloses;
  PodVector<scav_rect> pieces;
  PodVector<Pieces> by_route;
  PodVector<uint32_t> live, queue, merge, settled;
  PodVector<scav_extent> loop_label;
  Local local;
};

CallBuffers &call_buffers() {
  thread_local CallBuffers b;
  return b;
}

}  // namespace

SCAV_INTERNAL_BEGIN

uint32_t place_labels_by(Chart const &c,
                         SplitGraph const &g,
                         SizedLayout const &z,
                         scav_spaces const &s,
                         PodVector<scav_span> const &route,
                         PodVector<scav_point> const &points,
                         scav_profile const &p,
                         LabelSearch search,
                         PodVector<scav_rect> &out) {
  out.assign(s.n_path_box, {});
  if ((s.path_box == nullptr) || (s.n_path_box == 0)) { return 0; }

  CallBuffers &cb{ call_buffers() };
  PodVector<scav_rect> &pieces{ cb.pieces };
  pieces.clear();
  PodVector<Pieces> &by_route{ cb.by_route };
  by_route.assign(route.size(), Pieces{});
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
  PodVector<uint32_t> &live{ cb.live };
  live.clear();
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if (c.states[st].live != 0) { live.push_back(st); }
  }

  // By transition, then `order`; a box sees its transition's earlier boxes as settled.
  PodVector<uint32_t> &queue{ cb.queue };
  queue.resize(s.n_path_box);
  for (uint32_t i = 0; i < s.n_path_box; ++i) { queue[i] = i; }
  scav_stable_sort(queue, cb.merge, [&s](uint32_t a, uint32_t b) {
    if (s.path_box[a].subject != s.path_box[b].subject) {
      return s.path_box[a].subject < s.path_box[b].subject;
    }
    return s.path_box[a].order < s.path_box[b].order;
  });

  // The states a label may lie inside: the owner of the lowest submachine holding both
  // ends, and every state enclosing it.
  PodVector<uint8_t> &encloses{ cb.encloses };
  encloses.assign(c.states.size(), 0);
  auto const mark = [&](StateId from, uint8_t v) {
    StateId at{ from };
    for (size_t step = 0; (step < c.states.size()) && (at.v != INVALID); ++step) {
      encloses[at.v] = v;
      at = enclosing_state(c, at);
    }
  };

  Local &l{ cb.local };          // every field set per box before it is read
  thread_local Scratch scratch;  // every search resets what it reads first
  PodVector<uint32_t> &settled{ cb.settled };
  settled.clear();
  uint32_t fallbacks{ 0 };
  uint32_t prior_subject{ INVALID };
  uint32_t prior_seg{ 0 };
  int32_t prior_mid{ 0 };
  bool chained{ false };
  int32_t const leader{ label_leader(p) };
  loop_labels(c, s, cb.loop_label);
  int32_t loop_y{ 0 };  // the next box's top beside the current subject's loop

  for (uint32_t const i : queue) {
    scav_path_box const &box{ s.path_box[i] };
    scav_span const r{ (box.subject < route.size()) ? route[box.subject] : scav_span{} };
    bool const looped{ (box.subject < c.transitions.size()) &&
                       inner_loop(c, box.subject) };
    if (box.subject != prior_subject) {
      prior_subject = box.subject;
      chained = false;
      if (looped && (r.len >= 3)) {
        uint32_t const face{ loop_place(z, c.transitions[box.subject].src.v).face };
        int32_t const stack_h{ cb.loop_label[box.subject].h };
        Wide const mid{ (Wide{ points[r.off + 1].y } + points[r.off + 2].y) / 2 };
        int32_t const leg{ points[r.off + 1].y };
        loop_y = static_cast<int32_t>(mid - (stack_h / 2));
        if (face >= 2) {
          loop_y = (face == 2) ? (leg + loop_gap(p)) : (leg - loop_gap(p) - stack_h);
        }
      }
    }
    if (looped && (r.len >= 3)) {
      // Stacked `loop_gap` beyond the loop's far leg, in the room its row reserved.
      uint32_t const face{ loop_place(z, c.transitions[box.subject].src.v).face };
      int32_t const leg{ points[r.off + 1].x };
      Wide const mid{ (Wide{ points[r.off + 1].x } + points[r.off + 2].x) / 2 };
      int32_t x{ (face == 0) ? (leg + loop_gap(p)) : (leg - loop_gap(p) - box.w) };
      if (face >= 2) { x = static_cast<int32_t>(mid - (box.w / 2)); }
      out[i] = { .x = x, .y = loop_y, .w = box.w, .h = box.h };
      loop_y += box.h;
      settled.push_back(i);
      continue;
    }
    Outcome got{};
    CommonAncestor const lca{ (box.subject < c.transitions.size())
                                  ? g.trans_common[box.subject]
                                  : CommonAncestor{} };
    Legs const legs{ legs_in(z, lca, points, r) };
    scav_span const anchored{ (r.len >= 2)
                                  ? scav_span{ .off = r.off + legs.first,
                                               .len = (legs.last - legs.first) + 1 }
                                  : r };

    if (r.len >= 2) {
      scav_point const origin{ points[r.off] };
      l.w = box.w;
      l.h = box.h;
      l.leader = leader;
      l.chained = chained ? 1U : 0U;
      l.prior_seg = chained ? prior_seg : 0U;
      l.prior_mid = chained ? (prior_mid - along(points, r, prior_seg, origin)) : 0;
      l.first = legs.first;
      l.last = legs.last;
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
      // Ends in two regions of one state share no submachine; that state holds them.
      StateId const within{ (lca.frame.v != INVALID) ? c.submachines[lca.frame.v].owner
                                                     : enclosing_state(c, lca.child[0]) };
      mark(within, 1);
      // A label inside a composite is bounded by it: the intersection of every state
      // enclosing both endpoints.
      scav_rect holder{ z.chart };
      for (uint32_t const st : live) {
        if (encloses[st] != 0) { holder = intersection(holder, z.state[st]); }
      }
      // Also bounded by the endpoints' common region where its state holds another region.
      if ((lca.frame.v < c.submachines.size()) && shared_region(c, lca.frame)) {
        holder = intersection(holder, z.sub[lca.frame.v]);
      }
      // Clipping a rect to the region changes no test on a box of positive extent.
      bool const clip{ (box.w > 0) && (box.h > 0) };
      auto const local_rect = [&](scav_rect const &at) {
        scav_rect const shifted{ relative_to(at, origin) };
        return clip ? intersection(shifted, local_region) : shifted;
      };
      l.holder = relative_to(holder, origin);
      if (clip) {
        // Unclamped: a holder the region misses gets negative extent and refuses all.
        int32_t const x0{ imax(l.holder.x, local_region.x) };
        int32_t const y0{ imax(l.holder.y, local_region.y) };
        int32_t const x1{ imin(l.holder.x + l.holder.w, local_region.x + local_region.w) };
        int32_t const y1{ imin(l.holder.y + l.holder.h, local_region.y + local_region.h) };
        l.holder = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
      }

      l.walls.clear();
      for (uint32_t const st : live) {
        // A state enclosing both endpoints adds only its text bands as walls; any other
        // state adds its whole rect.
        if (encloses[st] != 0) {
          for (scav_rect const &band : state_walls(z, st)) {
            if ((band.w > 0) && (band.h > 0) && overlaps(region, band)) {
              l.walls.push_back(local_rect(band));
            }
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
          if (overlaps(region, pieces[k])) { l.foreign.push_back(local_rect(pieces[k])); }
        }
      }
      mark(within, 0);
      l.below.clear();
      if (lca.frame.v != INVALID) {
        for (uint32_t k = 0; k < lca.child.size(); ++k) {
          StateId const st{ lca.child[k] };
          if ((st.v == INVALID) || ((k == 1) && (st == lca.child[0]))) { continue; }
          if (overlaps(region, z.state[st.v])) {
            l.below.push_back(local_rect(z.state[st.v]));
          }
        }
      }

      switch (search) {
        case LabelSearch::Pruned: got = pruned(l, scratch); break;
        case LabelSearch::Memoized: got = remembered(l, scratch); break;
#ifdef SCAV_TESTING
        case LabelSearch::Exhaustive: got = exhaustive(l, scratch); break;
#endif
      }
      if (got.found) {
        got.at.x += origin.x;
        got.at.y += origin.y;
        got.mid += along(points, r, got.seg, origin);
      }
    }

    if (got.found) {
      prior_seg = got.seg;
      prior_mid = got.mid;
      chained = true;
      out[i] = got.at;
    } else {
      ++fallbacks;
      out[i] = centred(anchor_of(points, anchored), box, z.chart);
      if (box.subject < g.trans_segments.size()) {
        trace_emit({ .kind = TraceKind::LabelCentred,
                     .seg = { .seg = g.trans_segments[box.subject].off } });
      }
    }
    settled.push_back(i);
  }
  return fallbacks;
}

SCAV_INTERNAL_END

uint32_t place_labels(Chart const &c,
                      SplitGraph const &g,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      PodVector<scav_span> const &route,
                      PodVector<scav_point> const &points,
                      scav_profile const &p,
                      PodVector<scav_rect> &out) {
  return place_labels_by(c, g, z, s, route, points, p, LabelSearch::Memoized, out);
}

}  // namespace scav
