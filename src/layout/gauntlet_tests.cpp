// One layout element per chart, held to the properties a reader checks. The
// corpus next door says whether a real diagram comes out well; it cannot say
// which element was wrong when it does not, and every property below was a
// defect found there first and bisected back to one shape by hand.

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
#include <string>
#include <string_view>
#include <vector>

namespace scav {

// The portfolio's row, which `layout.cpp` brackets with SCAV_INTERNAL, declared
// here rather than in a header so the shipping build keeps it internal.
void search_tuple(scav_profile &p,
                  DarSource &dar,
                  Compaction &pack,
                  Fold &fold,
                  uint32_t index);

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

// Every chart in test_data/charts/gauntlet, named so a failure says which shape
// broke rather than which index did.
constexpr std::array GAUNTLET{
  "above.scav",     "carried.scav",   "chain.scav",   "corner.scav",  "crossing.scav",
  "crowd.scav",     "enclosing.scav", "entered.scav", "fanin.scav",   "folded.scav",
  "fork.scav",      "lane.scav",      "level.scav",   "long.scav",    "loop.scav",
  "marks.scav",     "mutual.scav",    "ported.scav",  "pulled.scav",  "regions.scav",
  "roundtrip.scav", "seated.scav",    "stretch.scav", "through.scav", "tight.scav",
  "transit.scav",   "under.scav",     "unfolded.scav"
};

// One chart, laid out: the pieces every property below reads.
struct Laid {
  Chart c;
  SplitGraph g;
  SubmachineOrders o;
  SizedLayout z;
  Routes r;
  SearchPins pins;            // what the drawing rests on
  uint32_t tuple{ INVALID };  // the portfolio row the run kept
};

// The profile with the portfolio and the move sweep switched off, so one row
// lays out and it is row 0 unsearched -- the caller's own tuple, and the
// pipeline as it ran before Level 2. The sweep is off too because a reversal
// kick from row 0 now reaches what the table's other rows did (11.10f).
scav_profile one_row(scav_profile const &p) {
  scav_profile out{ p };
  out.portfolio_m = 1;
  out.portfolio_k = 0;
  return out;
}

// The column's rows against what the phases produced, word for word. Every
// geometry POD is a block of int32 with no padding, so a word compare reads no
// byte whose value is unspecified.
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
// the run's space requests and starting pins.
void lay(char const *name,
         scav_profile const &p,
         Laid &out,
         scav_spaces const &s = {},
         SearchPins const *seed = nullptr) {
  // The router these properties are about, by the name it crosses every other
  // boundary under rather than by its position in the registry.
  scav_router_id id{};
  REQUIRE(router_by_name(reinterpret_cast<scav_byte const *>("orthogonal"), 10, id));
  std::string path{ SCAV_TEST_DATA_DIR "/charts/gauntlet/" };
  path += name;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, out.c, diags, failed));

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
  // Nothing here is a shape the router has to give up on, so a RouteDegraded
  // is a failure rather than a documented fallback.
  CHECK(diags.empty());

  scav_profile knobs{ p };
  DarSource dar{ DarSource::Profile };
  Compaction pack{ Compaction::Off };
  Fold fold{ Fold::Scale };
  search_tuple(knobs, dar, pack, fold, out.tuple);
  out.g = decompose(out.c);
  // The drawing is the tuple's *and* the pins' (11.10a), so re-deriving it
  // needs both or this measures a layout nobody was shown. The pins reach
  // phase 3 as well as phase 1: a face pin is the router's (11.10e).
  out.o = order_submachines(out.c, out.g, s, knobs, 0, pins);
  REQUIRE(size_layout(out.c, out.g, out.o, s, knobs, out.z, diags, dar, pack, fold));
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
  column_holds(out.c, "scav.geom.state", out.z.state);
  column_holds(out.c, "scav.geom.sub", out.z.sub);
  column_holds(out.c, "scav.geom.point", out.r.points);
  column_holds(out.c, "scav.geom.route", out.r.route);
  column_holds(out.c, "scav.geom.port", out.r.port);
  column_holds(out.c, "scav.geom.portslot", out.r.slots);
}

// The Tier-0 predicate, rewritten here as it is for the corpus: a gate that
// asks the scorer whether the scorer is happy is worth nothing.
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

// A bar is thin on one axis, so the two faces its short axis runs between are
// its long ones. A corner belongs to the cap rather than to either long face:
// that is the face an axis-aligned route leaves along.
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

// The two counts the regions carve-out is about: route segments entering a box
// 11.14 does not carve out, and legs that fold a polyline back over itself.
void shape_counts(Laid const &l, uint32_t &through, uint32_t &back);

uint32_t state_named(Chart const &c, std::string_view name) {
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    if (chart_string(c, c.states[st].name) == name) { return st; }
  }
  return INVALID;
}

// Live boxes, which is what a route may not enter and what its ends sit on.
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

// Every branch off a bar that left through one of its two short caps instead of
// along one of the long faces 11.5's rule is about.
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

TEST_CASE("gauntlet: no element routes an edge through a box") {
  // Every chart, `regions.scav` included: what the portfolio ships routes
  // through nothing at either profile. It carved this out while the suite
  // scored one candidate, and the count 11.8 still owns is pinned at the end
  // of this file against the row that produces it.
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
            // 11.14: a box enclosing either endpoint is crossed by the
            // transition's own meaning, and so is an endpoint's own box.
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

TEST_CASE("gauntlet: every route is axis-aligned, forward, and reaches its ends") {
  for (char const *name : GAUNTLET) {
    // The same shape and the same cause: a route round the outside of the state
    // holding both regions leaves and returns along one line.
    bool const open{ std::string_view{ name } == "regions.scav" };
    for (scav_profile const &p : { readable(), compact() }) {
      CAPTURE(name);
      CAPTURE(p.profile_id);
      Laid l;
      lay(name, p, l);
      for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
        CAPTURE(t);
        CHECK(l.r.failed[t] == 0);  // `lay` holds the run's diagnostics to this
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
        // A leg that reverses folds the polyline back over itself, and the
        // arrowhead then reads its direction off a line pointing both ways.
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

TEST_CASE("gauntlet: an end on an inscribed glyph is at the middle of a face") {
  // A disc and a diamond touch their box at four points. An axis-aligned route
  // to any other point on the face stops short of the mark it is drawn to, by
  // more of the glyph the further along the face it lands.
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
          if (!on_border(at, box)) { continue; }  // an inner face, 11.14's carve-out
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

TEST_CASE("gauntlet: an arrowhead is never inked over another route's own end") {
  // Two ends on one point of one box, one arriving and one leaving: the head is
  // drawn along the other route's first leg and reads as belonging to it. Two
  // arrivals sharing a point are a fan-in and keep their one head, which is
  // what gauntlet/fanin.scav is for; this is the mixed case. An inscribed glyph
  // seats one point per face and no other, so it answers by moving a direction
  // onto a face of its own rather than by sliding along one (11.5).
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
  // The bar is a mark whose long face is its attachment face, and every branch
  // off it used to be handed the box's centre: three arrows on one point of a
  // face fifteen times as long as the bar is wide, with the incoming arrowhead
  // inked over one of them. Two branches aimed the same way still share a
  // point, and that is a fan-out trunk 11.5 keeps whole, so two distinct
  // departure seats are not the property -- what may not happen is the arrival
  // joining them, or the whole bar collapsing to one seat.
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
          // On the bar rather than beside it, and on one of the two faces the
          // bar is long along, which is what 11.5's face rule says a bar gets
          // for free. The one branch that still leaves through a cap is
          // counted in "the shapes still open" below rather than passed over
          // here.
          CHECK(on_border(at, box));
          if (!on_long_face(at, box)) { continue; }
          bool fresh{ true };
          for (scav_point const &had : seats) { fresh = fresh && !same(at, had); }
          if (fresh) { seats.push_back(at); }
        }
      }
      // The whole property the bar has and a point does not.
      CHECK(seats.size() >= 2);
    }
  }
}

TEST_CASE("gauntlet: two states each other's target are two lines") {
  // Both transitions project onto the same point of the same face at both ends,
  // so without a seat apiece they draw as one line with a head at each end.
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
    // Neither end is shared, which is what the seating buys and what stops one
    // line carrying two heads.
    scav_span const a{ l.r.route[up] };
    scav_span const b{ l.r.route[down] };
    CHECK_FALSE(same(l.r.points[a.off], l.r.points[b.off + b.len - 1]));
    CHECK_FALSE(same(l.r.points[b.off], l.r.points[a.off + a.len - 1]));
    // Nor is any interior point, so the two are two polylines and not one drawn
    // twice over.
    for (uint32_t i = 0; i < a.len; ++i) {
      for (uint32_t j = 0; j < b.len; ++j) {
        CAPTURE(i);
        CAPTURE(j);
        CHECK_FALSE(same(l.r.points[a.off + i], l.r.points[b.off + j]));
      }
    }
    // The middle is a different question: breaking the cycle gives one of them
    // a corridor the long way round the frame, and where the frame is tight
    // enough that both take the same side of it the two share a run of it that
    // nudging has no room to take apart. 11.3's cycle-breaking heuristic is the
    // lever, so the compact profile's run is pinned here rather than excused.
    Wide shared{ 0 };
    for (uint32_t i = 0; (i + 1) < a.len; ++i) {
      for (uint32_t j = 0; (j + 1) < b.len; ++j) {
        shared += run_shared(l.r.points[a.off + i],
                             l.r.points[a.off + i + 1],
                             l.r.points[b.off + j],
                             l.r.points[b.off + j + 1]);
      }
    }
    // 652 before 11.9.5's reservation, 0 with it, 650 once offsets became lane
    // positions rather than per-member displacements, and **0 again once
    // nudging ran once over the composed polylines** (11.10a) -- the run was
    // between two segments the per-frame pass never had in hand at once.
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
  // Several transitions into one state converge near it; none may be drawn inside another.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("fanin.scav", p, l);
    uint32_t fault{ INVALID };
    for (uint32_t st = 0; st < l.c.states.size(); ++st) {
      if (chart_string(l.c, l.c.states[st].name) == "Fault") { fault = st; }
    }
    REQUIRE(fault != INVALID);
    // The fan's lanes all find room.
    CHECK(l.r.nudged.refused == 0);
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
    // A shared run is the trunk and is allowed; what is not is a run one route
    // shares with another over the whole of its own length. Measured against
    // each other route in turn and summed over this route's own segments,
    // since what a reader loses is the length hidden under one line rather
    // than the worst single overlap. Each segment contributes at most its own
    // length: two of the other route's segments may cover parts of the same
    // one, and adding both would charge that part twice.
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

TEST_CASE("gauntlet: an endpoint that is also a crossing is one point, not two") {
  // Into a composite's own child the route starts on the composite's border,
  // and the crossing it makes there is that same point; out of a child it ends
  // on it. Two points would put a leg along the border between them, which
  // reads as a route running round the box it is about to enter rather than
  // into it.
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
        // One slot per crossing, in the order the segments cross them, which
        // is how a slot is matched to its border everywhere else.
        REQUIRE(ports.len == (segs.len - 1));
        for (uint32_t k = 0; k < ports.len; ++k) {
          StateId const on{ l.g.ports[l.g.segments[segs.off + k].dst_port].state };
          scav_port_slot const slot{ l.r.slots[ports.off + k] };
          scav_point const at{ .x = slot.x, .y = slot.y };
          CAPTURE(t);
          CAPTURE(k);
          if (on == tr.src) { CHECK(same(l.r.points[route.off], at)); }
          if (on == tr.dst) { CHECK(same(l.r.points[route.off + route.len - 1], at)); }
        }
      }
    }
  }
}

TEST_CASE(
    "gauntlet: an out-of-machine label is drawn in the submachine holding both ends") {
  // Only the root holds both ends, so the label's width is charged to the root's
  // rank gap between the two composites and its box hangs off the leg there.
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
    lay("crossing.scav", p, l, spaces);
    REQUIRE(l.r.placed.size() == 1);
    CHECK(l.r.unplaced == 0);
    scav_rect const at{ l.r.placed[0] };

    Transition const &tr{ l.c.transitions[t] };
    CommonAncestor const lca{ lowest_common_ancestor(l.c, tr.src, tr.dst) };
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
  // Row 0 unsearched keeps the chain's own ranks, so only it is held to the gap
  // widths; what ships is held to placing both labels clear.
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
    lay("long.scav", p, l, spaces);
    CHECK(l.r.unplaced == 0);
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
  // Four boxes in a row fold into two rows, so two of the edges step between rows.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("chain.scav", p, l);
    CHECK(l.r.nudged.moved == 0);
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
    // A real-text title band on every state that draws one folds the run.
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
    lay("folded.scav", p, l, spaces);
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

    // The label sits in the room phase 2 reserved: on the leg's trailing side.
    REQUIRE(l.r.placed.size() == 1);
    CHECK(l.r.unplaced == 0);
    scav_rect const at{ l.r.placed[0] };
    CHECK(at.x >= from.x);
    uint32_t const frame{ l.c.submachine_ids[l.c.states[box].submachines.off].v };
    CHECK(contains(l.z.sub[frame], at));
  }
}

TEST_CASE(
    "gauntlet: a run the scale measure folds is laid straight where that is smaller") {
  // Row 0 unsearched folds `watch`; at `readable` the search pins it unfolded, its states
  // on one row. At `compact` it turns both regions down instead.
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
  // The root turned to run down puts `Source` over `Box`, whose own frame runs
  // across. Unsearched, so the facing pass alone puts the port on the top border.
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
    REQUIRE(l.pins.sides.size() == 1);
    CHECK(l.pins.sides[0].trans.v == drop);
    CHECK(l.pins.sides[0].leg == 1);
    CHECK(l.pins.sides[0].end == 0);
    CHECK(l.pins.sides[0].side == 2);
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
      .orients = { { .frame = frame_of(outer) }, { .frame = frame_of(inner) } },
      .sides = { { .trans = TransId{ reach }, .leg = 1, .end = 0, .side = 0 },
                 { .trans = TransId{ reach }, .leg = 2, .end = 0, .side = 0 } }
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

// `carried.scav`'s entering route unchained, and `Box`'s run cut before `Third` alone.
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

    // Straight, and no longer than the gap between the two states plus a rank gap.
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
  // The port is a point on `Box`'s border rather than a column of its own, and
  // the route turns nowhere, so `First` sits at most a route clearance inside.
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

// Holds the one transition leaving `from` to a straight line from its border to `to`'s.
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

TEST_CASE("gauntlet: every state lies inside the frame it is drawn in") {
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
  // `* -> T` seats beside `T`, so `B` is one rank gap past `A` however wide `T` is.
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
  // Moving a state a rank along squares the drawing up and stretches one
  // straight arrow across it, which only the route's own length prices.
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

TEST_CASE("gauntlet: a route passing through a composite bends outside it") {
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
  // Second's route leaves Box along its port's lead and turns once, onto the
  // vertical leg that enters Fault's top or bottom face.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("corner.scav", p, l);
    uint32_t const second{ state_named(l.c, "Second") };
    uint32_t const fault{ state_named(l.c, "Fault") };
    REQUIRE(second != INVALID);
    REQUIRE(fault != INVALID);
    uint32_t leaving{ INVALID };
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      Transition const &tr{ l.c.transitions[t] };
      if ((tr.src.v == second) && (tr.dst.v == fault)) { leaving = t; }
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
    CHECK(turns == 1U);
    scav_point const last{ l.r.points[route.off + route.len - 1] };
    scav_point const before{ l.r.points[route.off + route.len - 2] };
    scav_rect const box{ l.z.state[fault] };
    CHECK(before.x == last.x);
    CHECK(((last.y == box.y) || (last.y == (box.y + box.h))));
  }
}

TEST_CASE("gauntlet: the shapes still open, counted rather than excused") {
  // Each carve-out from the properties above, counted: a rising count is a regression.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);

    // Row 0 unsearched lays the chart out flat, and divider ports on the regions'
    // sides send three routes back on themselves; none passes through a box.
    Laid row_zero;
    lay("regions.scav", one_row(p), row_zero);
    REQUIRE(row_zero.tuple == 0);
    uint32_t through{ 0 };
    uint32_t back{ 0 };
    shape_counts(row_zero, through, back);
    CHECK(through == 0);
    CHECK(back == 3);
    // The scorer over the run's columns answers the same question.
    CHECK(cost_columns(row_zero.c, row_zero.g, p).through_box == 0);

    // What ships, scored both ways: the properties above hold on the drawing a
    // reader gets, whichever row the search ends on.
    Laid shipped;
    lay("regions.scav", p, shipped);
    uint32_t shipped_through{ 0 };
    uint32_t shipped_back{ 0 };
    shape_counts(shipped, shipped_through, shipped_back);
    CHECK(shipped_through == 0);
    CHECK(cost_columns(shipped.c, shipped.g, p).through_box == 0);
    // No route doubles back at either profile.
    CHECK(shipped_back == 0);

    // The face rule picks by separation per axis, so a branch stacked below a bar can
    // leave through the bar's short cap.
    Laid fork_zero;
    lay("fork.scav", one_row(p), fork_zero);
    CHECK(capped_branches(fork_zero) == 1);
    // What ships: the search settles on a four-bend arrangement where one
    // arrival enters the join through its cap.
    Laid fork_shipped;
    lay("fork.scav", p, fork_shipped);
    CHECK(capped_branches(fork_shipped) == 1);
  }
}
