// One layout element per chart, held to the properties a reader checks.

#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/router.h"
#include "layout/size.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"

#include "doctest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

// Portfolio row `index` as a delta from `p`; defined in `layout.cpp` under SCAV_INTERNAL.
Row search_row(scav_profile const &p, uint32_t index);

}  // namespace scav

namespace {

using namespace scav;

constexpr uint32_t PROFILE_SIZE{ static_cast<uint32_t>(sizeof(scav_profile)) };

scav_profile readable() {
  scav_profile p{};
  REQUIRE(scav_profile_named("readable", &p, PROFILE_SIZE) == SCAV_OK);
  return p;
}

scav_profile compact() {
  scav_profile p{};
  REQUIRE(scav_profile_named("compact", &p, PROFILE_SIZE) == SCAV_OK);
  return p;
}

// Every chart in test_data/charts/gauntlet, by file name.
constexpr std::array GAUNTLET{
  "above.scav",    "bypass.scav",    "carried.scav", "chain.scav",     "corner.scav",
  "crossing.scav", "crowd.scav",     "detour.scav",  "enclosing.scav", "entered.scav",
  "fanin.scav",    "flank.scav",     "folded.scav",  "fork.scav",      "header.scav",
  "headed.scav",   "inloop.scav",    "inward.scav",  "lane.scav",      "level.scav",
  "long.scav",     "loop.scav",      "marks.scav",   "mixed.scav",     "mutual.scav",
  "ported.scav",   "pulled.scav",    "rebound.scav", "reentry.scav",   "regions.scav",
  "resumed.scav",  "ring.scav",      "room.scav",    "rooms.scav",     "roundtrip.scav",
  "seated.scav",   "separator.scav", "side.scav",    "stretch.scav",   "through.scav",
  "tight.scav",    "transit.scav",   "under.scav",   "unfolded.scav"
};

// One chart, laid out: the pieces every property below reads.
struct Laid {
  Chart c;
  SplitGraph g;
  SubmachineOrders o;
  SizedLayout z;
  Routes r;
  SearchPins pins;            // the kept drawing's pins
  uint32_t tuple{ INVALID };  // the portfolio row the run kept
  uint32_t lane_moves{ 0 };   // segments nudging moved onto a lane
  uint32_t unplaced{ 0 };     // path boxes that took the centred placement
};

// `p` with one portfolio row and no bounded moves: row 0 unsearched, the caller's tuple.
scav_profile one_row(scav_profile const &p) {
  scav_profile out{ p };
  out.portfolio_m = 1;
  out.portfolio_k = 0;
  return out;
}

// Requires column `name` to equal `rows` word for word; geometry PODs are padding-free
// int32 blocks.
template <typename T>
void column_holds(Chart const &c, char const *name, std::vector<T> const &rows) {
  static_assert((sizeof(T) % sizeof(int32_t)) == 0, "geometry PODs are int32 blocks");
  constexpr uint32_t WORDS{ sizeof(T) / sizeof(int32_t) };
  ColumnId const id{ column_find(c, name) };
  REQUIRE(id.v != INVALID);
  REQUIRE(column_count(c, id) == static_cast<uint32_t>(rows.size()));
  for (uint32_t i = 0; i < rows.size(); ++i) {
    std::array<int32_t, WORDS> want{};
    std::array<int32_t, WORDS> got{};
    std::memcpy(want.data(), &rows[i], sizeof(T));
    std::memcpy(got.data(),
                column_data(c, id) + (static_cast<size_t>(i) * sizeof(T)),
                sizeof(T));
    CAPTURE(name);
    CAPTURE(i);
    REQUIRE(want == got);
  }
}

// `layout_run` on `name`, then the phases re-run for the row it kept; `s` and `seed` are
// the run's space requests and starting pins, and every `external` transition takes
// `external_as`.
void lay(char const *name,
         scav_profile const &p,
         Laid &out,
         scav_spaces const &s,
         SearchPins const *seed,
         TransKind external_as = TransKind::External) {
  scav_router_id id{};
  REQUIRE(router_by_name(reinterpret_cast<scav_byte const *>("orthogonal"), 10, id));
  std::string path{ SCAV_TEST_DATA_DIR "/charts/gauntlet/" };
  path += name;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, out.c, diags, failed));
  for (Transition &tr : out.c.transitions) {
    if (tr.kind == TransKind::External) { tr.kind = external_as; }
  }

  std::vector<scav_placed> placed;
  scav_layout_opts const o{ .profile = p, .router = id, .threads = 0 };
  SearchPins &pins{ out.pins };
  REQUIRE(layout_run(out.c,
                     s,
                     o,
                     placed,
                     diags,
                     nullptr,
                     &out.tuple,
                     INVALID,
                     nullptr,
                     &pins,
                     seed));
  CHECK(diags.empty());  // RouteDegraded included

  auto const [knobs, dar, pack, fold]{ search_row(p, out.tuple) };
  out.g = decompose(out.c);
  // Re-derives the kept drawing from its row and pins; order and route both read the pins.
  out.o = order_submachines(out.c, out.g, s, knobs, 0, pins);
  REQUIRE(size_layout(out.c, out.g, out.o, s, knobs, out.z, diags, dar, pack, fold));
  LayoutTrace routed;
  trace_sink_set(&routed);
  out.r = route_transitions(out.c,
                            out.g,
                            out.o,
                            out.z,
                            s,
                            knobs,
                            *router_at(id),
                            0,
                            nullptr,
                            nullptr,
                            &pins);
  trace_sink_set(nullptr);
  out.lane_moves =
      static_cast<uint32_t>(std::ranges::count_if(routed.events, [](TraceEvent const &e) {
        return e.kind == TraceKind::LaneAssigned;
      }));
  std::vector<scav_rect> boxes;
  out.unplaced =
      place_labels(out.c, out.g, out.z, s, out.r.route, out.r.points, knobs, boxes);
  column_holds(out.c, "scav.geom.state", out.z.state);
  column_holds(out.c, "scav.geom.sub", out.z.sub);
  column_holds(out.c, "scav.geom.point", out.r.points);
  column_holds(out.c, "scav.geom.route", out.r.route);
  column_holds(out.c, "scav.geom.port", out.r.port);
  column_holds(out.c, "scav.geom.portslot", out.r.slots);
}

// With no spaces or seed, each chart and profile is laid out once per process; a hit
// copies it.
void lay(char const *name, scav_profile const &p, Laid &out) {
  static std::map<std::string, Laid> laid;
  std::string key{ name };
  key.append(reinterpret_cast<char const *>(&p), sizeof p);  // a flat POD of int32_t
  auto it{ laid.find(key) };
  if (it == laid.end()) {
    Laid fresh;
    lay(name, p, fresh, {}, nullptr);
    it = laid.emplace(std::move(key), std::move(fresh)).first;
  }
  out = it->second;
}

// Tier-0 geometry predicates, implemented independently of the scorer.
Wide orient(scav_point a, scav_point b, scav_point c) {
  return ((Wide{ b.x } - a.x) * (Wide{ c.y } - a.y)) -
         ((Wide{ b.y } - a.y) * (Wide{ c.x } - a.x));
}

bool crosses(scav_point a, scav_point b, scav_point c, scav_point d) {
  Wide const d1{ orient(a, b, c) };
  Wide const d2{ orient(a, b, d) };
  Wide const d3{ orient(c, d, a) };
  Wide const d4{ orient(c, d, b) };
  if ((d1 == 0) || (d2 == 0) || (d3 == 0) || (d4 == 0)) { return false; }
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

bool strictly_inside(scav_point at, scav_rect const &r) {
  return (at.x > r.x) && (at.x < (r.x + r.w)) && (at.y > r.y) && (at.y < (r.y + r.h));
}

bool enters(scav_point a, scav_point b, scav_rect const &r) {
  if (strictly_inside(a, r) || strictly_inside(b, r)) { return true; }
  scav_point const tl{ .x = r.x, .y = r.y };
  scav_point const tr{ .x = r.x + r.w, .y = r.y };
  scav_point const bl{ .x = r.x, .y = r.y + r.h };
  scav_point const br{ .x = r.x + r.w, .y = r.y + r.h };
  return crosses(a, b, tl, tr) || crosses(a, b, bl, br) || crosses(a, b, tl, bl) ||
         crosses(a, b, tr, br);
}

bool ancestor(Chart const &c, StateId maybe, StateId of) {
  for (StateId at{ of }; at.v != INVALID;
       at = c.submachines[c.states[at.v].parent.v].owner) {
    if (at == maybe) { return true; }
  }
  return false;
}

bool on_border(scav_point at, scav_rect const &r) {
  bool const in_x{ (at.x >= r.x) && (at.x <= (r.x + r.w)) };
  bool const in_y{ (at.y >= r.y) && (at.y <= (r.y + r.h)) };
  return (in_x && in_y) && ((at.x == r.x) || (at.x == (r.x + r.w)) || (at.y == r.y) ||
                            (at.y == (r.y + r.h)));
}

// Whether `at` is on one of `r`'s two long faces; corners belong to the caps.
bool on_long_face(scav_point at, scav_rect const &r) {
  if (r.w < r.h) {
    return ((at.x == r.x) || (at.x == (r.x + r.w))) && (at.y > r.y) &&
           (at.y < (r.y + r.h));
  }
  return ((at.y == r.y) || (at.y == (r.y + r.h))) && (at.x > r.x) && (at.x < (r.x + r.w));
}

// The overlap of two collinear axis-aligned segments, zero unless they meet in
// more than a point.
Wide run_shared(scav_point a, scav_point b, scav_point c, scav_point d) {
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

// `through`: segments entering a live box other than an endpoint or an ancestor of one;
// `back`: legs collinear with and opposite to the leg before.
void shape_counts(Laid const &l, uint32_t &through, uint32_t &back);

uint32_t state_named(Chart const &c, std::string_view name) {
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if (chart_string(c, c.states[st].name) == name) { return st; }
  }
  return INVALID;
}

// Indices of live states: the boxes routes avoid and end on.
std::vector<uint32_t> live_of(Chart const &c) {
  std::vector<uint32_t> live;
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if (c.states[st].live != 0) { live.push_back(st); }
  }
  return live;
}

void shape_counts(Laid const &l, uint32_t &through, uint32_t &back) {
  std::vector<uint32_t> const live{ live_of(l.c) };
  through = 0;
  back = 0;
  for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
    scav_span const route{ l.r.route[t] };
    Transition const &tr{ l.c.transitions[t] };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      scav_point const a{ l.r.points[route.off + k] };
      scav_point const b{ l.r.points[route.off + k + 1] };
      for (uint32_t const st : live) {
        if ((st == tr.src.v) || (st == tr.dst.v)) { continue; }
        if (ancestor(l.c, { st }, tr.src) || ancestor(l.c, { st }, tr.dst)) { continue; }
        through += enters(a, b, l.z.state[st]) ? 1U : 0U;
      }
    }
    for (uint32_t k = 0; (k + 2) < route.len; ++k) {
      scav_point const a{ l.r.points[route.off + k] };
      scav_point const b{ l.r.points[route.off + k + 1] };
      scav_point const c{ l.r.points[route.off + k + 2] };
      back += (((a.x == b.x) && (b.x == c.x) && ((b.y > a.y) == (b.y > c.y))) ||
               ((a.y == b.y) && (b.y == c.y) && ((b.x > a.x) == (b.x > c.x))))
                  ? 1U
                  : 0U;
    }
  }
}

// Counts route ends at a fork or join bar that are off its two long faces.
uint32_t capped_branches(Laid const &l) {
  uint32_t capped{ 0 };
  for (uint32_t st = 0; st < l.c.states.size(); ++st) {
    StateKind const kind{ l.c.states[st].kind };
    if ((kind != StateKind::Fork) && (kind != StateKind::Join)) { continue; }
    scav_rect const box{ l.z.state[st] };
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      scav_span const route{ l.r.route[t] };
      if (route.len < 2) { continue; }
      Transition const &tr{ l.c.transitions[t] };
      if (tr.src.v == st) { capped += on_long_face(l.r.points[route.off], box) ? 0U : 1U; }
      if (tr.dst.v == st) {
        capped += on_long_face(l.r.points[route.off + route.len - 1], box) ? 0U : 1U;
      }
    }
  }
  return capped;
}

}  // namespace

TEST_CASE("gauntlet: no element routes an edge through a box" *
          doctest::test_suite("full")) {
  for (char const *name : GAUNTLET) {
    for (scav_profile const &p : { readable(), compact() }) {
      CAPTURE(name);
      CAPTURE(p.profile_id);
      Laid l;
      lay(name, p, l);
      std::vector<uint32_t> const live{ live_of(l.c) };
      for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
        scav_span const route{ l.r.route[t] };
        Transition const &tr{ l.c.transitions[t] };
        for (uint32_t k = 0; (k + 1) < route.len; ++k) {
          scav_point const a{ l.r.points[route.off + k] };
          scav_point const b{ l.r.points[route.off + k + 1] };
          for (uint32_t const st : live) {
            // Endpoint boxes and their ancestors are exempt.
            if ((st == tr.src.v) || (st == tr.dst.v)) { continue; }
            if (ancestor(l.c, { st }, tr.src) || ancestor(l.c, { st }, tr.dst)) {
              continue;
            }
            CAPTURE(t);
            CAPTURE(st);
            CHECK_FALSE(enters(a, b, l.z.state[st]));
          }
        }
      }
    }
  }
}

TEST_CASE("gauntlet: every route is axis-aligned, forward, and reaches its ends" *
          doctest::test_suite("full")) {
  for (char const *name : GAUNTLET) {
    // `regions.scav` skips the reversal check; the open-shapes test pins its count.
    bool const open{ std::string_view{ name } == "regions.scav" };
    for (scav_profile const &p : { readable(), compact() }) {
      CAPTURE(name);
      CAPTURE(p.profile_id);
      Laid l;
      lay(name, p, l);
      for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
        CAPTURE(t);
        CHECK(l.r.failed[t] == 0);  // `lay` also checks `diags` is empty
        scav_span const route{ l.r.route[t] };
        if (l.g.trans_segments[t].len == 0) {
          CHECK(route.len == 0);
          continue;
        }
        REQUIRE(route.len >= 2);
        for (uint32_t k = 0; (k + 1) < route.len; ++k) {
          scav_point const a{ l.r.points[route.off + k] };
          scav_point const b{ l.r.points[route.off + k + 1] };
          CAPTURE(k);
          CHECK((a.x == b.x) != (a.y == b.y));  // axis-aligned and not a point
        }
        // Consecutive collinear legs run in the same direction.
        for (uint32_t k = 0; (!open) && ((k + 2) < route.len); ++k) {
          scav_point const a{ l.r.points[route.off + k] };
          scav_point const b{ l.r.points[route.off + k + 1] };
          scav_point const c{ l.r.points[route.off + k + 2] };
          CAPTURE(k);
          CHECK_FALSE(((a.x == b.x) && (b.x == c.x) && ((b.y > a.y) == (b.y > c.y))));
          CHECK_FALSE(((a.y == b.y) && (b.y == c.y) && ((b.x > a.x) == (b.x > c.x))));
        }
      }
    }
  }
}

TEST_CASE("gauntlet: an end on an inscribed glyph is at the middle of a face" *
          doctest::test_suite("full")) {
  // An inscribed glyph touches its box only at the four face midpoints.
  for (char const *name : GAUNTLET) {
    for (scav_profile const &p : { readable(), compact() }) {
      CAPTURE(name);
      CAPTURE(p.profile_id);
      Laid l;
      lay(name, p, l);
      for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
        scav_span const route{ l.r.route[t] };
        if (route.len < 2) { continue; }
        Transition const &tr{ l.c.transitions[t] };
        for (uint32_t const end : { 0U, 1U }) {
          StateId const of{ (end == 0) ? tr.src : tr.dst };
          if (!kind_inscribed(l.c.states[of.v].kind)) { continue; }
          scav_rect const box{ l.z.state[of.v] };
          scav_point const at{
            l.r.points[route.off + ((end == 0) ? 0 : (route.len - 1))]
          };
          if (!on_border(at, box)) { continue; }  // an inner face, exempt
          CAPTURE(t);
          CAPTURE(end);
          bool const middle{ ((at.x == box.x) || (at.x == (box.x + box.w)))
                                 ? (at.y == (box.y + floor_div(box.h, 2)))
                                 : (at.x == (box.x + floor_div(box.w, 2))) };
          CHECK(middle);
        }
      }
    }
  }
}

TEST_CASE("gauntlet: an arrowhead is never inked over another route's own end" *
          doctest::test_suite("full")) {
  // No route ends where another starts; two arrivals may share a point as a fan-in.
  for (char const *name : GAUNTLET) {
    for (scav_profile const &p : { readable(), compact() }) {
      CAPTURE(name);
      CAPTURE(p.profile_id);
      Laid l;
      lay(name, p, l);
      for (uint32_t a = 0; a < l.c.transitions.size(); ++a) {
        scav_span const one{ l.r.route[a] };
        if (one.len < 2) { continue; }
        for (uint32_t b = 0; b < l.c.transitions.size(); ++b) {
          scav_span const two{ l.r.route[b] };
          if ((a == b) || (two.len < 2)) { continue; }
          CAPTURE(a);
          CAPTURE(b);
          CHECK_FALSE(same(l.r.points[one.off + one.len - 1], l.r.points[two.off]));
        }
      }
    }
  }
}

TEST_CASE("gauntlet: a fork's bar is used along its length, not at one point") {
  // Each bar's ends sit at two or more distinct points on its long faces, and no arrival
  // shares a point with a departure.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("fork.scav", p, l);
    for (StateKind const kind : { StateKind::Fork, StateKind::Join }) {
      uint32_t bar{ INVALID };
      for (uint32_t st = 0; st < l.c.states.size(); ++st) {
        if (l.c.states[st].kind == kind) { bar = st; }
      }
      REQUIRE(bar != INVALID);
      CAPTURE(static_cast<uint32_t>(kind));
      scav_rect const box{ l.z.state[bar] };
      std::vector<scav_point> leaves;
      std::vector<scav_point> arrives;
      for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
        scav_span const route{ l.r.route[t] };
        if (route.len < 2) { continue; }
        Transition const &tr{ l.c.transitions[t] };
        if (tr.src.v == bar) { leaves.push_back(l.r.points[route.off]); }
        if (tr.dst.v == bar) { arrives.push_back(l.r.points[route.off + route.len - 1]); }
      }
      // One in and three out of the fork, three in and one out of the join.
      CHECK((leaves.size() + arrives.size()) == 4);
      for (scav_point const &a : arrives) {
        for (scav_point const &b : leaves) { CHECK_FALSE(same(a, b)); }
      }
      std::vector<scav_point> seats;
      for (std::vector<scav_point> const &side : { leaves, arrives }) {
        for (scav_point const &at : side) {
          // Only long-face ends count as seats; the open-shapes test counts cap ends.
          CHECK(on_border(at, box));
          if (!on_long_face(at, box)) { continue; }
          bool fresh{ true };
          for (scav_point const &had : seats) { fresh = fresh && !same(at, had); }
          if (fresh) { seats.push_back(at); }
        }
      }
      CHECK(seats.size() >= 2);
    }
  }
}

TEST_CASE("gauntlet: two states each other's target are two lines") {
  // `Up -> Down` and `Down -> Up` project onto one point of one face at each end.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("mutual.scav", p, l);
    uint32_t up{ INVALID };
    uint32_t down{ INVALID };
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      Transition const &tr{ l.c.transitions[t] };
      if (l.c.states[tr.src.v].name.len == 0) { continue; }
      std::string_view const from{ chart_string(l.c, l.c.states[tr.src.v].name) };
      std::string_view const to{ chart_string(l.c, l.c.states[tr.dst.v].name) };
      if ((from == "Up") && (to == "Down")) { up = t; }
      if ((from == "Down") && (to == "Up")) { down = t; }
    }
    REQUIRE(up != INVALID);
    REQUIRE(down != INVALID);
    scav_span const a{ l.r.route[up] };
    scav_span const b{ l.r.route[down] };
    CHECK_FALSE(same(l.r.points[a.off], l.r.points[b.off + b.len - 1]));
    CHECK_FALSE(same(l.r.points[b.off], l.r.points[a.off + a.len - 1]));
    for (uint32_t i = 0; i < a.len; ++i) {
      for (uint32_t j = 0; j < b.len; ++j) {
        CAPTURE(i);
        CAPTURE(j);
        CHECK_FALSE(same(l.r.points[a.off + i], l.r.points[b.off + j]));
      }
    }
    Wide shared{ 0 };  // collinear overlap of the two routes
    for (uint32_t i = 0; (i + 1) < a.len; ++i) {
      for (uint32_t j = 0; (j + 1) < b.len; ++j) {
        shared += run_shared(l.r.points[a.off + i],
                             l.r.points[a.off + i + 1],
                             l.r.points[b.off + j],
                             l.r.points[b.off + j + 1]);
      }
    }
    CHECK(shared == 0);
  }
}

TEST_CASE("gauntlet: a transition and its return are two straight legs an em apart") {
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("roundtrip.scav", p, l);
    uint32_t there{ INVALID };
    uint32_t back{ INVALID };
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      Transition const &tr{ l.c.transitions[t] };
      if (l.c.states[tr.src.v].name.len == 0) { continue; }
      std::string_view const from{ chart_string(l.c, l.c.states[tr.src.v].name) };
      std::string_view const to{ chart_string(l.c, l.c.states[tr.dst.v].name) };
      if ((from == "Ready") && (to == "Busy")) { there = t; }
      if ((from == "Busy") && (to == "Ready")) { back = t; }
    }
    REQUIRE(there != INVALID);
    REQUIRE(back != INVALID);
    scav_span const a{ l.r.route[there] };
    scav_span const b{ l.r.route[back] };
    REQUIRE(a.len == 2);
    REQUIRE(b.len == 2);
    scav_point const a0{ l.r.points[a.off] };
    scav_point const a1{ l.r.points[a.off + 1] };
    scav_point const b0{ l.r.points[b.off] };
    scav_point const b1{ l.r.points[b.off + 1] };
    bool const vertical{ a0.x == a1.x };
    CHECK(vertical == (b0.x == b1.x));
    Wide const gap{ vertical ? (Wide{ b0.x } - a0.x) : (Wide{ b0.y } - a0.y) };
    Wide const apart{ (gap < 0) ? -gap : gap };
    CAPTURE(apart);
    CHECK(apart >= p.font_size_grid);
    CHECK(cost_columns(l.c, l.g, p).crowding == 0);
  }
}

TEST_CASE("gauntlet: a fan-in's arrivals are four arrows, none inside another") {
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("fanin.scav", p, l);
    uint32_t fault{ INVALID };
    for (uint32_t st = 0; st < l.c.states.size(); ++st) {
      if (chart_string(l.c, l.c.states[st].name) == "Fault") { fault = st; }
    }
    REQUIRE(fault != INVALID);
    std::vector<uint32_t> into;
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      if ((l.r.route[t].len >= 2) && (l.c.transitions[t].dst.v == fault)) {
        into.push_back(t);
      }
    }
    REQUIRE(into.size() == 4);
    for (uint32_t const t : into) {
      scav_span const rt{ l.r.route[t] };
      CHECK(on_border(l.r.points[rt.off + rt.len - 1], l.z.state[fault]));
    }
    // A shared trunk is allowed; each other route covers less than a route's whole length.
    // A segment counts at most its own length as covered.
    for (uint32_t const t : into) {
      scav_span const a{ l.r.route[t] };
      Wide own{ 0 };
      for (uint32_t i = 0; (i + 1) < a.len; ++i) {
        own += run_shared(l.r.points[a.off + i],
                          l.r.points[a.off + i + 1],
                          l.r.points[a.off + i],
                          l.r.points[a.off + i + 1]);
      }
      for (uint32_t const u : into) {
        if (u == t) { continue; }
        scav_span const b{ l.r.route[u] };
        Wide covered{ 0 };
        for (uint32_t i = 0; (i + 1) < a.len; ++i) {
          scav_point const from{ l.r.points[a.off + i] };
          scav_point const to{ l.r.points[a.off + i + 1] };
          Wide under{ 0 };
          for (uint32_t j = 0; (j + 1) < b.len; ++j) {
            under +=
                run_shared(from, to, l.r.points[b.off + j], l.r.points[b.off + j + 1]);
          }
          covered += imin(under, run_shared(from, to, from, to));
        }
        CAPTURE(t);
        CAPTURE(u);
        CHECK(covered < own);
      }
    }
  }
}

TEST_CASE("gauntlet: an endpoint that is also a crossing is one point, not two" *
          doctest::test_suite("full")) {
  // A port slot on an endpoint's own border is that end's route point, except on an
  // external route, where the leg between them is a loop.
  for (char const *name : GAUNTLET) {
    for (scav_profile const &p : { readable(), compact() }) {
      CAPTURE(name);
      CAPTURE(p.profile_id);
      Laid l;
      lay(name, p, l);
      for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
        scav_span const route{ l.r.route[t] };
        scav_span const ports{ l.r.port[t] };
        if (route.len < 2) { continue; }
        Span const segs{ l.g.trans_segments[t] };
        Transition const &tr{ l.c.transitions[t] };
        // One slot per crossing, in segment order.
        REQUIRE(ports.len == (segs.len - 1));
        for (uint32_t k = 0; k < ports.len; ++k) {
          StateId const on{ l.g.ports[l.g.segments[segs.off + k].dst_port].state };
          scav_port_slot const slot{ l.r.slots[ports.off + k] };
          scav_point const at{ .x = slot.x, .y = slot.y };
          CAPTURE(t);
          CAPTURE(k);
          bool const loop{ tr.kind == TransKind::External };
          if (on == tr.src) { CHECK(same(l.r.points[route.off], at) != loop); }
          if (on == tr.dst) {
            CHECK(same(l.r.points[route.off + route.len - 1], at) != loop);
          }
        }
      }
    }
  }
}

TEST_CASE(
    "gauntlet: an out-of-machine label is drawn in the submachine holding both ends") {
  // The root alone holds both ends; the label sits within a leader of the route's root
  // leg, nearer it than the leg inside `Right`.
  for (scav_profile const &p :
       { readable(), compact(), one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    CAPTURE(p.portfolio_m);
    Laid bare;
    lay("crossing.scav", p, bare);
    uint32_t t{ INVALID };
    for (uint32_t k = 0; k < bare.c.transitions.size(); ++k) {
      if (bare.c.transitions[k].label.len != 0) { t = k; }
    }
    REQUIRE(t != INVALID);
    // Wider than `rank_sep` at either profile.
    scav_path_box const box{ .subject = t, .w = 1511, .h = 269, .order = 0 };
    scav_spaces const spaces{ .path_box = &box, .n_path_box = 1 };
    Laid l;
    lay("crossing.scav", p, l, spaces, nullptr);
    REQUIRE(l.r.placed.size() == 1);
    CHECK(l.unplaced == 0);
    scav_rect const at{ l.r.placed[0] };

    CommonAncestor const lca{ l.g.trans_common[t] };
    REQUIRE(lca.frame == l.c.root_submachine);
    CHECK(contains(l.z.sub[lca.frame.v], at));
    for (uint32_t const st : live_of(l.c)) {
      CAPTURE(st);
      CHECK_FALSE(overlaps(at, l.z.state[st]));
    }
    // A leg is the root's where its midpoint lies inside neither composite.
    scav_span const route{ l.r.route[t] };
    REQUIRE(route.len >= 2);
    Wide held_gap{ -1 };
    Wide inner_gap{ -1 };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      scav_point const a{ l.r.points[route.off + k] };
      scav_point const b{ l.r.points[route.off + k + 1] };
      scav_point const mid{ .x = a.x + floor_div(b.x - a.x, 2),
                            .y = a.y + floor_div(b.y - a.y, 2) };
      bool held{ true };
      for (StateId const side : lca.child) {
        if ((side.v != INVALID) && inside(mid, l.z.state[side.v])) { held = false; }
      }
      Wide const gap{ chebyshev_gap(at, span_rect(a, b)) };
      Wide &into{ held ? held_gap : inner_gap };
      into = (into < 0) ? gap : imin(into, gap);
    }
    REQUIRE(held_gap >= 0);
    REQUIRE(inner_gap >= 0);  // the piece inside `Right`
    CHECK(held_gap <= label_leader(p));
    CHECK(held_gap < inner_gap);
  }
}

TEST_CASE("gauntlet: a long edge's label widens no boundary another already widened") {
  // Both labels clear every state and each other; row 0 unsearched, which keeps the
  // chain's own ranks, is also held to the gap widths.
  for (scav_profile const &p :
       { readable(), compact(), one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    CAPTURE(p.portfolio_m);
    Laid bare;
    lay("long.scav", p, bare);
    std::array<uint32_t, 2> labelled{ INVALID, INVALID };
    uint32_t found{ 0 };
    for (uint32_t k = 0; (k < bare.c.transitions.size()) && (found < 2); ++k) {
      if (bare.c.transitions[k].label.len != 0) { labelled[found++] = k; }
    }
    REQUIRE(found == 2);
    int32_t const wide{ 3000 };
    int32_t const along{ 1500 };
    std::array<scav_path_box, 2> const boxes{
      scav_path_box{ .subject = labelled[0], .w = wide, .h = 269, .order = 0 },
      scav_path_box{ .subject = labelled[1], .w = along, .h = 269, .order = 0 },
    };
    scav_spaces const spaces{ .path_box = boxes.data(), .n_path_box = 2 };
    Laid l;
    lay("long.scav", p, l, spaces, nullptr);
    CHECK(l.unplaced == 0);
    REQUIRE(l.r.placed.size() == 2);
    for (scav_rect const &at : l.r.placed) {
      for (uint32_t const st : live_of(l.c)) {
        CAPTURE(st);
        CHECK_FALSE(overlaps(at, l.z.state[st]));
      }
    }
    CHECK_FALSE(overlaps(l.r.placed[0], l.r.placed[1]));
    if (p.portfolio_m != 1) { continue; }

    Transition const &first{ l.c.transitions[labelled[0]] };
    Transition const &across{ l.c.transitions[labelled[1]] };
    REQUIRE(first.src == across.src);
    uint32_t const from{ l.o.nodes[l.o.state_node[across.src.v]].rank };
    uint32_t const to{ l.o.nodes[l.o.state_node[across.dst.v]].rank };
    REQUIRE(l.o.nodes[l.o.state_node[first.dst.v]].rank == (from + 1));
    REQUIRE(to == (from + 3));
    Span const gaps{ l.o.sub_gaps[l.c.root_submachine.v] };
    REQUIRE(gaps.len >= to);
    CHECK(l.o.gaps[gaps.off + from] >= wide);
    for (uint32_t b = from + 1; b < to; ++b) {
      CAPTURE(b);
      CHECK(l.o.gaps[gaps.off + b] < along);
    }
  }
}

TEST_CASE("gauntlet: a chain of states turns only where the fold cuts it") {
  // Four boxes in a row fold into two rows; two of the edges step between rows.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("chain.scav", p, l);
    CHECK(l.lane_moves == 0);
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      CAPTURE(t);
      // An edge across the cut turns twice at most.
      CHECK(l.r.route[t].len <= 4);
    }
  }
}

TEST_CASE("gauntlet: a folded frame's second piece starts under the state entering it") {
  // `First -> Second` crosses the cut. Row 0 unsearched takes the scale
  // measure's fold.
  for (scav_profile const &p : { one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    Laid bare;
    lay("folded.scav", p, bare);
    uint32_t const first{ state_named(bare.c, "First") };
    uint32_t const second{ state_named(bare.c, "Second") };
    uint32_t const box{ state_named(bare.c, "Box") };
    REQUIRE(first != INVALID);
    REQUIRE(second != INVALID);
    REQUIRE(box != INVALID);
    uint32_t t{ INVALID };
    for (uint32_t k = 0; k < bare.c.transitions.size(); ++k) {
      if ((bare.c.transitions[k].src.v == first) &&
          (bare.c.transitions[k].dst.v == second)) {
        t = k;
      }
    }
    REQUIRE(t != INVALID);
    // A real-text title band on every normal state folds the run.
    std::vector<scav_box_space> titles(bare.c.states.size(), scav_box_space{});
    for (uint32_t st = 0; st < titles.size(); ++st) {
      if (bare.c.states[st].kind == StateKind::Normal) {
        titles[st] = { .min_w = 832, .h_before = 397, .h_after = 0 };
      }
    }
    scav_path_box const label{ .subject = t, .w = 1856, .h = 269, .order = 0 };
    scav_spaces const spaces{ .box_state = titles.data(),
                              .n_box_state = static_cast<uint32_t>(titles.size()),
                              .path_box = &label,
                              .n_path_box = 1 };
    Laid l;
    lay("folded.scav", p, l, spaces, nullptr);
    scav_rect const &above{ l.z.state[first] };
    scav_rect const &below{ l.z.state[second] };
    REQUIRE(below.y >= (above.y + above.h));
    CHECK(below.x == above.x);

    // One straight leg down, one arc in from both leading corners.
    scav_span const route{ l.r.route[t] };
    REQUIRE(route.len == 2);
    scav_point const from{ l.r.points[route.off] };
    scav_point const to{ l.r.points[route.off + 1] };
    CHECK(from.x == to.x);
    int32_t const arc{ imax(imax(state_corner_radius(StateKind::Normal, above, p.pad),
                                 state_corner_radius(StateKind::Normal, below, p.pad)),
                            1) };
    CHECK(from.x == (below.x + arc));

    // The label sits in the room sizing reserved, on the leg's trailing side.
    REQUIRE(l.r.placed.size() == 1);
    CHECK(l.unplaced == 0);
    scav_rect const at{ l.r.placed[0] };
    CHECK(at.x >= from.x);
    uint32_t const frame{ l.c.submachine_ids[l.c.states[box].submachines.off].v };
    CHECK(contains(l.z.sub[frame], at));
  }
}

TEST_CASE(
    "gauntlet: a run the scale measure folds is laid straight where that is smaller") {
  // Row 0 unsearched folds `watch`; at `readable` the search pins it unfolded, its states
  // on one row. At `compact` it turns both regions down.
  for (scav_profile const &p : { readable() }) {
    CAPTURE(p.profile_id);
    Laid bare;
    lay("unfolded.scav", one_row(p), bare);
    uint32_t const busy{ state_named(bare.c, "Busy") };
    REQUIRE(busy != INVALID);
    REQUIRE(bare.c.states[busy].submachines.len == 2);
    uint32_t const watch{
      bare.c.submachine_ids[bare.c.states[busy].submachines.off + 1].v
    };
    REQUIRE(bare.z.folded[watch] != 0);

    Laid l;
    lay("unfolded.scav", p, l);
    CHECK(l.z.folded[watch] == 0);
    CHECK(std::ranges::any_of(l.pins.folds, [watch](FoldPin const &f) {
      return (f.frame.v == watch) && (f.mode == FOLD_NEVER);
    }));
    int32_t const row{ l.z.state[state_named(l.c, "Fine")].y };
    for (char const *name : { "Late", "Lost" }) {
      CAPTURE(name);
      CHECK(l.z.state[state_named(l.c, name)].y == row);
    }
    CHECK((Wide{ l.z.chart.w } * l.z.chart.h) < (Wide{ bare.z.chart.w } * bare.z.chart.h));
  }
}

TEST_CASE("gauntlet: a port level with a child keeps its seat, and the initial's moves") {
  // Row 0 unsearched seats the port and the initial's dot level with `First`,
  // both projecting onto one point of its leading face.
  for (scav_profile const &p : { one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("ported.scav", p, l);
    uint32_t const first{ state_named(l.c, "First") };
    uint32_t const above{ state_named(l.c, "Above") };
    REQUIRE(first != INVALID);
    REQUIRE(above != INVALID);
    uint32_t dot{ INVALID };
    uint32_t ported{ INVALID };
    std::vector<uint32_t> into;
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      Transition const &tr{ l.c.transitions[t] };
      if (tr.dst.v != first) { continue; }
      into.push_back(t);
      if (l.c.states[tr.src.v].kind == StateKind::Initial) { dot = t; }
      if (tr.src.v == above) { ported = t; }
    }
    REQUIRE(into.size() == 3);
    REQUIRE(dot != INVALID);
    REQUIRE(ported != INVALID);
    auto const last = [&](uint32_t t) {
      scav_span const route{ l.r.route[t] };
      REQUIRE(route.len >= 2);
      return l.r.points[route.off + route.len - 1];
    };
    REQUIRE(l.r.port[ported].len == 1);
    scav_port_slot const &slot{ l.r.slots[l.r.port[ported].off] };
    CHECK(last(ported).y == slot.y);
    for (uint32_t const t : into) {
      if (t == ported) { continue; }
      CAPTURE(t);
      CHECK_FALSE(same(last(t), last(ported)));
    }
  }
}

TEST_CASE(
    "gauntlet: a composite is entered by one straight line from where it is entered") {
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("above.scav", p, l);
    uint32_t const source{ state_named(l.c, "Source") };
    uint32_t const middle{ state_named(l.c, "Middle") };
    REQUIRE(source != INVALID);
    REQUIRE(middle != INVALID);
    uint32_t drop{ INVALID };
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      if (l.c.transitions[t].src.v == source) { drop = t; }
    }
    REQUIRE(drop != INVALID);
    scav_span const route{ l.r.route[drop] };
    REQUIRE(route.len >= 2);
    scav_point const first{ l.r.points[route.off] };
    scav_point const last{ l.r.points[route.off + route.len - 1] };
    bool const vertical{ first.x == last.x };
    for (uint32_t k = 0; k < route.len; ++k) {
      scav_point const at{ l.r.points[route.off + k] };
      CAPTURE(k);
      CHECK((vertical ? (at.x == first.x) : (at.y == first.y)));
    }
    CHECK(on_border(first, l.z.state[source]));
    CHECK(on_border(last, l.z.state[middle]));
  }
}

TEST_CASE(
    "gauntlet: entered from directly above, a composite running across is entered on "
    "top") {
  // The root turned to run down puts `Source` over `Box`, whose frame runs across;
  // unsearched, the facing pass puts the port on the top border.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    SearchPins const seed{ .orients = { { .frame = SubmachineId{ 0 } } } };
    Laid l;
    lay("above.scav", one_row(p), l, {}, &seed);
    uint32_t const source{ state_named(l.c, "Source") };
    uint32_t const middle{ state_named(l.c, "Middle") };
    REQUIRE(source != INVALID);
    REQUIRE(middle != INVALID);
    REQUIRE(l.o.sub_down[0] == 1);
    REQUIRE(l.o.sub_down[l.c.states[middle].parent.v] == 0);
    uint32_t drop{ INVALID };
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      if (l.c.transitions[t].src.v == source) { drop = t; }
    }
    REQUIRE(drop != INVALID);
    REQUIRE(l.pins.ends.size() == 1);
    CHECK(l.pins.ends[0].trans.v == drop);
    CHECK(l.pins.ends[0].leg == 1);
    CHECK(l.pins.ends[0].end == 0);
    CHECK(l.pins.ends[0].face == 2);
    REQUIRE(l.r.port[drop].len == 1);
    CHECK(l.r.slots[l.r.port[drop].off].side == 2);

    scav_span const route{ l.r.route[drop] };
    REQUIRE(route.len >= 2);
    scav_rect const from{ l.z.state[source] };
    scav_rect const to{ l.z.state[middle] };
    scav_point const first{ l.r.points[route.off] };
    scav_point const last{ l.r.points[route.off + route.len - 1] };
    CHECK(first.y == (from.y + from.h));
    CHECK(last.y == to.y);
    for (uint32_t k = 0; k < route.len; ++k) {
      CAPTURE(k);
      CHECK(l.r.points[route.off + k].x == first.x);
    }
  }
}

TEST_CASE("gauntlet: a route through two nested borders crosses both at one height") {
  // Both frames run down and both ports sit on a left border, a cross border
  // whose port is placed along its frame's ranks. Unsearched.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid bare;
    lay("through.scav", one_row(p), bare);
    uint32_t const source{ state_named(bare.c, "Source") };
    uint32_t const outer{ state_named(bare.c, "Outer") };
    uint32_t const inner{ state_named(bare.c, "Inner") };
    uint32_t const target{ state_named(bare.c, "Target") };
    REQUIRE(source != INVALID);
    REQUIRE(outer != INVALID);
    REQUIRE(inner != INVALID);
    REQUIRE(target != INVALID);
    uint32_t reach{ INVALID };
    for (uint32_t t = 0; t < bare.c.transitions.size(); ++t) {
      if (bare.c.transitions[t].src.v == source) { reach = t; }
    }
    REQUIRE(reach != INVALID);
    auto const frame_of = [&](uint32_t st) {
      return bare.c.submachine_ids[bare.c.states[st].submachines.off];
    };
    SearchPins const seed{
      .ends = { { .trans = TransId{ reach }, .leg = 1, .end = 0, .face = 0 },
                { .trans = TransId{ reach }, .leg = 2, .end = 0, .face = 0 } },
      .orients = { { .frame = frame_of(outer) }, { .frame = frame_of(inner) } }
    };
    Laid l;
    lay("through.scav", one_row(p), l, {}, &seed);
    REQUIRE(l.o.sub_down[frame_of(outer).v] == 1);
    REQUIRE(l.o.sub_down[frame_of(inner).v] == 1);
    REQUIRE(l.r.port[reach].len == 2);
    for (uint32_t k = 0; k < 2; ++k) {
      CAPTURE(k);
      CHECK(l.r.slots[l.r.port[reach].off + k].side == 0);
    }
    scav_rect const in{ l.z.state[inner] };
    scav_rect const to{ l.z.state[target] };
    REQUIRE((to.y + (to.h / 2)) != (in.y + (in.h / 2)));

    scav_span const route{ l.r.route[reach] };
    REQUIRE(route.len >= 2);
    scav_point const first{ l.r.points[route.off] };
    scav_point const last{ l.r.points[route.off + route.len - 1] };
    for (uint32_t k = 0; k < route.len; ++k) {
      CAPTURE(k);
      CHECK(l.r.points[route.off + k].y == first.y);
    }
    CHECK(on_border(first, l.z.state[source]));
    CHECK(on_border(last, to));
  }
}

namespace {

// Pins for `carried.scav`: leg 1 of the entering route `enter` unchained, and `Box`'s run
// folded before `Third` only.
SearchPins carried_pins(Laid const &bare, uint32_t &enter) {
  uint32_t const left{ state_named(bare.c, "Left") };
  uint32_t const third{ state_named(bare.c, "Third") };
  REQUIRE(left != INVALID);
  REQUIRE(third != INVALID);
  enter = INVALID;
  for (uint32_t t = 0; t < bare.c.transitions.size(); ++t) {
    if ((bare.c.transitions[t].src.v == left) && (bare.c.transitions[t].dst.v == third)) {
      enter = t;
    }
  }
  REQUIRE(enter != INVALID);
  SearchPins pins{ .cuts = { { .trans = TransId{ enter }, .leg = 1 } } };
  SubmachineOrders const o{ order_submachines(bare.c, bare.g, {}, readable(), 0, pins) };
  uint32_t const frame{ bare.c.states[third].parent.v };
  pins.folds.push_back({ .frame = SubmachineId{ frame },
                         .mode = FOLD_ALWAYS,
                         .layer = o.nodes[o.state_node[third]].rank });
  return pins;
}

}  // namespace

TEST_CASE("gauntlet: a pinned cut before an entered state takes the port into its piece") {
  for (scav_profile const &p : { one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    Laid bare;
    lay("carried.scav", p, bare);
    uint32_t enter{ INVALID };
    SearchPins const pins{ carried_pins(bare, enter) };
    Laid l;
    lay("carried.scav", p, l, {}, &pins);
    uint32_t const first{ state_named(l.c, "First") };
    uint32_t const third{ state_named(l.c, "Third") };
    uint32_t const left{ state_named(l.c, "Left") };
    uint32_t const frame{ l.c.states[third].parent.v };
    REQUIRE(l.z.folded[frame] != 0);
    scav_rect const &top{ l.z.state[first] };
    scav_rect const &target{ l.z.state[third] };
    CHECK(target.y >= (top.y + top.h));  // in the second piece
    CHECK(target.x <= top.x);            // no further in than the frame's first state

    // Straight, and at most the gap between the two states plus a rank gap long.
    scav_span const route{ l.r.route[enter] };
    REQUIRE(route.len >= 2);
    Wide len{ 0 };
    for (uint32_t k = 1; k < route.len; ++k) {
      scav_point const a{ l.r.points[route.off + k - 1] };
      scav_point const b{ l.r.points[route.off + k] };
      CHECK(a.y == b.y);
      len += imax(Wide{ b.x } - a.x, Wide{ a.x } - b.x) +
             imax(Wide{ b.y } - a.y, Wide{ a.y } - b.y);
    }
    scav_rect const &source{ l.z.state[left] };
    CHECK(len <= ((Wide{ target.x } - (source.x + source.w)) + p.rank_sep));
  }
}

TEST_CASE("gauntlet: a cut before a boundary-fed state is taken, and carries the port") {
  Laid bare;
  lay("carried.scav", one_row(readable()), bare);
  uint32_t enter{ INVALID };
  SearchPins const pins{ carried_pins(bare, enter) };
  uint32_t const layer{ pins.folds[0].layer };
  scav_layout_opts o{ .profile = one_row(readable()), .router = 0, .threads = 1 };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  LayoutTrace t;
  trace_sink_set(&t);
  bool const ran{
    layout_run(bare.c, {}, o, placed, diags, nullptr, nullptr, 0, nullptr, nullptr, &pins)
  };
  trace_sink_set(nullptr);
  REQUIRE(ran);
  uint32_t taken{ 0 };
  uint32_t refused{ 0 };
  uint32_t carried{ 0 };
  for (TraceEvent const &e : t.events) {
    if (e.frame != pins.folds[0].frame.v) { continue; }
    if (e.kind == TraceKind::FoldCut) { ++((e.fold.refused != 0) ? refused : taken); }
    if ((e.kind == TraceKind::BoundaryCarried) && (e.carry.rank == layer)) { ++carried; }
  }
  CHECK(taken == 1);
  CHECK(refused == 0);
  CHECK(carried == 1);
}

TEST_CASE("gauntlet: a composite entered straight holds its first state a clearance in") {
  for (scav_profile const &p :
       { readable(), compact(), one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    CAPTURE(p.portfolio_m);
    Laid l;
    lay("entered.scav", p, l);
    uint32_t const outside{ state_named(l.c, "Outside") };
    uint32_t const box{ state_named(l.c, "Box") };
    uint32_t const first{ state_named(l.c, "First") };
    REQUIRE(outside != INVALID);
    REQUIRE(box != INVALID);
    REQUIRE(first != INVALID);
    scav_rect const outer{ l.z.state[box] };
    scav_rect const band{ l.z.before[box] };
    scav_rect const in{ l.z.state[first] };

    uint32_t t{ INVALID };
    for (uint32_t k = 0; k < l.c.transitions.size(); ++k) {
      Transition const &tr{ l.c.transitions[k] };
      if ((tr.src.v == outside) && (tr.dst.v == first)) { t = k; }
    }
    REQUIRE(t != INVALID);
    scav_span const route{ l.r.route[t] };
    REQUIRE(route.len >= 2);
    scav_point const from{ l.r.points[route.off] };
    bool across{ true };
    bool down{ true };
    for (uint32_t k = 1; k < route.len; ++k) {
      across = across && (l.r.points[route.off + k].y == from.y);
      down = down && (l.r.points[route.off + k].x == from.x);
    }
    CHECK((across || down));

    Wide gap{ 0 };  // from the inside of the crossed border to `First`
    if (from.x < outer.x) {
      gap = Wide{ in.x } - (outer.x + p.pad);
    } else if (from.x >= (outer.x + outer.w)) {
      gap = (Wide{ outer.x } + outer.w - p.pad) - (Wide{ in.x } + in.w);
    } else if (from.y < outer.y) {
      gap = Wide{ in.y } - (Wide{ band.y } + band.h);
    } else {
      gap = (Wide{ outer.y } + outer.h - p.pad) - (Wide{ in.y } + in.h);
    }
    CHECK(gap >= 0);
    CHECK(gap <= route_clearance(p));
  }
}

namespace {

// Checks that the transition leaving `from` runs straight from its border to `to`'s.
void straight_from(Laid const &l, uint32_t from, uint32_t to) {
  uint32_t t{ INVALID };
  for (uint32_t k = 0; k < l.c.transitions.size(); ++k) {
    if (l.c.transitions[k].src.v == from) { t = k; }
  }
  REQUIRE(t != INVALID);
  scav_span const route{ l.r.route[t] };
  REQUIRE(route.len >= 2);
  scav_point const first{ l.r.points[route.off] };
  scav_point const last{ l.r.points[route.off + route.len - 1] };
  bool across{ true };
  bool down{ true };
  for (uint32_t k = 1; k < route.len; ++k) {
    across = across && (l.r.points[route.off + k].y == first.y);
    down = down && (l.r.points[route.off + k].x == first.x);
  }
  CHECK((across || down));
  CHECK(on_border(first, l.z.state[from]));
  CHECK(on_border(last, l.z.state[to]));
}

}  // namespace

TEST_CASE("gauntlet: a port on a rank border sits level with the state it joins") {
  // `Source -> Box/Target` crosses `Box`'s leading border into its second rank
  // through a bend beside `First`; the port and the bend sit level with `Target`'s face.
  for (scav_profile const &p :
       { readable(), compact(), one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    CAPTURE(p.portfolio_m);
    Laid l;
    lay("level.scav", p, l);
    uint32_t const source{ state_named(l.c, "Source") };
    uint32_t const target{ state_named(l.c, "Target") };
    REQUIRE(source != INVALID);
    REQUIRE(target != INVALID);
    straight_from(l, source, target);
  }
}

TEST_CASE("gauntlet: a state beside a composite is centred on the port it enters") {
  // `Source`, ranked with `Box`, joins `Box/Target` through a port on `Box`'s
  // bottom border that sits level with `Target`, off `Box`'s centre.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid bare;
    lay("under.scav", one_row(p), bare);
    uint32_t const source{ state_named(bare.c, "Source") };
    uint32_t const box{ state_named(bare.c, "Box") };
    uint32_t const target{ state_named(bare.c, "Target") };
    REQUIRE(source != INVALID);
    REQUIRE(box != INVALID);
    REQUIRE(target != INVALID);
    uint32_t const rank{ bare.o.nodes[bare.o.state_node[box]].rank };
    SearchPins const seed{ .ranks = { { .state = StateId{ source }, .rank = rank } } };
    Laid l;
    lay("under.scav", one_row(p), l, {}, &seed);
    REQUIRE(l.o.nodes[l.o.state_node[source]].rank == l.o.nodes[l.o.state_node[box]].rank);
    scav_rect const from{ l.z.state[source] };
    scav_rect const to{ l.z.state[target] };
    scav_rect const around{ l.z.state[box] };
    REQUIRE(from.y >= (around.y + around.h));
    REQUIRE((to.x + (to.w / 2)) != (around.x + (around.w / 2)));
    straight_from(l, source, target);
  }
}

TEST_CASE("gauntlet: every state lies inside the frame it is drawn in" *
          doctest::test_suite("full")) {
  for (char const *name : GAUNTLET) {
    for (scav_profile const &p :
         { readable(), compact(), one_row(readable()), one_row(compact()) }) {
      CAPTURE(name);
      CAPTURE(p.profile_id);
      CAPTURE(p.portfolio_m);
      Laid l;
      lay(name, p, l);
      for (uint32_t const st : live_of(l.c)) {
        CAPTURE(chart_string(l.c, l.c.states[st].name));
        CHECK(contains(l.z.sub[l.c.states[st].parent.v], l.z.state[st]));
      }
    }
  }
}

TEST_CASE("gauntlet: a pseudostate seated in a layer leaves the layers before it alone") {
  // `* -> T` seats beside `T`; `B` is at most a rank gap past `A`, however wide `T` is.
  for (scav_profile const &p : { one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("pulled.scav", p, l);
    uint32_t const a{ state_named(l.c, "A") };
    uint32_t const b{ state_named(l.c, "B") };
    REQUIRE(a != INVALID);
    REQUIRE(b != INVALID);
    scav_rect const from{ l.z.state[a] };
    scav_rect const to{ l.z.state[b] };
    CHECK(to.x <= (from.x + from.w + p.rank_sep));
  }
}

TEST_CASE("gauntlet: a run's arrows each span one gap, not the drawing") {
  // Each route is at most the widest state plus `rank_sep` long.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("stretch.scav", p, l);
    int32_t widest{ 0 };
    for (uint32_t const st : live_of(l.c)) { widest = imax(widest, l.z.state[st].w); }
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      scav_span const rt{ l.r.route[t] };
      Wide length{ 0 };
      for (uint32_t k = 0; (k + 1) < rt.len; ++k) {
        scav_point const a{ l.r.points[rt.off + k] };
        scav_point const b{ l.r.points[rt.off + k + 1] };
        length +=
            Wide{ imax(a.x, b.x) - imin(a.x, b.x) } + (imax(a.y, b.y) - imin(a.y, b.y));
      }
      CAPTURE(t);
      CHECK(length <= (Wide{ widest } + p.rank_sep));
    }
  }
}

TEST_CASE("gauntlet: a route passing through a composite bends outside it" *
          doctest::test_suite("full")) {
  // A bend strictly inside `Arm` and outside `Moving` is in a state the route
  // only passes through; the leg across `Arm` is straight at either weight.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    for (int32_t const weight : { 0, p.w_transit_bends }) {
      CAPTURE(weight);
      scav_profile priced{ p };
      priced.w_transit_bends = weight;
      Laid l;
      lay("transit.scav", priced, l);
      uint32_t const gripping{ state_named(l.c, "Gripping") };
      uint32_t const arm{ state_named(l.c, "Arm") };
      uint32_t const moving{ state_named(l.c, "Moving") };
      REQUIRE(gripping != INVALID);
      REQUIRE(arm != INVALID);
      REQUIRE(moving != INVALID);
      uint32_t extend{ INVALID };
      for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
        if (l.c.transitions[t].src.v == gripping) { extend = t; }
      }
      REQUIRE(extend != INVALID);
      scav_span const route{ l.r.route[extend] };
      REQUIRE(route.len >= 2);
      uint32_t in_arm{ 0 };
      for (uint32_t k = 1; (k + 1) < route.len; ++k) {
        scav_point const a{ l.r.points[route.off + k - 1] };
        scav_point const b{ l.r.points[route.off + k] };
        scav_point const c{ l.r.points[route.off + k + 1] };
        bool const straight{ ((a.x == b.x) && (b.x == c.x)) ||
                             ((a.y == b.y) && (b.y == c.y)) };
        if (!straight && strictly_inside(b, l.z.state[arm]) &&
            !strictly_inside(b, l.z.state[moving])) {
          ++in_arm;
        }
      }
      CHECK(in_arm == 0U);
    }
  }
}

TEST_CASE("gauntlet: priced whitespace takes the composite drawn tighter") {
  // Box's chain runs loose or tight; priced, the search keeps the drawing that leaves
  // less of Box empty, and neither drawing bends or crosses.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    std::array<int64_t, 2> empty{};
    for (int32_t const weight : { 0, 1 }) {
      CAPTURE(weight);
      scav_profile priced{ p };
      priced.w_whitespace = weight;
      Laid l;
      lay("tight.scav", priced, l);
      CostTerms const t{ cost_terms(l.c, l.g, l.z, l.r, {}, priced) };
      CHECK(t.bends == 0);
      CHECK(t.crossings == 0);
      empty[static_cast<uint32_t>(weight)] = t.whitespace;
    }
    CHECK(empty[1] < empty[0]);
  }
}

TEST_CASE("gauntlet: a route leaving a composite turns one corner into its target") {
  // Each route out of Box onto Fault leaves along its port's lead and turns at most once,
  // onto the vertical leg that enters Fault's top or bottom face.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("corner.scav", p, l);
    uint32_t const fault{ state_named(l.c, "Fault") };
    REQUIRE(fault != INVALID);
    for (char const *name : { "First", "Second" }) {
      CAPTURE(name);
      uint32_t const from{ state_named(l.c, name) };
      REQUIRE(from != INVALID);
      uint32_t leaving{ INVALID };
      for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
        Transition const &tr{ l.c.transitions[t] };
        if ((tr.src.v == from) && (tr.dst.v == fault)) { leaving = t; }
      }
      REQUIRE(leaving != INVALID);
      scav_span const route{ l.r.route[leaving] };
      REQUIRE(route.len >= 2);
      uint32_t turns{ 0 };
      for (uint32_t k = 1; (k + 1) < route.len; ++k) {
        scav_point const a{ l.r.points[route.off + k - 1] };
        scav_point const b{ l.r.points[route.off + k] };
        scav_point const c{ l.r.points[route.off + k + 1] };
        bool const straight{ ((a.x == b.x) && (b.x == c.x)) ||
                             ((a.y == b.y) && (b.y == c.y)) };
        turns += straight ? 0U : 1U;
      }
      CHECK(turns <= 1U);
      if (turns == 0U) { continue; }
      scav_point const last{ l.r.points[route.off + route.len - 1] };
      scav_point const before{ l.r.points[route.off + route.len - 2] };
      scav_rect const box{ l.z.state[fault] };
      CHECK(before.x == last.x);
      CHECK(((last.y == box.y) || (last.y == (box.y + box.h))));
    }
  }
}

namespace {

// Gauntlet chart `name`, loaded only; tests index space requests by its states.
Chart loaded(char const *name) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/gauntlet/" };
  path += name;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  Chart c;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
  return c;
}

scav_spaces spaces_of(std::vector<scav_box_space> const &box) {
  return { .box_state = box.data(),
           .n_box_state = static_cast<uint32_t>(box.size()),
           .box_state_stride = static_cast<uint32_t>(sizeof(scav_box_space)) };
}

// Whether any segment of any route enters any nonempty wall of any state: its sealed
// bands, and its loop room for every route but its own inner loops.
bool any_band_entered(Laid const &l) {
  for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
    scav_span const route{ l.r.route[t] };
    bool const loop{ inner_loop(l.c, t) };
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      scav_point const a{ l.r.points[route.off + k] };
      scav_point const b{ l.r.points[route.off + k + 1] };
      for (uint32_t st = 0; st < l.c.states.size(); ++st) {
        std::array<scav_rect, 5> const walls{ state_walls(l.z, st) };
        bool const own{ loop && (l.c.transitions[t].src.v == st) };
        for (uint32_t w = 0; w < (own ? 4U : 5U); ++w) {
          scav_rect const &wall{ walls[w] };
          if ((wall.w > 0) && (wall.h > 0) && enters(a, b, wall)) { return true; }
        }
      }
    }
  }
  return false;
}

// State `st`'s interior inside its ring, less its four bands.
scav_rect free_interior(SizedLayout const &z, uint32_t st) {
  int32_t const x0{ z.lead[st].x + z.lead[st].w };
  int32_t const y0{ z.before[st].y + z.before[st].h };
  return { .x = x0, .y = y0, .w = z.trail[st].x - x0, .h = z.after[st].y - y0 };
}

// Inner loop `t`: both ends on its room's exit face of the free interior (the border where
// no band lines that side, the band's inner edge where one does), its far leg inside the
// free interior, its room in the placement's corner, and no band of its state entered.
void loop_lands(Laid const &l, uint32_t t) {
  uint32_t const st{ l.c.transitions[t].src.v };
  LoopPlace const at{ loop_place(l.z, st) };
  CAPTURE(at.face);
  CAPTURE(at.end);
  scav_rect const &box{ l.z.state[st] };
  std::array<scav_rect, 4> const band{ l.z.lead[st],
                                       l.z.trail[st],
                                       l.z.before[st],
                                       l.z.after[st] };
  std::array<int32_t, 4> const border{ box.x, box.x + box.w, box.y, box.y + box.h };
  std::array<int32_t, 4> const inner{ band[0].x + band[0].w,
                                      band[1].x,
                                      band[2].y + band[2].h,
                                      band[3].y };
  bool const vertical{ at.face >= 2 };
  bool const lined{ vertical ? (band[at.face].h > 0) : (band[at.face].w > 0) };
  int32_t const edge{ lined ? inner[at.face] : border[at.face] };
  scav_span const route{ l.r.route[t] };
  REQUIRE(route.len == 4);
  scav_point const *const pt{ l.r.points.data() + route.off };
  CHECK((vertical ? pt[0].y : pt[0].x) == edge);
  CHECK((vertical ? pt[3].y : pt[3].x) == edge);
  CHECK((vertical ? (pt[0].x < pt[3].x) : (pt[0].y < pt[3].y)));
  scav_rect const hole{ free_interior(l.z, st) };
  for (uint32_t k = 1; k < 3; ++k) { CHECK(strictly_inside(pt[k], hole)); }
  scav_rect const &room{ l.z.loop[st] };
  bool const low_x{ (at.face == 0) || (vertical && (at.end == 0)) };
  bool const low_y{ (at.face == 2) || (!vertical && (at.end == 0)) };
  CHECK((low_x ? (room.x == hole.x) : ((room.x + room.w) == (hole.x + hole.w))));
  CHECK((low_y ? (room.y == hole.y) : ((room.y + room.h) == (hole.y + hole.h))));
  std::array<scav_rect, 5> const walls{ state_walls(l.z, st) };
  for (uint32_t k = 0; k < 3; ++k) {
    for (uint32_t w = 0; w < 4; ++w) {
      scav_rect const &wall{ walls[w] };
      if ((wall.w > 0) && (wall.h > 0)) { CHECK_FALSE(enters(pt[k], pt[k + 1], wall)); }
    }
  }
}

// One `w` by `h` box per labelled transition of `c`.
std::vector<scav_path_box> label_boxes(Chart const &c, int32_t w, int32_t h) {
  std::vector<scav_path_box> out;
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if (c.transitions[t].label.len != 0) {
      out.push_back({ .subject = t, .w = w, .h = h, .order = 0 });
    }
  }
  return out;
}

uint32_t from_named(Chart const &c, uint32_t src) {
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if (c.transitions[t].src.v == src) { return t; }
  }
  return INVALID;
}

}  // namespace

TEST_CASE("gauntlet: a header walls its face, so the port from above moves off it") {
  // `above` with its root turned to run down puts Source over Box; a header on Box's top
  // sends the port to another face.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Chart const probe{ loaded("above.scav") };
    uint32_t const box{ state_named(probe, "Box") };
    REQUIRE(box != INVALID);
    std::vector<scav_box_space> rows(probe.states.size());
    rows[box].h_before = 4 * p.font_size_grid;
    scav_spaces const s{ spaces_of(rows) };
    SearchPins const seed{ .orients = { { .frame = SubmachineId{ 0 } } } };
    Laid l;
    lay("above.scav", one_row(p), l, s, &seed);
    uint32_t const drop{ from_named(l.c, state_named(l.c, "Source")) };
    uint32_t const middle{ state_named(l.c, "Middle") };
    REQUIRE(drop != INVALID);
    REQUIRE(l.r.port[drop].len == 1);
    CHECK(l.r.slots[l.r.port[drop].off].side != 2);
    CHECK_FALSE(any_band_entered(l));
    CHECK(cost_terms(l.c, l.g, l.z, l.r, s, one_row(p)).through_band == 0);
    scav_span const route{ l.r.route[drop] };
    REQUIRE(route.len >= 2);
    CHECK(on_border(l.r.points[route.off + route.len - 1], l.z.state[middle]));
  }
}

TEST_CASE("gauntlet: a port takes the one face no band lines") {
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Chart const probe{ loaded("above.scav") };
    uint32_t const box{ state_named(probe, "Box") };
    REQUIRE(box != INVALID);
    std::vector<scav_box_space> rows(probe.states.size());
    rows[box] = { .min_w = 0,
                  .h_before = 2 * p.font_size_grid,
                  .h_after = 0,
                  .w_before = 2 * p.font_size_grid,
                  .w_after = 2 * p.font_size_grid };
    scav_spaces const s{ spaces_of(rows) };
    Laid l;
    lay("above.scav", one_row(p), l, s, nullptr);
    uint32_t const drop{ from_named(l.c, state_named(l.c, "Source")) };
    REQUIRE(drop != INVALID);
    REQUIRE(l.r.port[drop].len == 1);
    CHECK(l.r.slots[l.r.port[drop].off].side == 3);
    CHECK_FALSE(any_band_entered(l));
    CHECK(cost_terms(l.c, l.g, l.z, l.r, s, one_row(p)).through_band == 0);
  }
}

TEST_CASE("gauntlet: a route into a state walled on every face crosses its band square") {
  // `above` with a band on each of Box's faces: the drop stays orthogonal and pays Tier 0.
  scav_router_id id{};
  REQUIRE(router_by_name(reinterpret_cast<scav_byte const *>("orthogonal"), 10, id));
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Chart const probe{ loaded("above.scav") };
    uint32_t const box{ state_named(probe, "Box") };
    REQUIRE(box != INVALID);
    int32_t const band{ 2 * p.font_size_grid };
    std::vector<scav_box_space> rows(probe.states.size());
    rows[box] = { .min_w = 0,
                  .h_before = band,
                  .h_after = band,
                  .w_before = band,
                  .w_after = band };
    scav_spaces const s{ spaces_of(rows) };
    Laid l;
    lay("above.scav", one_row(p), l, s, nullptr);
    CHECK(l.r.degraded() == 0);
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      CAPTURE(t);
      scav_span const route{ l.r.route[t] };
      CHECK(route.len >= 2);
      for (uint32_t k = 0; (k + 1) < route.len; ++k) {
        scav_point const a{ l.r.points[route.off + k] };
        scav_point const b{ l.r.points[route.off + k + 1] };
        CHECK(((a.x == b.x) || (a.y == b.y)));
      }
    }
    CHECK(cost_terms(l.c, l.g, l.z, l.r, s, one_row(p)).through_band > 0);

    Chart c{ loaded("above.scav") };
    scav_layout_opts const o{ .profile = one_row(p), .router = id, .threads = 1 };
    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    LayoutTrace trace;
    trace_sink_set(&trace);
    bool const ran{ layout_run(c, s, o, placed, diags) };
    trace_sink_set(nullptr);
    REQUIRE(ran);
    CHECK(std::ranges::count_if(trace.events, [](TraceEvent const &e) {
            return e.kind == TraceKind::RouteWalled;
          }) > 0);
  }
}

TEST_CASE("gauntlet: a state walled on every face is still drawn, and pays for the wall") {
  // Every face of every composite carries a band: layout succeeds, draws every transition,
  // and Tier 0 counts the band crossings.
  scav_router_id id{};
  REQUIRE(router_by_name(reinterpret_cast<scav_byte const *>("orthogonal"), 10, id));
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    for (char const *name : { "above.scav", "mixed.scav", "header.scav" }) {
      CAPTURE(name);
      Chart c{ loaded(name) };
      std::vector<scav_box_space> rows(c.states.size());
      for (uint32_t st = 0; st < c.states.size(); ++st) {
        if (c.states[st].submachines.len == 0) { continue; }
        rows[st] = { .min_w = 0,
                     .h_before = p.font_size_grid,
                     .h_after = p.font_size_grid,
                     .w_before = p.font_size_grid,
                     .w_after = p.font_size_grid };
      }
      scav_spaces const s{ spaces_of(rows) };
      std::vector<scav_placed> placed;
      std::vector<Diagnostic> diags;
      scav_layout_opts const o{ .profile = p, .router = id, .threads = 0 };
      REQUIRE(layout_run(c, s, o, placed, diags));
      ColumnId const routes{ column_find(c, "scav.geom.route") };
      REQUIRE(routes.v != INVALID);
      for (uint32_t t = 0; t < c.transitions.size(); ++t) {
        scav_span route{};
        std::memcpy(&route,
                    column_data(c, routes) + (size_t{ t } * sizeof(scav_span)),
                    sizeof(scav_span));
        CAPTURE(t);
        CHECK(route.len >= 2);
      }
      CHECK(cost_columns(c, decompose(c), p, s, placed).through_band > 0);
    }
  }
}

TEST_CASE("gauntlet: with every state headed, no route enters a band" *
          doctest::test_suite("full")) {
  for (char const *name : GAUNTLET) {
    for (scav_profile const &p : { readable(), compact() }) {
      std::string const chart{ name };
      CAPTURE(chart);
      CAPTURE(p.profile_id);
      Chart const probe{ loaded(name) };
      std::vector<scav_box_space> rows(probe.states.size());
      for (uint32_t st = 0; st < probe.states.size(); ++st) {
        if (probe.states[st].kind == StateKind::Normal) {
          rows[st].h_before = 2 * p.font_size_grid;
        }
      }
      scav_spaces const s{ spaces_of(rows) };
      Laid l;
      lay(name, p, l, s, nullptr);
      CHECK_FALSE(any_band_entered(l));
      CHECK(cost_terms(l.c, l.g, l.z, l.r, s, p).through_band == 0);
      for (scav_port_slot const &slot : l.r.slots) {
        CAPTURE(slot.x);
        CAPTURE(slot.y);
        bool headed_top{ false };  // on a headed state's top border; a region's is free
        for (uint32_t st = 0; st < l.c.states.size(); ++st) {
          scav_rect const r{ l.z.state[st] };
          headed_top =
              headed_top || ((rows[st].h_before > 0) && (slot.side == 2) &&
                             (slot.y == r.y) && (slot.x > r.x) && (slot.x < (r.x + r.w)));
        }
        CHECK_FALSE(headed_top);
      }
    }
  }
}

TEST_CASE("gauntlet: an internal loop stays inside its state, under its header") {
  // Each loop leaves its room's exit face and returns to it, every corner strictly inside
  // the free interior, clear of the header and the children, its label inside.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Chart const probe{ loaded("inloop.scav") };
    std::vector<scav_box_space> rows(probe.states.size());
    std::vector<scav_path_box> boxes;
    for (uint32_t st = 0; st < probe.states.size(); ++st) {
      if (probe.states[st].kind == StateKind::Normal) {
        rows[st].h_before = 2 * p.font_size_grid;
      }
    }
    for (uint32_t t = 0; t < probe.transitions.size(); ++t) {
      if (probe.transitions[t].label.len == 0) { continue; }
      boxes.push_back(
          { .subject = t, .w = 4 * p.font_size_grid, .h = p.font_size_grid, .order = 0 });
    }
    scav_spaces s{ spaces_of(rows) };
    s.path_box = boxes.data();
    s.n_path_box = static_cast<uint32_t>(boxes.size());
    s.path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box));
    Laid l;
    lay("inloop.scav", p, l, s, nullptr);
    uint32_t loops{ 0 };
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      Transition const &tr{ l.c.transitions[t] };
      if ((tr.src != tr.dst) || (tr.kind == TransKind::Default)) { continue; }
      CAPTURE(t);
      ++loops;
      loop_lands(l, t);
      scav_rect const hole{ free_interior(l.z, tr.src.v) };
      scav_span const route{ l.r.route[t] };
      scav_point const *const pt{ l.r.points.data() + route.off };
      for (uint32_t st = 0; st < l.c.states.size(); ++st) {
        if ((st == tr.src.v) || !ancestor(l.c, tr.src, { st })) { continue; }
        for (uint32_t k = 0; k < 3; ++k) {
          CHECK_FALSE(enters(pt[k], pt[k + 1], l.z.state[st]));
        }
      }
      for (uint32_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].subject != t) { continue; }
        scav_rect const &label{ l.r.placed[i] };
        CHECK(label.x >= hole.x);
        CHECK((label.x + label.w) <= (hole.x + hole.w));
        CHECK(label.y >= hole.y);
        CHECK((label.y + label.h) <= (hole.y + hole.h));
      }
    }
    CHECK(loops == 4);
    CHECK_FALSE(any_band_entered(l));
  }
}

TEST_CASE(
    "gauntlet: a pinned loop room lands its legs on its face's border or band edge") {
  // `room` with each of the eight placements pinned: unbanded, with all four bands, and
  // with all four bands ruled; a band's edge with no rule counts in Tier 0.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    for (uint32_t const mode : { 0U, 1U, 2U }) {
      CAPTURE(mode);
      bool const banded{ mode != 0 };
      Chart const probe{ loaded("room.scav") };
      uint32_t const idle{ state_named(probe, "Idle") };
      REQUIRE(idle != INVALID);
      std::vector<scav_box_space> rows(probe.states.size());
      if (banded) {
        rows[idle].w_before = 2 * p.font_size_grid;
        rows[idle].w_after = 2 * p.font_size_grid;
        rows[idle].h_before = 2 * p.font_size_grid;
        rows[idle].h_after = 2 * p.font_size_grid;
        rows[idle].ruled = (mode == 2) ? 0xFU : 0U;
      }
      std::vector<scav_path_box> const boxes{
        label_boxes(probe, 3 * p.font_size_grid, p.font_size_grid)
      };
      scav_spaces s{ spaces_of(rows) };
      s.path_box = boxes.data();
      s.n_path_box = static_cast<uint32_t>(boxes.size());
      s.path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box));
      for (uint32_t k = 0; k < 8; ++k) {
        CAPTURE(k);
        SearchPins const seed{
          .loops = { { .state = StateId{ idle }, .face = k / 2, .end = k % 2 } }
        };
        Laid l;
        lay("room.scav", one_row(p), l, s, &seed);
        REQUIRE(l.z.loop_place.size() > idle);
        CHECK(l.z.loop_place[idle] == k);
        scav_rect const &box{ l.z.state[idle] };
        std::array<int32_t, 4> const border{ box.x, box.x + box.w, box.y, box.y + box.h };
        CHECK((loop_boundary(l.z, idle, k / 2) != border[k / 2]) == banded);
        for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
          if (inner_loop(l.c, t)) { loop_lands(l, t); }
        }
        CHECK_FALSE(any_band_entered(l));
        CostTerms const t{ cost_terms(l.c, l.g, l.z, l.r, s, one_row(p)) };
        CHECK(t.loop_unanchored == ((mode == 1) ? 1 : 0));
        CHECK(cost_of(t, one_row(p)).t0_violations == t.loop_unanchored);
      }
    }
  }
}

TEST_CASE("gauntlet: a loop pinned to a ruled top band's face leaves the rule") {
  // A ruled top band and the top-face placement: the legs end on the band's bottom edge.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Chart const probe{ loaded("room.scav") };
    uint32_t const idle{ state_named(probe, "Idle") };
    REQUIRE(idle != INVALID);
    std::vector<scav_box_space> rows(probe.states.size());
    rows[idle].h_before = 2 * p.font_size_grid;
    rows[idle].ruled = 1;
    std::vector<scav_path_box> const boxes{
      label_boxes(probe, 3 * p.font_size_grid, p.font_size_grid)
    };
    scav_spaces s{ spaces_of(rows) };
    s.path_box = boxes.data();
    s.n_path_box = static_cast<uint32_t>(boxes.size());
    s.path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box));
    SearchPins const seed{ .loops = {
                               { .state = StateId{ idle }, .face = 2, .end = 1 } } };
    Laid l;
    lay("room.scav", one_row(p), l, s, &seed);
    uint32_t const poll{ from_named(l.c, idle) };
    REQUIRE(poll != INVALID);
    REQUIRE(inner_loop(l.c, poll));
    scav_rect const &head{ l.z.before[idle] };
    scav_span const route{ l.r.route[poll] };
    REQUIRE(route.len == 4);
    scav_point const *const pt{ l.r.points.data() + route.off };
    CHECK(pt[0].y == (head.y + head.h));
    CHECK(pt[3].y == (head.y + head.h));
    CHECK(pt[1].y > (head.y + head.h));
    loop_lands(l, poll);
    CHECK_FALSE(any_band_entered(l));
    CHECK(
        cost_of(cost_terms(l.c, l.g, l.z, l.r, s, one_row(p)), one_row(p)).t0_violations ==
        0);
  }
}

namespace {

// `room`'s spaces: Idle's row `idle_box`, a three-em label per loop.
struct RoomSpaces {
  std::vector<scav_box_space> rows;
  std::vector<scav_path_box> boxes;
  scav_spaces s{};
};

uint32_t room_spaces(scav_profile const &p,
                     scav_box_space const &idle_box,
                     RoomSpaces &out) {
  Chart const probe{ loaded("room.scav") };
  uint32_t const idle{ state_named(probe, "Idle") };
  REQUIRE(idle != INVALID);
  out.rows.assign(probe.states.size(), scav_box_space{});
  out.rows[idle] = idle_box;
  out.boxes = label_boxes(probe, 3 * p.font_size_grid, p.font_size_grid);
  out.s = spaces_of(out.rows);
  out.s.path_box = out.boxes.data();
  out.s.n_path_box = static_cast<uint32_t>(out.boxes.size());
  out.s.path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box));
  return idle;
}

}  // namespace

TEST_CASE("gauntlet: a loop never takes the face of a band with no rule") {
  // A name band with no rule: neither the unpinned placement nor a search takes the top
  // face, and a search under a wide unruled band turns its loops to the bottom face.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    RoomSpaces sp;
    uint32_t const idle{ room_spaces(p, { .h_before = 2 * p.font_size_grid }, sp) };
    for (scav_profile const &run : { one_row(p), p }) {
      Laid l;
      lay("room.scav", run, l, sp.s, nullptr);
      CHECK(loop_place(l.z, idle).face != 2);
      CHECK(cost_terms(l.c, l.g, l.z, l.r, sp.s, run).loop_unanchored == 0);
    }
  }
  scav_profile const p{ readable() };
  Chart const probe{ loaded("rooms.scav") };
  uint32_t const busy{ state_named(probe, "Busy") };
  REQUIRE(busy != INVALID);
  std::vector<scav_box_space> rows(probe.states.size());
  rows[busy].h_before = 2 * p.font_size_grid;
  rows[busy].min_w = 100 * p.font_size_grid;
  std::vector<scav_path_box> const boxes{
    label_boxes(probe, 2 * p.font_size_grid, p.font_size_grid)
  };
  scav_spaces s{ spaces_of(rows) };
  s.path_box = boxes.data();
  s.n_path_box = static_cast<uint32_t>(boxes.size());
  s.path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box));
  Laid searched;
  lay("rooms.scav", p, searched, s, nullptr);
  CHECK(loop_place(searched.z, busy).face == 3);
  CHECK_FALSE(std::ranges::any_of(searched.pins.loops, [&](LoopPin const &pin) {
    return (pin.state.v == busy) && (pin.face == 2);
  }));
  CHECK(cost_of(cost_terms(searched.c, searched.g, searched.z, searched.r, s, p), p)
            .t0_violations == 0);
}

TEST_CASE("gauntlet: a loop pinned to the face of a band with no rule counts in Tier 0") {
  // The pin holds: the legs end on the band's edge, and the loop counts once.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    RoomSpaces sp;
    uint32_t const idle{ room_spaces(p, { .h_before = 2 * p.font_size_grid }, sp) };
    SearchPins const seed{ .loops = {
                               { .state = StateId{ idle }, .face = 2, .end = 1 } } };
    Laid l;
    lay("room.scav", one_row(p), l, sp.s, &seed);
    REQUIRE(l.z.loop_place.size() > idle);
    CHECK(l.z.loop_place[idle] == 5);
    uint32_t const poll{ from_named(l.c, idle) };
    REQUIRE(poll != INVALID);
    loop_lands(l, poll);
    CostTerms const t{ cost_terms(l.c, l.g, l.z, l.r, sp.s, one_row(p)) };
    CHECK(t.loop_unanchored == 1);
    CHECK(cost_of(t, one_row(p)).t0_violations == 1);
  }
}

TEST_CASE(
    "gauntlet: a loop with every face banded and none ruled takes the default face") {
  // No face is eligible: the unpinned placement and a search both lay out, on the right
  // face, and Tier 0 counts the loop.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    int32_t const band{ 2 * p.font_size_grid };
    RoomSpaces sp;
    uint32_t const idle{ room_spaces(
        p,
        { .h_before = band, .h_after = band, .w_before = band, .w_after = band },
        sp) };
    for (scav_profile const &run : { one_row(p), p }) {
      Laid l;
      lay("room.scav", run, l, sp.s, nullptr);
      CHECK(loop_place(l.z, idle).face == 1);
      uint32_t const poll{ from_named(l.c, idle) };
      REQUIRE(poll != INVALID);
      loop_lands(l, poll);
      CHECK(cost_terms(l.c, l.g, l.z, l.r, sp.s, run).loop_unanchored == 1);
    }
  }
}

TEST_CASE(
    "gauntlet: the search turns a loop room under a wide description to save height") {
  // `rooms` under a wide top band: three loops side by side take less height than three
  // rows, so the searched drawing pins a top or bottom face.
  scav_profile const p{ readable() };
  Chart const probe{ loaded("rooms.scav") };
  uint32_t const busy{ state_named(probe, "Busy") };
  REQUIRE(busy != INVALID);
  std::vector<scav_box_space> rows(probe.states.size());
  rows[busy].h_before = 2 * p.font_size_grid;
  rows[busy].min_w = 100 * p.font_size_grid;
  std::vector<scav_path_box> const boxes{
    label_boxes(probe, 2 * p.font_size_grid, p.font_size_grid)
  };
  scav_spaces s{ spaces_of(rows) };
  s.path_box = boxes.data();
  s.n_path_box = static_cast<uint32_t>(boxes.size());
  s.path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box));
  Laid searched;
  lay("rooms.scav", p, searched, s, nullptr);
  CHECK(loop_place(searched.z, busy).face >= 2);
  CHECK(std::ranges::any_of(searched.pins.loops,
                            [&](LoopPin const &pin) { return pin.state.v == busy; }));
  SearchPins const stacked{ .loops = {
                                { .state = StateId{ busy }, .face = 1, .end = 1 } } };
  Laid unturned;
  lay("rooms.scav", one_row(p), unturned, s, &stacked);
  CHECK(searched.z.state[busy].h < unturned.z.state[busy].h);
  for (uint32_t t = 0; t < searched.c.transitions.size(); ++t) {
    if (inner_loop(searched.c, t)) { loop_lands(searched, t); }
  }
  CHECK(cost_of(cost_terms(searched.c, searched.g, searched.z, searched.r, s, p), p)
            .t0_violations == 0);
}

TEST_CASE("gauntlet: an external self-loop leaves its state and returns to it outside") {
  // `tick` is a loop off one face of Waiting: both ends on its border at least a clearance
  // apart, and every corner outside it.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("loop.scav", p, l);
    uint32_t const waiting{ state_named(l.c, "Waiting") };
    REQUIRE(waiting != INVALID);
    uint32_t tick{ INVALID };
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      Transition const &tr{ l.c.transitions[t] };
      if ((tr.src.v == waiting) && (tr.dst.v == waiting)) { tick = t; }
    }
    REQUIRE(tick != INVALID);
    scav_rect const &box{ l.z.state[waiting] };
    scav_span const route{ l.r.route[tick] };
    REQUIRE(route.len >= 4);
    scav_point const *const pt{ l.r.points.data() + route.off };
    scav_point const first{ pt[0] };
    scav_point const last{ pt[route.len - 1] };
    CHECK(on_border(first, box));
    CHECK(on_border(last, box));
    Wide const apart{ imax(imax(Wide{ first.x } - last.x, Wide{ last.x } - first.x),
                           imax(Wide{ first.y } - last.y, Wide{ last.y } - first.y)) };
    CHECK(apart >= route_clearance(p));
    for (uint32_t k = 1; (k + 1) < route.len; ++k) {
      CAPTURE(k);
      CHECK_FALSE(strictly_inside(pt[k], box));
    }
  }
}

TEST_CASE("gauntlet: an inner or outer loop's label lies within a leader of its route") {
  // Boxes sized like real text: 0.55 em a character plus `pad` wide, one line tall.
  struct Loops {
    char const *name;
    uint32_t inner, outer;
  };
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    for (Loops const &want : { Loops{ .name = "inloop.scav", .inner = 4, .outer = 0 },
                               Loops{ .name = "loop.scav", .inner = 1, .outer = 1 } }) {
      CAPTURE(want.name);
      Chart const probe{ loaded(want.name) };
      std::vector<scav_path_box> boxes;
      for (uint32_t t = 0; t < probe.transitions.size(); ++t) {
        int32_t const chars{ static_cast<int32_t>(probe.transitions[t].label.len) };
        if (chars == 0) { continue; }
        boxes.push_back({ .subject = t,
                          .w = ((chars * p.font_size_grid * 11) / 20) + p.pad,
                          .h = label_line_height(p),
                          .order = 0 });
      }
      scav_spaces const s{ .path_box = boxes.data(),
                           .n_path_box = static_cast<uint32_t>(boxes.size()),
                           .path_box_stride =
                               static_cast<uint32_t>(sizeof(scav_path_box)) };
      Laid l;
      lay(want.name, p, l, s, nullptr);
      REQUIRE(l.r.placed.size() == boxes.size());
      uint32_t inner{ 0 };
      uint32_t outer{ 0 };
      for (uint32_t i = 0; i < boxes.size(); ++i) {
        uint32_t const t{ boxes[i].subject };
        if (l.c.transitions[t].src != l.c.transitions[t].dst) { continue; }
        CAPTURE(t);
        ++(inner_loop(l.c, t) ? inner : outer);
        scav_span const route{ l.r.route[t] };
        REQUIRE(route.len >= 2);
        Wide gap{ -1 };
        for (uint32_t k = 0; (k + 1) < route.len; ++k) {
          Wide const away{ chebyshev_gap(
              l.r.placed[i],
              span_rect(l.r.points[route.off + k], l.r.points[route.off + k + 1])) };
          gap = (gap < 0) ? away : imin(gap, away);
        }
        CHECK(gap <= label_leader(p));
      }
      CHECK(inner == want.inner);
      CHECK(outer == want.outer);
      CHECK(cost_terms(l.c, l.g, l.z, l.r, s, p).label_far == 0);
    }
  }
}

namespace {

// The Chebyshev gap from `box` to the nearest leg of route `t`.
Wide gap_to_route(Laid const &l, uint32_t t, scav_rect const &box) {
  scav_span const route{ l.r.route[t] };
  Wide gap{ -1 };
  for (uint32_t k = 0; (k + 1) < route.len; ++k) {
    Wide const away{ chebyshev_gap(
        box,
        span_rect(l.r.points[route.off + k], l.r.points[route.off + k + 1])) };
    gap = (gap < 0) ? away : imin(gap, away);
  }
  return gap;
}

}  // namespace

TEST_CASE(
    "gauntlet: an inner loop's label lies within a leader at a pad wider than an em") {
  for (scav_profile base : { readable(), compact() }) {
    base.pad = 3 * base.font_size_grid;
    REQUIRE(profile_validate(base));
    CAPTURE(base.profile_id);
    Chart const probe{ loaded("inloop.scav") };
    std::vector<scav_path_box> boxes;
    for (uint32_t t = 0; t < probe.transitions.size(); ++t) {
      if (probe.transitions[t].label.len == 0) { continue; }
      boxes.push_back({ .subject = t,
                        .w = 4 * base.font_size_grid,
                        .h = label_line_height(base),
                        .order = 0 });
    }
    scav_spaces const s{ .path_box = boxes.data(),
                         .n_path_box = static_cast<uint32_t>(boxes.size()),
                         .path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box)) };
    Laid l;
    lay("inloop.scav", base, l, s, nullptr);
    REQUIRE(l.r.placed.size() == boxes.size());
    uint32_t loops{ 0 };
    for (uint32_t i = 0; i < boxes.size(); ++i) {
      if (!inner_loop(l.c, boxes[i].subject)) { continue; }
      CAPTURE(boxes[i].subject);
      ++loops;
      Wide const gap{ gap_to_route(l, boxes[i].subject, l.r.placed[i]) };
      CHECK(gap >= 0);
      CHECK(gap <= label_leader(base));
    }
    CHECK(loops == 4);
    CHECK(cost_terms(l.c, l.g, l.z, l.r, s, base).label_far == 0);
  }
}

TEST_CASE("gauntlet: every box of an inner loop's three-box label lies within a leader") {
  // Three boxes per label, each two lines tall: the stack is taller than the least lane.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Chart const probe{ loaded("inloop.scav") };
    std::vector<scav_path_box> boxes;
    for (uint32_t t = 0; t < probe.transitions.size(); ++t) {
      if (probe.transitions[t].label.len == 0) { continue; }
      for (uint32_t k = 0; k < 3; ++k) {
        boxes.push_back({ .subject = t,
                          .w = (2 + static_cast<int32_t>(k)) * p.font_size_grid,
                          .h = 2 * label_line_height(p),
                          .order = k });
      }
    }
    scav_spaces const s{ .path_box = boxes.data(),
                         .n_path_box = static_cast<uint32_t>(boxes.size()),
                         .path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box)) };
    Laid l;
    lay("inloop.scav", p, l, s, nullptr);
    REQUIRE(l.r.placed.size() == boxes.size());
    uint32_t stacked{ 0 };
    for (uint32_t i = 0; i < boxes.size(); ++i) {
      if (!inner_loop(l.c, boxes[i].subject)) { continue; }
      CAPTURE(boxes[i].subject);
      CAPTURE(boxes[i].order);
      ++stacked;
      Wide const gap{ gap_to_route(l, boxes[i].subject, l.r.placed[i]) };
      CHECK(gap >= 0);
      CHECK(gap <= label_leader(p));
    }
    CHECK(stacked == 12);
    CHECK(cost_terms(l.c, l.g, l.z, l.r, s, p).label_far == 0);
  }
}

TEST_CASE("gauntlet: crossings into decorated composites keep clear of every band") {
  // Headers everywhere and footers on composites: ports take side faces, nothing enters
  // a band, and an internal transition into a composite's depth starts on its border.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Chart const probe{ loaded("mixed.scav") };
    std::vector<scav_box_space> rows(probe.states.size());
    for (uint32_t st = 0; st < probe.states.size(); ++st) {
      if (probe.states[st].kind != StateKind::Normal) { continue; }
      rows[st].h_before = 2 * p.font_size_grid;
      if (probe.states[st].submachines.len != 0) { rows[st].h_after = p.font_size_grid; }
    }
    scav_spaces const s{ spaces_of(rows) };
    Laid l;
    lay("mixed.scav", p, l, s, nullptr);
    CHECK_FALSE(any_band_entered(l));
    CHECK(cost_terms(l.c, l.g, l.z, l.r, s, p).through_band == 0);
    for (scav_port_slot const &slot : l.r.slots) { CHECK(slot.side < 2); }
    uint32_t const left{ state_named(l.c, "Left") };
    uint32_t const core{ state_named(l.c, "Core") };
    REQUIRE(left != INVALID);
    REQUIRE(core != INVALID);
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      Transition const &tr{ l.c.transitions[t] };
      if ((tr.src.v != left) || (tr.dst.v != core)) { continue; }
      scav_span const route{ l.r.route[t] };
      REQUIRE(route.len >= 2);
      CHECK(on_border(l.r.points[route.off], l.z.state[left]));
      CHECK(on_border(l.r.points[route.off + route.len - 1], l.z.state[core]));
    }
  }
}

namespace {

// The one transition from `src` to `dst`, by state name.
uint32_t between(Chart const &c, std::string_view src, std::string_view dst) {
  uint32_t const a{ state_named(c, src) };
  uint32_t const b{ state_named(c, dst) };
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if ((c.transitions[t].src.v == a) && (c.transitions[t].dst.v == b)) { return t; }
  }
  return INVALID;
}

// `inward` with Box's frame running down under a header, and a footer where `footer` is
// set; no route runs flush along a box or enters a wall.
void inward_down(scav_profile const &p, bool footer, Laid &l, uint32_t &frame) {
  Chart const probe{ loaded("inward.scav") };
  uint32_t const box{ state_named(probe, "Box") };
  REQUIRE(box != INVALID);
  REQUIRE(probe.states[box].submachines.len == 1);
  frame = probe.submachine_ids[probe.states[box].submachines.off].v;
  std::vector<scav_box_space> rows(probe.states.size());
  rows[box].h_before = 2 * p.font_size_grid;
  rows[box].h_after = footer ? p.font_size_grid : 0;
  scav_spaces const s{ spaces_of(rows) };
  SearchPins const seed{ .orients = { { .frame = SubmachineId{ frame } } } };
  lay("inward.scav", one_row(p), l, s, &seed);
  REQUIRE(l.o.sub_down[frame] != 0);
  CostTerms const t{ cost_terms(l.c, l.g, l.z, l.r, s, one_row(p)) };
  CHECK(t.flush == 0);
  CHECK(t.through_band == 0);
  CHECK_FALSE(any_band_entered(l));
}

}  // namespace

TEST_CASE("gauntlet: an inner-face end turns off a header onto the unlined rank face") {
  // Box's frame runs down under a header: `reset` starts on Box's bottom and `restart`
  // ends off its top.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    uint32_t frame{ INVALID };
    inward_down(p, false, l, frame);
    uint32_t const box{ state_named(l.c, "Box") };
    uint32_t const reset{ between(l.c, "Box", "Child") };
    uint32_t const restart{ between(l.c, "Other", "Box") };
    REQUIRE(reset != INVALID);
    REQUIRE(restart != INVALID);
    scav_rect const &b{ l.z.state[box] };
    scav_point const first{ l.r.points[l.r.route[reset].off] };
    CHECK(first.y == (b.y + b.h));
    CHECK(first.x > b.x);
    CHECK(first.x < (b.x + b.w));
    scav_span const back{ l.r.route[restart] };
    REQUIRE(back.len >= 2);
    scav_point const last{ l.r.points[back.off + back.len - 1] };
    CHECK(on_border(last, b));
    CHECK(last.y != b.y);
  }
}

TEST_CASE("gauntlet: an inner-face end on a lined face stays on its frame's edge") {
  // A header and a footer line both rank faces of Box's down-running frame; each end sits
  // on the frame's edge, off Box's border.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    uint32_t frame{ INVALID };
    inward_down(p, true, l, frame);
    uint32_t const box{ state_named(l.c, "Box") };
    scav_rect const &f{ l.z.sub[frame] };
    for (uint32_t const t :
         { between(l.c, "Box", "Child"), between(l.c, "Other", "Box") }) {
      REQUIRE(t != INVALID);
      CAPTURE(t);
      scav_span const route{ l.r.route[t] };
      REQUIRE(route.len >= 2);
      bool const leaves{ l.c.transitions[t].src.v == box };
      scav_point const end{ l.r.points[route.off + (leaves ? 0 : (route.len - 1))] };
      CHECK(((end.y == f.y) || (end.y == (f.y + f.h))));
      CHECK_FALSE(on_border(end, l.z.state[box]));
    }
  }
}

TEST_CASE("gauntlet: an inner-face end beside a sibling region sits on the separator") {
  // Row 0 sets the two regions side by side: `light up` starts in the second from the
  // gap between them and `stall` ends in it from the first, each on the divider's line.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("separator.scav", one_row(p), l);
    uint32_t const running{ state_named(l.c, "Running") };
    REQUIRE(running != INVALID);
    REQUIRE(l.c.states[running].submachines.len == 2);
    Span const subs{ l.c.states[running].submachines };
    scav_rect const &motor{ l.z.sub[l.c.submachine_ids[subs.off].v] };
    scav_rect const &light{ l.z.sub[l.c.submachine_ids[subs.off + 1].v] };
    REQUIRE((motor.x + motor.w) < light.x);
    int32_t const divider{ (motor.x + motor.w) + ((light.x - (motor.x + motor.w)) / 2) };
    uint32_t const up{ between(l.c, "Running", "Lit") };
    uint32_t const stall{ between(l.c, "Stopped", "Running") };
    REQUIRE(up != INVALID);
    REQUIRE(stall != INVALID);
    scav_point const first{ l.r.points[l.r.route[up].off] };
    scav_span const route{ l.r.route[stall] };
    REQUIRE(route.len >= 2);
    scav_point const last{ l.r.points[route.off + route.len - 1] };
    for (scav_point const end : { first, last }) {
      CHECK(end.x == divider);
      CHECK(end.y > imax(motor.y, light.y));
      CHECK(end.y < imin(motor.y + motor.h, light.y + light.h));
    }
  }
}

TEST_CASE(
    "gauntlet: a port is reported walled only when every face it could take is lined") {
  // `drop` enters Box: a header alone leaves it another face, and all four lined leave
  // it none.
  scav_router_id id{};
  REQUIRE(router_by_name(reinterpret_cast<scav_byte const *>("orthogonal"), 10, id));
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    for (uint32_t const lined : { 0U, 1U, 4U }) {
      CAPTURE(lined);
      Chart c{ loaded("header.scav") };
      uint32_t const box{ state_named(c, "Box") };
      REQUIRE(box != INVALID);
      std::vector<scav_box_space> rows(c.states.size());
      int32_t const band{ p.font_size_grid };
      rows[box] = { .min_w = 0,
                    .h_before = (lined > 0) ? band : 0,
                    .h_after = (lined > 1) ? band : 0,
                    .w_before = (lined > 1) ? band : 0,
                    .w_after = (lined > 1) ? band : 0 };
      scav_spaces const s{ spaces_of(rows) };
      scav_layout_opts const o{ .profile = one_row(p), .router = id, .threads = 1 };
      std::vector<scav_placed> placed;
      std::vector<Diagnostic> diags;
      LayoutTrace trace;
      trace_sink_set(&trace);
      bool const ran{ layout_run(c, s, o, placed, diags) };
      trace_sink_set(nullptr);
      REQUIRE(ran);
      uint32_t walled{ 0 };
      for (TraceEvent const &e : trace.events) {
        if (e.kind == TraceKind::PortWalled) { ++walled; }
      }
      CHECK((walled > 0) == (lined == 4));
    }
  }
}

TEST_CASE("gauntlet: the shapes still open, counted rather than excused") {
  // Each carve-out from the properties above, counted: a rising count is a regression.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);

    // Row 0 unsearched lays the chart out flat; divider ports on the regions' sides make
    // three routes double back, and none passes through a box.
    Laid row_zero;
    lay("regions.scav", one_row(p), row_zero);
    REQUIRE(row_zero.tuple == 0);
    uint32_t through{ 0 };
    uint32_t back{ 0 };
    shape_counts(row_zero, through, back);
    CHECK(through == 0);
    CHECK(back == 3);
    // The scorer's through-box count over the run's columns agrees.
    CHECK(cost_columns(row_zero.c, row_zero.g, p).through_box == 0);

    // The shipped drawing, counted here and by the scorer: no route passes through a box
    // or doubles back.
    Laid shipped;
    lay("regions.scav", p, shipped);
    uint32_t shipped_through{ 0 };
    uint32_t shipped_back{ 0 };
    shape_counts(shipped, shipped_through, shipped_back);
    CHECK(shipped_through == 0);
    CHECK(cost_columns(shipped.c, shipped.g, p).through_box == 0);
    CHECK(shipped_back == 0);

    // The face rule picks by per-axis separation: at row 0 one branch stacked below a bar
    // leaves through its short cap.
    Laid fork_zero;
    lay("fork.scav", one_row(p), fork_zero);
    CHECK(capped_branches(fork_zero) == 1);
    // Shipped, one arrival enters the join through its cap.
    Laid fork_shipped;
    lay("fork.scav", p, fork_shipped);
    CHECK(capped_branches(fork_shipped) == 1);
  }
}

namespace {

// True when `entered` lies at least as near as each of `others` to the face of `box` the
// route into `entered` crosses.
bool nearest_its_port(Laid const &l,
                      uint32_t box,
                      uint32_t entered,
                      std::initializer_list<uint32_t> others) {
  scav_rect const b{ l.z.state[box] };
  uint32_t into{ INVALID };
  for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
    if (l.c.transitions[t].dst.v == entered) { into = t; }
  }
  REQUIRE(into != INVALID);
  scav_span const route{ l.r.route[into] };
  auto const gap = [&](scav_point at, scav_rect const &r) {
    if (at.x == b.x) { return r.x - b.x; }
    if (at.x == (b.x + b.w)) { return (b.x + b.w) - (r.x + r.w); }
    if (at.y == b.y) { return r.y - b.y; }
    return (b.y + b.h) - (r.y + r.h);
  };
  for (uint32_t k = 0; k < route.len; ++k) {
    scav_point const at{ l.r.points[route.off + k] };
    if (!on_border(at, b)) { continue; }
    int32_t const near{ gap(at, l.z.state[entered]) };
    CAPTURE(at.x);
    CAPTURE(at.y);
    CAPTURE(near);
    return std::ranges::all_of(others,
                               [&](uint32_t o) { return near <= gap(at, l.z.state[o]); });
  }
  FAIL("the route never meets the composite's border");
  return false;
}

}  // namespace

TEST_CASE("gauntlet: a state entered only through a port sits at the port's end") {
  // `Outside -> Box/Resumed` enters `Box` through its trailing face; `Resumed`, joined to
  // nothing else, is the state nearest that face, with `Box`'s ranks across or down.
  for (scav_profile const &p : { readable(), compact() }) {
    for (bool const down : { false, true }) {
      CAPTURE(p.profile_id);
      CAPTURE(down);
      Chart const probe{ loaded("resumed.scav") };
      uint32_t const box{ state_named(probe, "Box") };
      REQUIRE(box != INVALID);
      SearchPins seed;
      for (uint32_t m = 0; m < probe.submachines.size(); ++m) {
        if (down && (probe.submachines[m].owner.v == box)) {
          seed.orients.push_back({ .frame = SubmachineId{ m } });
        }
      }
      Laid l;
      lay("resumed.scav", one_row(p), l, {}, &seed);
      uint32_t const resumed{ state_named(l.c, "Resumed") };
      uint32_t const first{ state_named(l.c, "First") };
      uint32_t const second{ state_named(l.c, "Second") };
      REQUIRE(resumed != INVALID);
      CHECK(nearest_its_port(l, box, resumed, { first, second }));
    }
  }
}

TEST_CASE("gauntlet: a cycle member entered through a port sits on the port's side") {
  // `Outside -> Box/Entered` enters the cycle `First -> Entered -> Third -> First` through
  // `Box`'s trailing face; `Entered` is the cycle member nearest that face.
  for (scav_profile const &p :
       { readable(), compact(), one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    CAPTURE(p.portfolio_m);
    Laid l;
    lay("ring.scav", p, l);
    uint32_t const box{ state_named(l.c, "Box") };
    uint32_t const entered{ state_named(l.c, "Entered") };
    uint32_t const first{ state_named(l.c, "First") };
    uint32_t const third{ state_named(l.c, "Third") };
    REQUIRE(entered != INVALID);
    CHECK(nearest_its_port(l, box, entered, { first, third }));
  }
}

TEST_CASE("gauntlet: two arrivals along one side of a composite never share a run") {
  // `Right -> Box/Upper` and `Left -> Box/Lower`, both ports pinned to `Box`'s right face.
  for (scav_profile const &p : { one_row(readable()), one_row(compact()) }) {
    CAPTURE(p.profile_id);
    Laid bare;
    lay("side.scav", p, bare);
    uint32_t const box{ state_named(bare.c, "Box") };
    uint32_t const upper{ state_named(bare.c, "Upper") };
    REQUIRE(upper != INVALID);
    SearchPins seed;
    std::vector<uint32_t> in;
    for (uint32_t t = 0; t < bare.c.transitions.size(); ++t) {
      Transition const &tr{ bare.c.transitions[t] };
      if (!ancestor(bare.c, StateId{ box }, tr.dst) || (tr.src.v == upper)) { continue; }
      seed.ends.push_back({ .trans = TransId{ t }, .leg = 1, .end = 0, .face = 1 });
      in.push_back(t);
    }
    REQUIRE(in.size() == 2);
    Laid l;
    lay("side.scav", p, l, {}, &seed);
    scav_rect const frame{ l.z.state[box] };
    for (uint32_t const t : in) {
      scav_span const r{ l.r.route[t] };
      bool ported{ false };  // a point on `Box`'s right face
      for (uint32_t k = 0; k < r.len; ++k) {
        scav_point const at{ l.r.points[r.off + k] };
        ported = ported || (on_border(at, frame) && (at.x == (frame.x + frame.w)));
      }
      CHECK(ported);
    }
    scav_span const a{ l.r.route[in[0]] };
    scav_span const b{ l.r.route[in[1]] };
    Wide shared{ 0 };
    for (uint32_t i = 0; (i + 1) < a.len; ++i) {
      for (uint32_t j = 0; (j + 1) < b.len; ++j) {
        shared += run_shared(l.r.points[a.off + i],
                             l.r.points[a.off + i + 1],
                             l.r.points[b.off + j],
                             l.r.points[b.off + j + 1]);
      }
    }
    CHECK(shared == 0);
    CHECK(cost_terms(l.c, l.g, l.z, l.r, {}, p).shared_run == 0);
  }
}

namespace {

// Transition `t`'s route against state `st`'s box, as the runs it makes inside (`I`) and
// outside (`O`); an end at `st` itself counts as inside.
std::string runs_against(Laid const &l, uint32_t t, uint32_t st) {
  scav_rect const &box{ l.z.state[st] };
  scav_span const route{ l.r.route[t] };
  Transition const &tr{ l.c.transitions[t] };
  std::string out;
  auto const add = [&out](char side) {
    if ((side != 'B') && (out.empty() || (out.back() != side))) { out += side; }
  };
  Wide const x0{ Wide{ box.x } * 2 };
  Wide const y0{ Wide{ box.y } * 2 };
  Wide const x1{ (Wide{ box.x } + box.w) * 2 };
  Wide const y1{ (Wide{ box.y } + box.h) * 2 };
  auto const side_of = [&](Wide x2, Wide y2) {  // doubled coordinates; `B` on the border
    if ((x2 > x0) && (x2 < x1) && (y2 > y0) && (y2 < y1)) { return 'I'; }
    if ((x2 < x0) || (x2 > x1) || (y2 < y0) || (y2 > y1)) { return 'O'; }
    return 'B';
  };
  if (tr.src.v == st) { add('I'); }
  for (uint32_t k = 0; k < route.len; ++k) {
    scav_point const a{ l.r.points[route.off + k] };
    add(side_of(Wide{ a.x } * 2, Wide{ a.y } * 2));
    if ((k + 1) == route.len) { break; }
    scav_point const b{ l.r.points[route.off + k + 1] };
    add(side_of(Wide{ a.x } + b.x, Wide{ a.y } + b.y));
  }
  if (tr.dst.v == st) { add('I'); }
  return out;
}

// True when transition `t`'s route enters the gap between two live regions of state `st`.
bool crosses_divider(Laid const &l, uint32_t t, uint32_t st) {
  scav_rect const &box{ l.z.state[st] };
  scav_span const route{ l.r.route[t] };
  Span const subs{ l.c.states[st].submachines };
  for (uint32_t i = 0; i < subs.len; ++i) {
    for (uint32_t j = 0; j < subs.len; ++j) {
      uint32_t const m{ l.c.submachine_ids[subs.off + i].v };
      uint32_t const o{ l.c.submachine_ids[subs.off + j].v };
      if ((m == o) || (l.c.submachines[m].live == 0) || (l.c.submachines[o].live == 0)) {
        continue;
      }
      scav_rect const &a{ l.z.sub[m] };
      scav_rect const &b{ l.z.sub[o] };
      scav_rect gap{};
      if (b.x >= (a.x + a.w)) {
        gap = { .x = a.x + a.w, .y = box.y, .w = b.x - (a.x + a.w), .h = box.h };
      } else if (b.y >= (a.y + a.h)) {
        gap = { .x = box.x, .y = a.y + a.h, .w = box.w, .h = b.y - (a.y + a.h) };
      } else {
        continue;
      }
      for (uint32_t k = 0; (k + 1) < route.len; ++k) {
        scav_point const p{ l.r.points[route.off + k] };
        scav_point const q{ l.r.points[route.off + k + 1] };
        if ((imin(p.x, q.x) < (gap.x + gap.w)) && (imax(p.x, q.x) > gap.x) &&
            (imin(p.y, q.y) < (gap.y + gap.h)) && (imax(p.y, q.y) > gap.y)) {
          return true;
        }
      }
    }
  }
  return false;
}

// How far transition `t`'s route reaches outside state `st`'s box.
int32_t reach_outside(Laid const &l, uint32_t t, uint32_t st) {
  scav_rect const &box{ l.z.state[st] };
  scav_span const route{ l.r.route[t] };
  int32_t out{ 0 };
  for (uint32_t k = 0; k < route.len; ++k) {
    scav_point const a{ l.r.points[route.off + k] };
    out = imax(out,
               imax(imax(box.x - a.x, a.x - (box.x + box.w)),
                    imax(box.y - a.y, a.y - (box.y + box.h))));
  }
  return out;
}

struct OutAndBack {
  char const *chart;
  char const *state;  // the composite the external route leaves and re-enters
  char const *src;
  char const *dst;
  TransKind inside;  // the kind that keeps the route inside `state`
};

constexpr std::array OUT_AND_BACK{
  OutAndBack{ .chart = "reentry.scav",
              .state = "Outer",
              .src = "Outer",
              .dst = "Other",
              .inside = TransKind::Internal },
  OutAndBack{ .chart = "rebound.scav",
              .state = "Outer",
              .src = "Other",
              .dst = "Outer",
              .inside = TransKind::Default },
  OutAndBack{ .chart = "detour.scav",
              .state = "Outer",
              .src = "A",
              .dst = "B",
              .inside = TransKind::Default },
  OutAndBack{ .chart = "bypass.scav",
              .state = "Outer",
              .src = "A",
              .dst = "B",
              .inside = TransKind::Default },
  OutAndBack{ .chart = "headed.scav",
              .state = "Box",
              .src = "Idle",
              .dst = "Work",
              .inside = TransKind::Default },
};

// `name`'s space requests: a two-line header on every normal state.
std::vector<scav_box_space> headers_on(char const *name, scav_profile const &p) {
  Chart const probe{ loaded(name) };
  std::vector<scav_box_space> rows(probe.states.size());
  for (uint32_t st = 0; st < probe.states.size(); ++st) {
    if (probe.states[st].kind == StateKind::Normal) {
      rows[st].h_before = 2 * p.font_size_grid;
    }
  }
  return rows;
}

}  // namespace

TEST_CASE(
    "gauntlet: an external route out of a machine leaves its composite and returns") {
  // Out across the composite's border, outside it, and back in; with the inside kind it
  // stays in. Headed, no route enters a band. Tier 0 is zero throughout, no route crosses
  // a region divider or itself, and a loop off the composite reaches out `2 * pad`.
  for (OutAndBack const &shape : OUT_AND_BACK) {
    for (scav_profile const &p : { readable(), compact() }) {
      std::string const chart{ shape.chart };
      CAPTURE(chart);
      CAPTURE(p.profile_id);
      Laid out;
      lay(shape.chart, p, out);
      uint32_t const st{ state_named(out.c, shape.state) };
      uint32_t const t{ between(out.c, shape.src, shape.dst) };
      REQUIRE(st != INVALID);
      REQUIRE(t != INVALID);
      CHECK(out.c.transitions[t].kind == TransKind::External);
      CHECK(runs_against(out, t, st) == "IOI");
      CostTerms const terms{ cost_columns(out.c, out.g, p) };
      CHECK(cost_of(terms, p).t0_violations == 0);
      CHECK(terms.self_crossing == 0);
      CHECK_FALSE(crosses_divider(out, t, st));
      bool const own{ (out.c.transitions[t].src.v == st) ||
                      (out.c.transitions[t].dst.v == st) };
      if (own) { CHECK(reach_outside(out, t, st) >= (2 * p.pad)); }

      Laid in;
      lay(shape.chart, p, in, {}, nullptr, shape.inside);
      CHECK(runs_against(in, t, st) == "I");
      CHECK(cost_of(cost_columns(in.c, in.g, p), p).t0_violations == 0);

      std::vector<scav_box_space> const rows{ headers_on(shape.chart, p) };
      scav_spaces const s{ spaces_of(rows) };
      Laid headed;
      lay(shape.chart, p, headed, s, nullptr);
      CHECK_FALSE(any_band_entered(headed));
      CHECK(runs_against(headed, t, st) == "IOI");
      CHECK(cost_of(cost_terms(headed.c, headed.g, headed.z, headed.r, s, p), p)
                .t0_violations == 0);
      CHECK_FALSE(crosses_divider(headed, t, st));
      if (own) { CHECK(reach_outside(headed, t, st) >= (2 * p.pad)); }
    }
  }
}

TEST_CASE(
    "gauntlet: unsearched, an external route enters a region through its own share") {
  // Row 0 unsearched lays `detour`'s regions side by side; the facing pass keeps each port
  // off a face toward the sibling region.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("detour.scav", one_row(p), l, {}, nullptr);
    uint32_t const st{ state_named(l.c, "Outer") };
    uint32_t const t{ between(l.c, "A", "B") };
    REQUIRE(st != INVALID);
    REQUIRE(t != INVALID);
    CHECK_FALSE(crosses_divider(l, t, st));
    CHECK(runs_against(l, t, st) == "IOI");
    CHECK(cost_of(cost_columns(l.c, l.g, one_row(p)), p).t0_violations == 0);
  }
}

TEST_CASE(
    "gauntlet: unsearched, a route from outside enters a region through its own share") {
  // The root turned to run down puts `X` over `Outer`, whose regions stack; `B`'s region
  // lies under `A`'s, so its port keeps off its top border.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    SearchPins const seed{ .orients = { { .frame = SubmachineId{ 0 } } } };
    Laid l;
    lay("flank.scav", one_row(p), l, {}, &seed);
    uint32_t const st{ state_named(l.c, "Outer") };
    uint32_t const t{ between(l.c, "X", "B") };
    REQUIRE(st != INVALID);
    REQUIRE(t != INVALID);
    CHECK_FALSE(crosses_divider(l, t, st));
    CostTerms const terms{ cost_columns(l.c, l.g, one_row(p)) };
    CHECK(terms.through_region == 0);
    CHECK(cost_of(terms, p).t0_violations == 0);
  }
}

TEST_CASE("gauntlet: unsearched, an external route out of a machine crosses no own leg") {
  for (OutAndBack const &shape : OUT_AND_BACK) {
    for (scav_profile const &p : { readable(), compact() }) {
      std::string const chart{ shape.chart };
      CAPTURE(chart);
      CAPTURE(p.profile_id);
      Laid l;
      lay(shape.chart, one_row(p), l, {}, nullptr);
      CHECK(cost_columns(l.c, l.g, one_row(p)).self_crossing == 0);
    }
  }
}

namespace {

// Pairs of transition `t`'s non-adjacent route segments that cross.
uint32_t knots_of(Laid const &l, uint32_t t) {
  scav_span const route{ l.r.route[t] };
  uint32_t knots{ 0 };
  for (uint32_t i = 0; (i + 1) < route.len; ++i) {
    for (uint32_t j = i + 2; (j + 1) < route.len; ++j) {
      if (crosses(l.r.points[route.off + i],
                  l.r.points[route.off + i + 1],
                  l.r.points[route.off + j],
                  l.r.points[route.off + j + 1])) {
        ++knots;
      }
    }
  }
  return knots;
}

// `chart` unsearched at `readable` under `seed`, its `src -> dst` index in `t`.
void lay_pinned(char const *chart,
                char const *src,
                char const *dst,
                SearchPins seed,
                Laid &l,
                uint32_t &t) {
  t = between(loaded(chart), src, dst);
  REQUIRE(t != INVALID);
  for (EndPin &pin : seed.ends) { pin.trans = TransId{ t }; }
  lay(chart, one_row(readable()), l, {}, &seed);
}

}  // namespace

TEST_CASE("gauntlet: pinned so its own legs must cross, a route crosses itself") {
  // `A` fills `Outer`'s top right corner: its leg down to the bottom walls the right face
  // off from `Inner`, so the way back in on the right crosses it, and Tier 0 counts that.
  Laid l;
  uint32_t t{ INVALID };
  lay_pinned("bypass.scav",
             "A",
             "B",
             { .ends = { { .leg = 0, .end = 1, .face = 3 },
                         { .leg = 2, .end = 0, .face = 1 },
                         { .leg = 3, .end = 0, .face = 1 } },
               .orients = { { .frame = SubmachineId{ 2 } } } },
             l,
             t);
  CHECK(knots_of(l, t) == 1);
  CHECK(cost_columns(l.c, l.g, one_row(readable())).self_crossing == 1);
}

TEST_CASE(
    "gauntlet: pinned out the bottom and back in on the left, a route rounds its leg") {
  // `headed`'s way back in to `Busy` goes round `Idle` rather than across the leg from
  // `Idle` down to `Box`'s bottom.
  Laid l;
  uint32_t t{ INVALID };
  lay_pinned(
      "headed.scav",
      "Idle",
      "Work",
      { .ends = { { .leg = 0, .end = 1, .face = 3 }, { .leg = 2, .end = 0, .face = 0 } } },
      l,
      t);
  CHECK(knots_of(l, t) == 0);
  CHECK(l.r.failed[t] == 0);
}
