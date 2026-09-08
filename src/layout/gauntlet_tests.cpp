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
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

// The portfolio's row, which `layout.cpp` brackets with SCAV_INTERNAL, declared
// here rather than in a header so the shipping build keeps it internal.
void search_tuple(scav_profile &p, DarSource &dar, uint32_t index);

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
constexpr std::array GAUNTLET{ "chain.scav",  "crowd.scav", "enclosing.scav",
                               "fanin.scav",  "fork.scav",  "lane.scav",
                               "loop.scav",   "marks.scav", "mutual.scav",
                               "regions.scav" };

// One chart, laid out: the pieces every property below reads.
struct Laid {
  Chart c;
  SplitGraph g;
  SubmachineOrders o;
  SizedLayout z;
  Routes r;
  uint32_t tuple{ INVALID };  // the portfolio row the run kept
};

// The profile with the portfolio switched off, so one row lays out and it is
// row 0 -- the caller's own tuple, and the pipeline as it ran before Level 2.
scav_profile one_row(scav_profile const &p) {
  scav_profile out{ p };
  out.portfolio_m = 1;
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

// What a reader gets: `layout_run` picks a phase-2 tuple out of the portfolio
// and writes the geometry columns (11.10). The phases are then re-run for that
// same row, because the nudge statistics and the per-transition fallback flag
// have no column of their own, and every column they came with is held to the
// one the run wrote -- so the properties below read the drawing that ships and
// not a candidate nobody drew.
void lay(char const *name, scav_profile const &p, Laid &out) {
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
  REQUIRE(layout_run(out.c, {}, o, placed, diags, nullptr, &out.tuple));
  // Nothing here is a shape the router has to give up on, so a RouteDegraded
  // is a failure rather than a documented fallback.
  CHECK(diags.empty());

  scav_profile knobs{ p };
  DarSource dar{ DarSource::Profile };
  search_tuple(knobs, dar, out.tuple);
  out.g = decompose(out.c);
  out.o = order_submachines(out.c, out.g, {}, knobs);
  REQUIRE(size_layout(out.c, out.g, out.o, {}, knobs, out.z, diags, dar));
  out.r = route_transitions(out.c, out.g, out.o, out.z, {}, knobs, *router_at(id));
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
    CHECK(shared == ((p.profile_id == compact().profile_id) ? 652 : 0));
  }
}

TEST_CASE("gauntlet: a fan-in's arrivals are four arrows, none inside another") {
  // The counterpart to the fork: several transitions into one state coincide
  // near it by construction, and 11.5's bundles exist to stop nudging taking
  // that trunk apart into four parallel lanes a gap each side. Whether they
  // converge or arrive at seats of their own is the projection's business and
  // measured on the corpus; what may not happen either way is one of them
  // being drawn inside another, where a reader sees three arrows and four
  // labels.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("fanin.scav", p, l);
    uint32_t fault{ INVALID };
    for (uint32_t st = 0; st < l.c.states.size(); ++st) {
      if (chart_string(l.c, l.c.states[st].name) == "Fault") { fault = st; }
    }
    REQUIRE(fault != INVALID);
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

TEST_CASE("gauntlet: a chain of states turns only where the fold cuts it") {
  // The trivial case nothing else here covers, and it is not as trivial as it
  // looks: four boxes in a row fold into two rows because a strip four wide has
  // the worse aspect, so two of the four edges step between rows.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);
    Laid l;
    lay("chain.scav", p, l);
    CHECK(l.r.nudged.moved == 0);
    for (uint32_t t = 0; t < l.c.transitions.size(); ++t) {
      CAPTURE(t);
      // A four-state run folds rather than drawing a strip four boxes wide
      // (11.4), so an edge the cut crosses steps between two rows and turns
      // twice. Nothing here needs a third turn, and a lane is work invented.
      CHECK(l.r.route[t].len <= 4);
    }
  }
}

TEST_CASE("gauntlet: the shapes still open, counted rather than excused") {
  // A property above that cannot hold on one of these charts carves it out, and
  // a carve-out with no number on it is an excuse. Each count is what the tree
  // does today with the section that owns it named; a count that grows is a
  // regression and one that shrinks is the fix arriving, and either way the
  // test says so.
  for (scav_profile const &p : { readable(), compact() }) {
    CAPTURE(p.profile_id);

    // 11.8, and the corpus has no chart carrying the shape. A transition
    // between two concurrent submachines of one state is routed in the frame
    // that state itself sits in -- the two submachines are siblings under it
    // and their common frame is one level further out than the state -- so the
    // state is an obstacle walling the route out of the space between its own
    // two regions, and the route goes the long way round it: through whatever
    // else that frame holds, and back along the line it arrived on.
    //
    // The portfolio routes around that hole rather than closing it: at the
    // shipped M the packer choice puts the two regions where the long way round
    // clips nothing, which is why the property above now holds on this chart
    // too. So the count stays visible against the row that produces it -- row 0
    // alone, the profile's own tuple -- until 11.8 is built and it is zero
    // whichever row lays out.
    Laid row_zero;
    lay("regions.scav", one_row(p), row_zero);
    REQUIRE(row_zero.tuple == 0);
    uint32_t through{ 0 };
    uint32_t back{ 0 };
    shape_counts(row_zero, through, back);
    CHECK(through == 2);
    CHECK(back == 2);
    // The scorer from the other end, over the columns that run wrote: 11.6's
    // descent and the predicate above are two implementations of one question,
    // and a carve-out is worth more when both answer it.
    CHECK(cost_columns(row_zero.c, row_zero.g, p).through_box == 2);

    // What ships, scored both ways: the shape is still there and the drawing no
    // longer shows it, which is why the properties above hold on this chart.
    Laid shipped;
    lay("regions.scav", p, shipped);
    CHECK(shipped.tuple != 0);
    uint32_t shipped_through{ 0 };
    uint32_t shipped_back{ 0 };
    shape_counts(shipped, shipped_through, shipped_back);
    CHECK(shipped_through == 0);
    CHECK(cost_columns(shipped.c, shipped.g, p).through_box == 0);
    // The other half of the same hole, and it got worse rather than better: the
    // route still leaves and returns along one line, and the arrangement the
    // packer picked makes it double back twice each way instead of once. That
    // is the count the axis-aligned property still carves this chart out for,
    // and it is 11.8's to take to zero.
    CHECK(shipped_back == 4);

    // 11.5's face rule, which picks a face by how far the target lies outside
    // the box on each axis rather than by the distance to a point on it. A
    // branch whose target is stacked below the bar rather than beside it has
    // the y separation dominate, so it leaves through the bar's own 64-unit
    // cap while the 960-unit face beside it goes unused, and the arrow then
    // reads as coming off the bar's end instead of off its length.
    Laid fork_zero;
    lay("fork.scav", one_row(p), fork_zero);
    CHECK(capped_branches(fork_zero) == 1);
    // And what ships: at `readable` the packer choice stacks the branch beside
    // the bar rather than below it, so the y separation stops dominating and
    // the count is zero. The rule is still the rule -- `compact` packs tighter,
    // ships row 0, and still reads one.
    Laid fork_shipped;
    lay("fork.scav", p, fork_shipped);
    CHECK(capped_branches(fork_shipped) ==
          ((p.profile_id == compact().profile_id) ? 1U : 0U));
  }
}
