// Strip matching over the finished routes: a leg's two sides are sliced into
// strips, the box slides along each, and the feasible candidate that reads as
// its own transition's, then nearest where centring would have put it, wins.

#include "layout/label.h"

#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_int.h"
#include "scav_stable_sort.h"

#include <cstdint>
#include <vector>

namespace scav {

namespace {

// The eight points of the label's own rectangle the leader may attach to: its
// four corners, then the midpoints of its four sides. Corners because a
// polyline is kinked -- a label hung off a bend belongs on the diagonal, and an
// axis-aligned leader reaches a diagonal position only through a corner.
constexpr uint32_t ATTACH{ 8 };

// The four directions the leader runs in, so its length is exact in integers:
// a diagonal of the same length is not representable on a 1/16-pt grid.
constexpr uint32_t LEADS{ 4 };

// Where attachment point `which` sits inside a `w` by `h` rectangle.
scav_point attach_at(uint32_t which, int32_t w, int32_t h) {
  switch (which) {
    case 0: return { .x = 0, .y = 0 };
    case 1: return { .x = w, .y = 0 };
    case 2: return { .x = 0, .y = h };
    case 3: return { .x = w, .y = h };
    case 4: return { .x = w / 2, .y = 0 };
    case 5: return { .x = w / 2, .y = h };
    case 6: return { .x = 0, .y = h / 2 };
    default: return { .x = w, .y = h / 2 };
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

struct Piece {
  scav_rect at;
  uint32_t trans;
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

// How far inside `reach` the nearest foreign segment lies, `reach` being the
// candidate's gap to its own leg plus one line of its own text (11.6).
Wide shortfall_of(scav_rect const &cand,
                  Wide reach,
                  std::vector<scav_rect> const &foreign) {
  Wide nearest{ reach };
  for (scav_rect const &seg : foreign) {
    nearest = imin(nearest, Wide{ chebyshev_gap(cand, seg) });
    if (nearest == 0) { break; }
  }
  return reach - nearest;
}

bool within(scav_rect const &outer, scav_rect const &inner) {
  return (inner.x >= outer.x) && (inner.y >= outer.y) &&
         ((inner.x + inner.w) <= (outer.x + outer.w)) &&
         ((inner.y + inner.h) <= (outer.y + outer.h));
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

}  // namespace

uint32_t place_labels(Chart const &c,
                      SizedLayout const &z,
                      scav_spaces const &s,
                      std::vector<scav_span> const &route,
                      std::vector<scav_point> const &points,
                      scav_profile const &p,
                      std::vector<scav_rect> &out) {
  out.assign(s.n_path_box, {});
  if ((s.path_box == nullptr) || (s.n_path_box == 0)) { return 0; }

  std::vector<Piece> pieces;
  for (uint32_t t = 0; t < route.size(); ++t) {
    for (uint32_t k = 0; (k + 1) < route[t].len; ++k) {
      pieces.push_back(
          { .at = span_rect(points[route[t].off + k], points[route[t].off + k + 1]),
            .trans = t });
    }
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

  std::vector<scav_rect> blocked;
  std::vector<scav_rect> foreign;
  std::vector<scav_rect> nearby;
  std::vector<scav_rect> own;
  std::vector<uint32_t> settled;
  uint32_t fallbacks{ 0 };
  uint32_t prior_subject{ INVALID };
  uint32_t prior_seg{ 0 };
  int32_t prior_mid{ 0 };
  bool chained{ false };

  for (uint32_t const i : queue) {
    scav_path_box const &box{ s.path_box[i] };
    scav_span const r{ (box.subject < route.size()) ? route[box.subject] : scav_span{} };
    if (box.subject != prior_subject) {
      prior_subject = box.subject;
      chained = false;
    }
    scav_point const at{ anchor_of(points, r) };
    scav_rect best{ centred(at, box, z.chart) };
    Key key{ .shortfall = 0, .dist = -1, .seg = 0, .attach = 0, .lead = 0, .mid = 0 };
    // Half the label's height, so the anchor slides finer than the box it
    // carries; the floor is one grid unit, which is 1/16 pt (11.9.4).
    int32_t const step{ imax(box.h / 2, 1) };
    int32_t const leader{ label_leader(p) };

    if (r.len >= 2) {
      own.assign(r.len - 1, {});
      int32_t x0{ points[r.off].x };
      int32_t x1{ x0 };
      int32_t y0{ points[r.off].y };
      int32_t y1{ y0 };
      for (uint32_t k = 0; (k + 1) < r.len; ++k) {
        own[k] = span_rect(points[r.off + k], points[r.off + k + 1]);
        x0 = imin(x0, own[k].x);
        y0 = imin(y0, own[k].y);
        x1 = imax(x1, own[k].x + own[k].w);
        y1 = imax(y1, own[k].y + own[k].h);
      }
      // Every candidate lies in the route's box grown by the box's extent, the
      // strips, and the box height the distance query reaches past them.
      // Every candidate lies within the leader plus the box from the polyline,
      // and the distance query reaches one box height past that.
      int32_t const reach_x{ box.w + leader + box.h };
      int32_t const reach_y{ box.h + leader + box.h };
      scav_rect const region{ .x = x0 - reach_x,
                              .y = y0 - reach_y,
                              .w = (x1 - x0) + (2 * reach_x),
                              .h = (y1 - y0) + (2 * reach_y) };

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
      blocked.clear();
      for (uint32_t st = 0; st < c.states.size(); ++st) {
        if (c.states[st].live == 0) { continue; }
        // A state enclosing both endpoints holds the label legitimately; the
        // bands it reserved for its own text do not.
        if (encloses[st] == 2) {
          if (overlaps(region, z.before[st])) { blocked.push_back(z.before[st]); }
          if (overlaps(region, z.after[st])) { blocked.push_back(z.after[st]); }
        } else if (overlaps(region, z.state[st])) {
          blocked.push_back(z.state[st]);
        }
      }
      for (uint32_t const j : settled) {
        if (overlaps(region, out[j])) { blocked.push_back(out[j]); }
      }
      foreign.clear();
      for (Piece const &piece : pieces) {
        if (piece.trans == box.subject) { continue; }
        if (overlaps(region, piece.at)) {
          blocked.push_back(piece.at);
          foreign.push_back(piece.at);
        }
      }

      for (uint32_t k = chained ? prior_seg : 0U; (k + 1) < r.len; ++k) {
        scav_point const a{ points[r.off + k] };
        scav_point const b{ points[r.off + k + 1] };
        bool const flat{ a.y == b.y };
        // Equal means diagonal or degenerate: neither has a strip beside it.
        if (flat == (a.x == b.x)) { continue; }
        int32_t const lo{ flat ? imin(a.x, b.x) : imin(a.y, b.y) };
        int32_t const hi{ flat ? imax(a.x, b.x) : imax(a.y, b.y) };
        // The sign of the leg's traversal: `dir` times a difference of two of
        // its coordinates is how much further along the route the first lies.
        bool const ascending{ flat ? (a.x < b.x) : (a.y < b.y) };
        int32_t const dir{ ascending ? 1 : -1 };
        bool const bounded{ chained && (k == prior_seg) };
        // Every candidate for this leg lies within the leader plus the box of
        // it, so the distance prefilter runs once per leg rather than once per
        // band as the strip grid needed.
        nearby.clear();
        for (scav_rect const &seg : foreign) {
          if (chebyshev_gap(own[k], seg) <= (leader + box.w + box.h)) {
            nearby.push_back(seg);
          }
        }
        // Both ends of the leg are anchors whatever the step divides into, so
        // the last slot is clamped rather than skipped.
        int32_t const runs{ (hi - lo) / step };
        for (int32_t n = 0; n <= (runs + 1); ++n) {
          int32_t const mid{ imin(lo + (n * step), hi) };
          // Past the box before it is further along the route, which on a leg
          // running backwards is the smaller coordinate.
          if (bounded && (((mid - prior_mid) * dir) <= 0)) { continue; }
          scav_point const on{ flat ? scav_point{ .x = mid, .y = a.y }
                                    : scav_point{ .x = a.x, .y = mid } };
          for (uint32_t lead = 0; lead < LEADS; ++lead) {
            scav_point const away{ lead_by(lead, leader) };
            for (uint32_t which = 0; which < ATTACH; ++which) {
              // The leader ends on the attachment point, so the box's origin is
              // that point less where the point sits inside the box.
              scav_point const in{ attach_at(which, box.w, box.h) };
              scav_rect const cand{ .x = (on.x + away.x) - in.x,
                                    .y = (on.y + away.y) - in.y,
                                    .w = box.w,
                                    .h = box.h };
              Wide const dx{ (Wide{ cand.x } + floor_div(box.w, 2)) - at.x };
              Wide const dy{ (Wide{ cand.y } + floor_div(box.h, 2)) - at.y };
              Key here{ .shortfall = 0,
                        .dist = imax(dx, -dx) + imax(dy, -dy),
                        .seg = k,
                        .attach = which,
                        .lead = lead,
                        .mid = mid };
              // Keyed before tested, and zero is the floor of any shortfall: one
              // test with it standing in, then the real one, then the sweep.
              if ((key.dist >= 0) && !better(here, key)) { continue; }
              if (!nearby.empty()) {
                here.shortfall = shortfall_of(cand,
                                              chebyshev_gap(cand, own[k]) + Wide{ box.h },
                                              nearby);
                if ((key.dist >= 0) && !better(here, key)) { continue; }
              }
              if (!within(z.chart, cand)) { continue; }
              bool clear{ true };
              for (scav_rect const &obstacle : blocked) {
                if (overlaps(cand, obstacle)) {
                  clear = false;
                  break;
                }
              }
              // Every leg of its own route, this one included: an axis-aligned
              // leader off a corner can still put the label across the line it
              // is anchored to, which is the slice the anchor exists to stop
              // and is not structural (11.9.4).
              for (uint32_t j = 0; clear && (j < own.size()); ++j) {
                if (overlaps(cand, own[j])) { clear = false; }
              }
              if (!clear) { continue; }
              key = here;
              best = cand;
            }
          }
        }
      }
      if (box.subject < c.transitions.size()) { mark(c.transitions[box.subject].src, 0); }
    }

    if (key.dist < 0) {
      ++fallbacks;
    } else {
      prior_seg = key.seg;
      prior_mid = key.mid;
      chained = true;
    }
    out[i] = best;
    settled.push_back(i);
  }
  return fallbacks;
}

}  // namespace scav
