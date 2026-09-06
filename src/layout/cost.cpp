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

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace scav {

namespace {

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

// Two routes' shared ends: the identical points they finish and start with,
// and whether the leg reaching each of those runs lies along it.
struct Trunk {
  uint32_t tail{ 0 }, head{ 0 };
  bool merged_tail{ false }, merged_head{ false };
};

Trunk trunk_of(std::vector<scav_point> const &pts, scav_span a, scav_span b) {
  Trunk out;
  uint32_t const shortest{ imin(a.len, b.len) };
  while ((out.tail < shortest) &&
         same(pts[(a.off + a.len - 1) - out.tail], pts[(b.off + b.len - 1) - out.tail])) {
    ++out.tail;
  }
  while ((out.head < shortest) && same(pts[a.off + out.head], pts[b.off + out.head])) {
    ++out.head;
  }
  if ((out.tail > 0) && (out.tail < a.len) && (out.tail < b.len)) {
    uint32_t const i{ (a.off + a.len - 1) - out.tail };
    uint32_t const j{ (b.off + b.len - 1) - out.tail };
    out.merged_tail = shared_run(pts[i], pts[i + 1], pts[j], pts[j + 1]) > 0;
  }
  if ((out.head > 0) && (out.head < a.len) && (out.head < b.len)) {
    uint32_t const i{ (a.off + out.head) - 1 };
    uint32_t const j{ (b.off + out.head) - 1 };
    out.merged_head = shared_run(pts[i], pts[i + 1], pts[j], pts[j + 1]) > 0;
  }
  return out;
}

// Segment `k` of one route of the pair lies in the trunk: inside a shared run,
// or the leg that merges into one. `len` counts that route's points.
bool trunk_piece(Trunk const &t, uint32_t len, uint32_t k) {
  return ((k + t.tail) >= len) || (t.merged_tail && ((k + t.tail + 1) == len)) ||
         ((k + 1) < t.head) || (t.merged_head && ((k + 1) == t.head));
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

}  // namespace

CostTerms cost_terms(Chart const &c,
                     SplitGraph const &g,
                     SizedLayout const &z,
                     Routes const &r,
                     scav_spaces const &s,
                     scav_profile const &p) {
  CostTerms t;
  t.aspect = (Wide{ z.chart.w } * p.dar_den) - (Wide{ z.chart.h } * p.dar_num);
  if (t.aspect < 0) { t.aspect = -t.aspect; }
  t.area = Wide{ z.chart.w } * z.chart.h;

  // Every route segment once, with the transition it belongs to, so the
  // pair sweep below is over one flat list.
  struct Piece {
    scav_point a, b;
    uint32_t trans;
    uint32_t k;  // its index in that transition's own polyline
  };
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
  for (uint32_t i = 0; i < pieces.size(); ++i) {
    Piece const &u{ pieces[i] };
    for (uint32_t j = i + 1; j < pieces.size(); ++j) {
      Piece const &v{ pieces[j] };
      if (u.trans == v.trans) { continue; }
      if (crosses(u.a, u.b, v.a, v.b)) {
        ++t.crossings;
        ++crossings_of[u.trans];
        ++crossings_of[v.trans];
      }
      // Corridor is the length one pair shares along one line, so a run three
      // nets lie on is charged over its three pairs. Two routes fanning into
      // one trunk are not that run and are not charged for it.
      Wide const shared{ shared_run(u.a, u.b, v.a, v.b) };
      if (shared <= 0) { continue; }
      Trunk const pair{ trunk_of(r.points, r.route[u.trans], r.route[v.trans]) };
      if (trunk_piece(pair, r.route[u.trans].len, u.k) &&
          trunk_piece(pair, r.route[v.trans].len, v.k)) {
        continue;
      }
      t.corridor += shared;
    }
  }

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
  std::vector<uint8_t> encloses(c.states.size(), 0);
  auto const mark = [&](StateId of, uint8_t v) {
    StateId at{ of };
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
      mark(c.transitions[subject].src, 1);
      mark(c.transitions[subject].dst, 1);
    }
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if (c.states[st].live == 0) { continue; }
      if (encloses[st] != 0) {
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
    if (subject != INVALID) {
      mark(c.transitions[subject].src, 0);
      mark(c.transitions[subject].dst, 0);
    }
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

  // Tier 0. Siblings may not overlap; a nested box legitimately does, so the
  // pairs are taken within one submachine's children.
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    Span const kids{ c.submachines[m].children };
    for (uint32_t i = 0; i < kids.len; ++i) {
      uint32_t const a{ c.state_ids[kids.off + i].v };
      if (c.states[a].live == 0) { continue; }
      for (uint32_t j = i + 1; j < kids.len; ++j) {
        uint32_t const b{ c.state_ids[kids.off + j].v };
        if (c.states[b].live == 0) { continue; }
        if (overlaps(z.state[a], z.state[b])) { ++t.box_overlap; }
      }
    }
  }
  // An edge may occupy the interior of a state it is an endpoint of or a
  // descendant of, and only that one: 11.14's carve-out.
  for (Piece const &piece : pieces) {
    Transition const &tr{ c.transitions[piece.trans] };
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if (c.states[st].live == 0) { continue; }
      if (ancestor_or_self(c, { st }, tr.src) || ancestor_or_self(c, { st }, tr.dst)) {
        continue;
      }
      if (enters(piece.a, piece.b, z.state[st])) { ++t.through_box; }
    }
  }
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

Cost cost_of(CostTerms const &t, scav_profile const &p) {
  Cost out;
  out.t0_violations = t.through_box + t.box_overlap;
  out.t2 = (Wide{ p.w_bends } * t.bends) + (Wide{ p.w_corridor } * t.corridor) +
           (Wide{ p.w_crossings } * t.crossings) +
           (Wide{ p.w_excess_len } * t.excess_len) +
           (Wide{ p.w_adjacency } * t.adjacency) + (Wide{ p.w_label } * t.label) +
           (Wide{ p.w_label_near } * t.label_near) + (Wide{ p.w_aspect } * t.aspect) +
           (Wide{ p.w_area } * t.area);
  return out;
}

bool cost_less(Cost const &a, Cost const &b) {
  if (a.t0_violations != b.t0_violations) { return a.t0_violations < b.t0_violations; }
  if (a.t1_hints != b.t1_hints) { return a.t1_hints < b.t1_hints; }
  return a.t2 < b.t2;
}

}  // namespace scav
