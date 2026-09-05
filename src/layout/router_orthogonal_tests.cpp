// The orthogonal router part by part, every input typed into the test so a
// failure names one function (11). The search is checked twice: against
// hand-computable answers, and against an independent Dijkstra.

#include "layout/router_orthogonal.h"

#include "layout/router.h"
#include "scav/scav_layout.h"
#include "scav_int.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace scav;

constexpr bool operator==(scav_point const &a, scav_point const &b) {
  return (a.x == b.x) && (a.y == b.y);
}

constexpr scav_point pt(int32_t x, int32_t y) { return { .x = x, .y = y }; }

constexpr scav_rect rect(int32_t x, int32_t y, int32_t w, int32_t h) {
  return { .x = x, .y = y, .w = w, .h = h };
}

scav_profile profile() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

// --- Independent checkers; none call into the router ---

// The cost scorer's Tier-0 predicate, rewritten: a segment enters the rect's
// interior if either end is strictly inside or it properly crosses a side.
bool enters_box(scav_point a, scav_point b, scav_rect const &r) {
  auto const strictly_in = [&r](scav_point p) {
    return (p.x > r.x) && (p.x < (r.x + r.w)) && (p.y > r.y) && (p.y < (r.y + r.h));
  };
  if (strictly_in(a) || strictly_in(b)) { return true; }
  // Axis-aligned, so a crossing is an overlap on one axis and a strict
  // straddle on the other.
  if (a.y == b.y) {
    int32_t const lo{ imin(a.x, b.x) };
    int32_t const hi{ imax(a.x, b.x) };
    return (a.y > r.y) && (a.y < (r.y + r.h)) && (lo < (r.x + r.w)) && (hi > r.x);
  }
  if (a.x == b.x) {
    int32_t const lo{ imin(a.y, b.y) };
    int32_t const hi{ imax(a.y, b.y) };
    return (a.x > r.x) && (a.x < (r.x + r.w)) && (lo < (r.y + r.h)) && (hi > r.y);
  }
  FAIL("a route segment was not axis-aligned");
  return true;
}

// Shortest path over the same plane-split graph by an O(V^2) scan: no
// heuristic, no heap, no tie-break. -1 when the target is unreachable.
Wide reference_cost(OrthoGrid const &g, uint32_t from, uint32_t to, Wide bend) {
  size_t const nodes{ size_t{ g.nx() } * g.ny() * 2 };
  std::vector<Wide> dist(nodes, -1);
  std::vector<uint8_t> done(nodes, 0);
  dist[size_t{ from } * 2] = 0;
  dist[(size_t{ from } * 2) + 1] = 0;
  for (;;) {
    uint32_t at{ INVALID };
    Wide best{ -1 };
    for (size_t i = 0; i < nodes; ++i) {
      if ((done[i] != 0) || (dist[i] < 0)) { continue; }
      if ((best < 0) || (dist[i] < best)) {
        best = dist[i];
        at = static_cast<uint32_t>(i);
      }
    }
    if (at == INVALID) { break; }
    done[at] = 1;
    uint32_t const v{ at / 2 };
    uint32_t const plane{ at % 2 };
    uint32_t const ix{ v % g.nx() };
    uint32_t const iy{ v / g.nx() };
    auto const edge = [&](uint32_t next, Wide w) {
      if ((dist[next] < 0) || (dist[next] > (best + w))) { dist[next] = best + w; }
    };
    edge((v * 2) + (1 - plane), bend);
    if (plane == 0) {
      if (((ix + 1) < g.nx()) && (g.pass_h[(iy * (g.nx() - 1)) + ix] != 0)) {
        edge(g.vertex(ix + 1, iy) * 2, Wide{ g.xs[ix + 1] } - g.xs[ix]);
      }
      if ((ix > 0) && (g.pass_h[(iy * (g.nx() - 1)) + (ix - 1)] != 0)) {
        edge(g.vertex(ix - 1, iy) * 2, Wide{ g.xs[ix] } - g.xs[ix - 1]);
      }
    } else {
      if (((iy + 1) < g.ny()) && (g.pass_v[(iy * g.nx()) + ix] != 0)) {
        edge((g.vertex(ix, iy + 1) * 2) + 1, Wide{ g.ys[iy + 1] } - g.ys[iy]);
      }
      if ((iy > 0) && (g.pass_v[((iy - 1) * g.nx()) + ix] != 0)) {
        edge((g.vertex(ix, iy - 1) * 2) + 1, Wide{ g.ys[iy] } - g.ys[iy - 1]);
      }
    }
  }
  Wide const a{ dist[size_t{ to } * 2] };
  Wide const b{ dist[(size_t{ to } * 2) + 1] };
  if (a < 0) { return b; }
  if (b < 0) { return a; }
  return imin(a, b);
}

// Length plus one bend per direction change, over a vertex path. Starting in
// either plane is free, so the first leg never charges a bend.
Wide path_cost(OrthoGrid const &g, std::vector<uint32_t> const &path, Wide bend) {
  Wide total{ 0 };
  uint32_t previous{ 9 };
  for (uint32_t k = 0; (k + 1) < path.size(); ++k) {
    scav_point const a{ g.point(path[k]) };
    scav_point const b{ g.point(path[k + 1]) };
    Wide const dx{ (a.x < b.x) ? (Wide{ b.x } - a.x) : (Wide{ a.x } - b.x) };
    Wide const dy{ (a.y < b.y) ? (Wide{ b.y } - a.y) : (Wide{ a.y } - b.y) };
    total += dx + dy;
    uint32_t const axis{ (a.x == b.x) ? 1U : 0U };
    if ((k > 0) && (axis != previous)) { total += bend; }
    previous = axis;
  }
  return total;
}

// Every step is one grid edge, in bounds, and passable.
void check_path_is_walkable(OrthoGrid const &g, std::vector<uint32_t> const &path) {
  for (uint32_t k = 0; (k + 1) < path.size(); ++k) {
    uint32_t const ax{ path[k] % g.nx() };
    uint32_t const ay{ path[k] / g.nx() };
    uint32_t const bx{ path[k + 1] % g.nx() };
    uint32_t const by{ path[k + 1] / g.nx() };
    CAPTURE(ax);
    CAPTURE(ay);
    CAPTURE(bx);
    CAPTURE(by);
    if (ay == by) {
      bool const adjacent{ ((ax + 1) == bx) || ((bx + 1) == ax) };
      REQUIRE(adjacent);
      CHECK(g.pass_h[(ay * (g.nx() - 1)) + imin(ax, bx)] != 0);
    } else {
      REQUIRE(ax == bx);
      bool const adjacent{ ((ay + 1) == by) || ((by + 1) == ay) };
      REQUIRE(adjacent);
      CHECK(g.pass_v[(imin(ay, by) * g.nx()) + ax] != 0);
    }
  }
}

// doctest compares with `std::equal`, which finds an operator by argument
// lookup only, and this file's `operator==` is in an unnamed namespace.
void check_points(std::vector<scav_point> const &got,
                  std::vector<scav_point> const &want) {
  REQUIRE(got.size() == want.size());
  for (uint32_t i = 0; i < got.size(); ++i) {
    CAPTURE(i);
    CHECK((got[i] == want[i]));
  }
}

// A counter-based generator, so a case number reproduces its own inputs.
uint32_t mix(uint32_t x) {
  x ^= x >> 16U;
  x *= 0x7feb352dU;
  x ^= x >> 15U;
  x *= 0x846ca68bU;
  x ^= x >> 16U;
  return x;
}

}  // namespace

// --- The blocking predicates ---

TEST_CASE("ortho: a horizontal segment blocks only on a strict interior cut") {
  scav_rect const r{ rect(10, 10, 20, 20) };  // x in [10,30], y in [10,30]

  // Both borders are open: a route may run along a box edge, and the cost
  // scorer does not count that as entering either.
  CHECK(!ortho_blocks_h(r, 10, 0, 40));
  CHECK(!ortho_blocks_h(r, 30, 0, 40));
  CHECK(!ortho_blocks_h(r, 9, 0, 40));
  CHECK(!ortho_blocks_h(r, 31, 0, 40));

  // Clear of the box on the layering axis, at an interior height.
  CHECK(!ortho_blocks_h(r, 20, 0, 5));
  CHECK(!ortho_blocks_h(r, 20, 0, 10));   // ends exactly on the left border
  CHECK(!ortho_blocks_h(r, 20, 30, 40));  // starts exactly on the right border
  CHECK(!ortho_blocks_h(r, 20, 35, 40));

  // Any strict overlap of the interior.
  CHECK(ortho_blocks_h(r, 20, 0, 11));
  CHECK(ortho_blocks_h(r, 20, 29, 40));
  CHECK(ortho_blocks_h(r, 20, 12, 18));
  CHECK(ortho_blocks_h(r, 20, 0, 40));
  CHECK(ortho_blocks_h(r, 11, 0, 40));
  CHECK(ortho_blocks_h(r, 29, 0, 40));

  // A degenerate box has no interior to enter.
  CHECK(!ortho_blocks_h(rect(10, 10, 20, 0), 10, 0, 40));
  CHECK(!ortho_blocks_h(rect(10, 10, 0, 20), 20, 0, 40));
}

TEST_CASE("ortho: a vertical segment blocks on the same rule, axes swapped") {
  scav_rect const r{ rect(10, 10, 20, 20) };

  CHECK(!ortho_blocks_v(r, 10, 0, 40));
  CHECK(!ortho_blocks_v(r, 30, 0, 40));
  CHECK(!ortho_blocks_v(r, 9, 0, 40));
  CHECK(!ortho_blocks_v(r, 31, 0, 40));

  CHECK(!ortho_blocks_v(r, 20, 0, 5));
  CHECK(!ortho_blocks_v(r, 20, 0, 10));
  CHECK(!ortho_blocks_v(r, 20, 30, 40));

  CHECK(ortho_blocks_v(r, 20, 0, 11));
  CHECK(ortho_blocks_v(r, 20, 29, 40));
  CHECK(ortho_blocks_v(r, 20, 12, 18));
  CHECK(ortho_blocks_v(r, 11, 0, 40));
  CHECK(ortho_blocks_v(r, 29, 0, 40));

  CHECK(!ortho_blocks_v(rect(10, 10, 0, 20), 10, 0, 40));
  CHECK(!ortho_blocks_v(rect(10, 10, 20, 0), 20, 0, 40));
}

TEST_CASE("ortho: the two predicates agree with the cost scorer's own") {
  // The router forbids exactly what the scorer counts; a drift between the
  // two would show as a Tier-0 violation the router believed it had avoided.
  scav_rect const r{ rect(100, 100, 200, 200) };
  for (int32_t y = 60; y <= 340; y += 20) {
    for (int32_t x0 = 0; x0 <= 400; x0 += 50) {
      for (int32_t x1 = x0; x1 <= 400; x1 += 50) {
        CAPTURE(y);
        CAPTURE(x0);
        CAPTURE(x1);
        CHECK(ortho_blocks_h(r, y, x0, x1) == enters_box(pt(x0, y), pt(x1, y), r));
        CHECK(ortho_blocks_v(r, y, x0, x1) == enters_box(pt(y, x0), pt(y, x1), r));
      }
    }
  }
}

// --- Line condensing and lookup ---

TEST_CASE("ortho: condensing sorts, deduplicates, and keeps negatives") {
  std::vector<int32_t> empty;
  ortho_sort_unique(empty);
  CHECK(empty.empty());

  std::vector<int32_t> one{ 7 };
  ortho_sort_unique(one);
  CHECK(one == std::vector<int32_t>{ 7 });

  std::vector<int32_t> same{ 3, 3, 3, 3 };
  ortho_sort_unique(same);
  CHECK(same == std::vector<int32_t>{ 3 });

  std::vector<int32_t> sorted{ 1, 2, 3 };
  ortho_sort_unique(sorted);
  CHECK(sorted == std::vector<int32_t>{ 1, 2, 3 });

  std::vector<int32_t> mixed{ 5, -3, 5, 0, -3, 12, 0 };
  ortho_sort_unique(mixed);
  CHECK(mixed == std::vector<int32_t>{ -3, 0, 5, 12 });

  std::vector<int32_t> reversed{ 9, 8, 7, 6, 5 };
  ortho_sort_unique(reversed);
  CHECK(reversed == std::vector<int32_t>{ 5, 6, 7, 8, 9 });
}

TEST_CASE("ortho: the line lookup finds every value it was given") {
  std::vector<int32_t> const lines{ -40, -5, 0, 3, 17, 200 };
  for (uint32_t i = 0; i < lines.size(); ++i) {
    CAPTURE(i);
    CHECK(ortho_index_of(lines, lines[i]) == i);
  }
  // Between two lines it answers the lower, and it never runs off either end.
  CHECK(ortho_index_of(lines, 4) == 3);
  CHECK(ortho_index_of(lines, -1000) == 0);
  CHECK(ortho_index_of(lines, 1000) == 5);

  std::vector<int32_t> const single{ 42 };
  CHECK(ortho_index_of(single, 42) == 0);
  CHECK(ortho_index_of(single, 0) == 0);
}

// --- The escape off a box ---

TEST_CASE("ortho: escaping leaves a point that is not strictly inside alone") {
  std::vector<scav_rect> const boxes{ rect(10, 10, 20, 20) };
  CHECK((ortho_escape(pt(0, 0), pt(100, 0), boxes) == pt(0, 0)));
  CHECK((ortho_escape(pt(50, 50), pt(0, 0), boxes) == pt(50, 50)));
  // On a border is not inside it, which is what keeps a port slot where the
  // planner put it.
  CHECK((ortho_escape(pt(10, 20), pt(100, 20), boxes) == pt(10, 20)));
  CHECK((ortho_escape(pt(30, 20), pt(100, 20), boxes) == pt(30, 20)));
  CHECK((ortho_escape(pt(20, 10), pt(20, 0), boxes) == pt(20, 10)));
  CHECK((ortho_escape(pt(20, 30), pt(20, 99), boxes) == pt(20, 30)));

  CHECK((ortho_escape(pt(20, 20), pt(0, 0), {}) == pt(20, 20)));
}

TEST_CASE("ortho: escaping moves to the border nearest where the route is going") {
  std::vector<scav_rect> const boxes{ rect(10, 10, 20, 20) };
  scav_point const centre{ pt(20, 20) };

  CHECK((ortho_escape(centre, pt(100, 20), boxes) == pt(30, 20)));   // right
  CHECK((ortho_escape(centre, pt(-100, 20), boxes) == pt(10, 20)));  // left
  CHECK((ortho_escape(centre, pt(20, -100), boxes) == pt(20, 10)));  // up
  CHECK((ortho_escape(centre, pt(20, 100), boxes) == pt(20, 30)));   // down

  // One axis only, so the stub the escape leaves behind is axis-aligned.
  scav_point const away{ ortho_escape(centre, pt(500, 12), boxes) };
  CHECK(((away.x == centre.x) || (away.y == centre.y)));
}

TEST_CASE("ortho: an elongated box is left through a long face, not an end") {
  // The separation is measured from the box, not from a candidate border point:
  // measuring to the border charges an exit for the run along the face it leaves
  // through, so the longer a face is the worse its own exit scores. A fork bar is
  // the shape that makes the difference visible.
  std::vector<scav_rect> const bar{ rect(0, 0, 4, 60) };  // 4x60, ends at top and bottom
  scav_point const centre{ pt(2, 30) };

  // Far to the right and slightly above: the target is nearer the top end in y,
  // but x is what separates them, so the exit is the long right face.
  CHECK((ortho_escape(centre, pt(500, 1), bar) == pt(4, 30)));
  CHECK((ortho_escape(centre, pt(-500, 1), bar) == pt(0, 30)));
  CHECK((ortho_escape(centre, pt(500, 59), bar) == pt(4, 30)));

  // Only a target genuinely stacked above or below leaves through an end.
  CHECK((ortho_escape(centre, pt(2, -500), bar) == pt(2, 0)));
  CHECK((ortho_escape(centre, pt(2, 500), bar) == pt(2, 60)));

  // And the transpose behaves the same way about its own long faces.
  std::vector<scav_rect> const flat{ rect(0, 0, 60, 4) };
  CHECK((ortho_escape(pt(30, 2), pt(1, 500), flat) == pt(30, 4)));
  CHECK((ortho_escape(pt(30, 2), pt(1, -500), flat) == pt(30, 0)));
  CHECK((ortho_escape(pt(30, 2), pt(-500, 2), flat) == pt(0, 2)));
}

TEST_CASE("ortho: an equidistant escape is decided by the fixed side order") {
  // Dead centre with a target dead centre: neither axis separates, so the tie
  // goes to x and then to the nearer border, which is the left one.
  std::vector<scav_rect> const boxes{ rect(0, 0, 20, 20) };
  CHECK((ortho_escape(pt(10, 10), pt(10, 10), boxes) == pt(0, 10)));

  // Two boxes containing the point: innermost wins whichever order they arrive
  // in, since the shortest stub crosses least.
  std::vector<scav_rect> const nested{ rect(5, 5, 10, 10), rect(0, 0, 20, 20) };
  CHECK((ortho_escape(pt(10, 10), pt(100, 10), nested) == pt(15, 10)));
  std::vector<scav_rect> const other_way{ rect(0, 0, 20, 20), rect(5, 5, 10, 10) };
  CHECK((ortho_escape(pt(10, 10), pt(100, 10), other_way) == pt(15, 10)));

  // Equal areas fall back on list order, so the answer is still total.
  std::vector<scav_rect> const twins{ rect(0, 0, 20, 20), rect(2, 2, 20, 20) };
  CHECK((ortho_escape(pt(10, 10), pt(100, 10), twins) == pt(20, 10)));
}

// --- Polyline simplification ---

TEST_CASE("ortho: simplifying drops repeats and the middle of collinear runs") {
  std::vector<scav_point> to;

  ortho_simplify({}, to);
  CHECK(to.empty());

  ortho_simplify({ pt(0, 0) }, to);
  check_points(to, { pt(0, 0) });

  to.clear();
  ortho_simplify({ pt(0, 0), pt(0, 0), pt(0, 0) }, to);
  check_points(to, { pt(0, 0) });

  to.clear();
  ortho_simplify({ pt(0, 0), pt(5, 0), pt(9, 0) }, to);
  check_points(to, { pt(0, 0), pt(9, 0) });

  to.clear();
  ortho_simplify({ pt(0, 0), pt(0, 5), pt(0, 9) }, to);
  check_points(to, { pt(0, 0), pt(0, 9) });

  // A corner is a turn and survives.
  to.clear();
  ortho_simplify({ pt(0, 0), pt(5, 0), pt(5, 9) }, to);
  check_points(to, { pt(0, 0), pt(5, 0), pt(5, 9) });

  // A staircase is all corners.
  to.clear();
  ortho_simplify({ pt(0, 0), pt(2, 0), pt(2, 2), pt(4, 2), pt(4, 4) }, to);
  CHECK(to.size() == 5);

  // Doubling straight back collapses to the one point, because popping the
  // collinear middle exposes a duplicate underneath it.
  to.clear();
  ortho_simplify({ pt(0, 0), pt(5, 0), pt(0, 0) }, to);
  check_points(to, { pt(0, 0) });

  to.clear();
  ortho_simplify({ pt(0, 0), pt(0, 5), pt(0, 0) }, to);
  check_points(to, { pt(0, 0) });

  // And a there-and-back that overshoots keeps only where it ended up.
  to.clear();
  ortho_simplify({ pt(0, 0), pt(9, 0), pt(3, 0) }, to);
  check_points(to, { pt(0, 0), pt(3, 0) });
}

TEST_CASE("ortho: simplifying across two calls closes the seam between them") {
  // Consecutive A* hops meet at a shared anchor, and a route that ran straight
  // through one must not keep a vertex there just because two searches met.
  std::vector<scav_point> to;
  ortho_simplify({ pt(0, 0), pt(5, 0) }, to);
  ortho_simplify({ pt(5, 0), pt(9, 0) }, to);
  check_points(to, { pt(0, 0), pt(9, 0) });

  to.clear();
  ortho_simplify({ pt(0, 0), pt(5, 0) }, to);
  ortho_simplify({ pt(5, 0), pt(5, 4) }, to);
  check_points(to, { pt(0, 0), pt(5, 0), pt(5, 4) });
}

TEST_CASE("ortho: escaping a named box does not ask whether it contains the point") {
  // The named-obstacle path hands the box in rather than searching: a box centre
  // can sit inside a nested box too, and the caller knows which it meant.
  scav_rect const r{ rect(10, 10, 20, 20) };
  CHECK((ortho_escape_box(pt(20, 20), pt(100, 20), r) == pt(30, 20)));
  CHECK((ortho_escape_box(pt(20, 20), pt(-100, 20), r) == pt(10, 20)));
  CHECK((ortho_escape_box(pt(20, 20), pt(20, -100), r) == pt(20, 10)));
  CHECK((ortho_escape_box(pt(20, 20), pt(20, 100), r) == pt(20, 30)));
  // A point already outside is still projected onto the named box.
  CHECK((ortho_escape_box(pt(20, 50), pt(100, 50), r) == pt(30, 50)));
  // Equidistant resolves left, right, top, bottom.
  CHECK((ortho_escape_box(pt(20, 20), pt(20, 20), r) == pt(10, 20)));
}

TEST_CASE("ortho: the box under a point is the innermost whose border holds it") {
  std::vector<scav_rect> const boxes{ rect(0, 0, 100, 100), rect(0, 20, 40, 40) };
  // On the shared left edge, inside both: the smaller wins.
  CHECK(ortho_box_at(pt(0, 30), boxes) == 1);
  // On the outer box only.
  CHECK(ortho_box_at(pt(0, 90), boxes) == 0);
  CHECK(ortho_box_at(pt(100, 50), boxes) == 0);
  CHECK(ortho_box_at(pt(50, 0), boxes) == 0);
  CHECK(ortho_box_at(pt(50, 100), boxes) == 0);
  // Corners count as on the border.
  CHECK(ortho_box_at(pt(40, 20), boxes) == 1);
  // Strictly inside is not on a border, and neither is outside.
  CHECK(ortho_box_at(pt(50, 50), boxes) == INVALID);
  CHECK(ortho_box_at(pt(200, 200), boxes) == INVALID);
  CHECK(ortho_box_at(pt(0, 0), {}) == INVALID);

  // Equal areas fall back on list order, so the answer stays total.
  std::vector<scav_rect> const twins{ rect(0, 0, 10, 10), rect(0, 0, 10, 10) };
  CHECK(ortho_box_at(pt(0, 5), twins) == 0);
}

TEST_CASE("ortho: the ring point is one clearance straight out of the border") {
  scav_rect const r{ rect(10, 10, 20, 20) };
  CHECK((ortho_ring(pt(10, 20), r, 4) == pt(6, 20)));   // left
  CHECK((ortho_ring(pt(30, 20), r, 4) == pt(34, 20)));  // right
  CHECK((ortho_ring(pt(20, 10), r, 4) == pt(20, 6)));   // top
  CHECK((ortho_ring(pt(20, 30), r, 4) == pt(20, 34)));  // bottom

  // A corner is on two sides at once and resolves in the fixed order.
  CHECK((ortho_ring(pt(10, 10), r, 4) == pt(6, 10)));

  // Nowhere on the border, or no clearance to give: unchanged, and the caller
  // reads that as "this end gets no ring".
  CHECK((ortho_ring(pt(20, 20), r, 4) == pt(20, 20)));
  CHECK((ortho_ring(pt(50, 50), r, 4) == pt(50, 50)));
  CHECK((ortho_ring(pt(10, 20), r, 0) == pt(10, 20)));
}

// --- The grid ---

TEST_CASE("ortho: an empty region is two lines each way and fully open") {
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 100, 80), {}, {}, 8, g));
  CHECK(g.xs == std::vector<int32_t>{ 0, 100 });
  CHECK(g.ys == std::vector<int32_t>{ 0, 80 });
  CHECK(g.nx() == 2);
  CHECK(g.ny() == 2);
  for (uint8_t const at : g.pass_h) { CHECK(at != 0); }
  for (uint8_t const at : g.pass_v) { CHECK(at != 0); }
}

TEST_CASE("ortho: the grid carries clearance lanes and never a box's own border") {
  // A line on a box edge is a lane, and hugging costs nothing in length or
  // bends, so a border line is an invitation. Only the offsets are laid.
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), { rect(40, 40, 20, 20) }, {}, 5, g));
  CHECK(g.xs == std::vector<int32_t>{ 0, 35, 65, 100 });
  CHECK(g.ys == std::vector<int32_t>{ 0, 35, 65, 100 });

  // Zero clearance collapses the lanes back onto the borders, which is the one
  // case where a border line is laid -- and is why the profile floors it at 1.
  OrthoGrid tight;
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), { rect(40, 40, 20, 20) }, {}, 0, tight));
  CHECK(tight.xs == std::vector<int32_t>{ 0, 40, 60, 100 });
}

TEST_CASE("ortho: lines outside the region are dropped, so no route can leave") {
  OrthoGrid g;
  // The obstacle straddles the left edge and its clearance lanes fall outside.
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), { rect(-30, 40, 20, 20) }, {}, 5, g));
  for (int32_t const at : g.xs) {
    CHECK(at >= 0);
    CHECK(at <= 100);
  }
  // An anchor outside the region contributes no line either.
  OrthoGrid h;
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), {}, { pt(-5, 50), pt(50, 500) }, 5, h));
  CHECK(h.xs == std::vector<int32_t>{ 0, 100 });
  CHECK(h.ys == std::vector<int32_t>{ 0, 100 });
}

TEST_CASE("ortho: anchors inside the region each get their own pair of lines") {
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), {}, { pt(17, 3), pt(17, 90) }, 5, g));
  CHECK(g.xs == std::vector<int32_t>{ 0, 17, 100 });
  CHECK(g.ys == std::vector<int32_t>{ 0, 3, 90, 100 });
}

TEST_CASE("ortho: passability is closed exactly where a lane cuts a box") {
  OrthoGrid g;
  scav_rect const box{ rect(40, 40, 20, 20) };
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), { box }, { pt(50, 50) }, 0, g));

  // Every table entry agrees with the predicate it was built from, and the
  // predicate is independently checked above.
  for (uint32_t iy = 0; iy < g.ny(); ++iy) {
    for (uint32_t ix = 0; (ix + 1) < g.nx(); ++ix) {
      bool const open{ g.pass_h[(iy * (g.nx() - 1)) + ix] != 0 };
      CHECK(open == !enters_box(pt(g.xs[ix], g.ys[iy]), pt(g.xs[ix + 1], g.ys[iy]), box));
    }
  }
  for (uint32_t iy = 0; (iy + 1) < g.ny(); ++iy) {
    for (uint32_t ix = 0; ix < g.nx(); ++ix) {
      bool const open{ g.pass_v[(iy * g.nx()) + ix] != 0 };
      CHECK(open == !enters_box(pt(g.xs[ix], g.ys[iy]), pt(g.xs[ix], g.ys[iy + 1]), box));
    }
  }
}

TEST_CASE("ortho: a grid past the vertex budget is refused rather than built") {
  // Coprime strides so no lane lands on another's, and enough of them that two
  // lanes per box per axis multiply past the cap.
  std::vector<scav_rect> many;
  many.reserve(400);
  for (int32_t i = 0; i < 400; ++i) { many.push_back(rect(i * 13, i * 11, 5, 7)); }
  OrthoGrid g;
  CHECK(!ortho_grid(rect(0, 0, 8000, 8000), many, {}, 2, g));
  CHECK((Wide{ g.nx() } * g.ny()) > ORTHO_VERTEX_BUDGET);
  CHECK(g.pass_h.empty());
  CHECK(g.pass_v.empty());
}

// --- The search ---

TEST_CASE("ortho: a search to where it already is returns that one vertex") {
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), {}, {}, 5, g));
  OrthoScratch s;
  std::vector<uint32_t> path;
  REQUIRE(ortho_search(g, 0, 0, 100, s, path));
  CHECK(path == std::vector<uint32_t>{ 0 });
}

TEST_CASE("ortho: an out-of-range endpoint is refused, not clamped") {
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), {}, {}, 5, g));
  OrthoScratch s;
  std::vector<uint32_t> path{ 7 };
  CHECK(!ortho_search(g, 0, 999, 100, s, path));
  CHECK(path.empty());
  CHECK(!ortho_search(g, 999, 0, 100, s, path));
  CHECK(!ortho_search(g, 999, 999, 100, s, path));
}

TEST_CASE("ortho: an open grid gives the straight run and no turn") {
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), {}, {}, 5, g));
  REQUIRE(g.nx() == 2);
  OrthoScratch s;
  std::vector<uint32_t> path;

  // (0,0) to (100,0): one horizontal edge.
  REQUIRE(ortho_search(g, g.vertex(0, 0), g.vertex(1, 0), 500, s, path));
  CHECK(path == std::vector<uint32_t>{ g.vertex(0, 0), g.vertex(1, 0) });
  CHECK(path_cost(g, path, 500) == 100);

  // The diagonal corner costs both runs plus exactly one bend.
  REQUIRE(ortho_search(g, g.vertex(0, 0), g.vertex(1, 1), 500, s, path));
  CHECK(path.size() == 3);
  CHECK(path_cost(g, path, 500) == 700);
  check_path_is_walkable(g, path);
}

TEST_CASE("ortho: a wall with no gap is unreachable rather than crossed") {
  OrthoGrid g;
  // The obstacle spans the region's full height, so nothing gets past it.
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), { rect(40, -10, 20, 120) }, {}, 0, g));
  OrthoScratch s;
  std::vector<uint32_t> path;
  uint32_t const from{ g.vertex(0, ortho_index_of(g.ys, 50)) };
  uint32_t const to{ g.vertex(g.nx() - 1, ortho_index_of(g.ys, 50)) };
  // y = 50 is interior to the wall on both sides, and the grid has no y line
  // above or below it that is clear, so the two sides are disconnected.
  CHECK(!ortho_search(g, from, to, 500, s, path));
  CHECK(path.empty());
}

TEST_CASE("ortho: a box in the way is routed around, never through") {
  scav_rect const box{ rect(40, 40, 20, 20) };
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 100, 100), { box }, { pt(0, 50), pt(100, 50) }, 0, g));
  OrthoScratch s;
  std::vector<uint32_t> path;
  uint32_t const from{ g.vertex(ortho_index_of(g.xs, 0), ortho_index_of(g.ys, 50)) };
  uint32_t const to{ g.vertex(ortho_index_of(g.xs, 100), ortho_index_of(g.ys, 50)) };
  REQUIRE(ortho_search(g, from, to, 100, s, path));
  check_path_is_walkable(g, path);
  for (uint32_t k = 0; (k + 1) < path.size(); ++k) {
    CHECK(!enters_box(g.point(path[k]), g.point(path[k + 1]), box));
  }
  CHECK(path.size() > 2);
}

TEST_CASE("ortho: the search returns an optimal path, checked against Dijkstra") {
  // Grids small enough for the O(V^2) reference, shaped differently enough
  // that a heuristic bug or a heap bug has somewhere to show.
  for (uint32_t seed = 0; seed < 60; ++seed) {
    CAPTURE(seed);
    uint32_t const r{ mix(seed) };
    std::vector<scav_rect> boxes;
    uint32_t const count{ 1 + (r % 4U) };
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t const q{ mix((seed * 31U) + i) };
      boxes.push_back(rect(static_cast<int32_t>(10 + (q % 60U)),
                           static_cast<int32_t>(10 + ((q >> 8U) % 60U)),
                           static_cast<int32_t>(5 + ((q >> 16U) % 25U)),
                           static_cast<int32_t>(5 + ((q >> 24U) % 25U))));
    }
    OrthoGrid g;
    REQUIRE(ortho_grid(rect(0, 0, 100, 100), boxes, {}, 3, g));

    for (Wide const bend : { Wide{ 0 }, Wide{ 7 }, Wide{ 500 } }) {
      CAPTURE(bend);
      uint32_t const vertices{ g.nx() * g.ny() };
      for (uint32_t trial = 0; trial < 6; ++trial) {
        uint32_t const from{ mix((seed * 7U) + trial) % vertices };
        uint32_t const to{ mix((seed * 13U) + trial + 1U) % vertices };
        CAPTURE(from);
        CAPTURE(to);
        std::vector<uint32_t> path;
        OrthoScratch s;
        bool const found{ ortho_search(g, from, to, bend, s, path) };
        Wide const want{ reference_cost(g, from, to, bend) };
        CHECK(found == (want >= 0));
        if (!found) { continue; }
        check_path_is_walkable(g, path);
        CHECK(path.front() == from);
        CHECK(path.back() == to);
        CHECK(path_cost(g, path, bend) == want);
        for (uint32_t k = 0; (k + 1) < path.size(); ++k) {
          for (scav_rect const &box : boxes) {
            CHECK(!enters_box(g.point(path[k]), g.point(path[k + 1]), box));
          }
        }
      }
    }
  }
}

TEST_CASE("ortho: the bend penalty is what decides between two equal detours") {
  // Free bends take the shortest line whatever it costs in turns; an expensive
  // bend prefers the route that turns less even where that is longer.
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 200, 200), { rect(50, 90, 100, 20) }, {}, 0, g));
  OrthoScratch s;
  std::vector<uint32_t> cheap;
  std::vector<uint32_t> dear;
  uint32_t const from{ g.vertex(ortho_index_of(g.xs, 0), ortho_index_of(g.ys, 100)) };
  uint32_t const to{ g.vertex(ortho_index_of(g.xs, 200), ortho_index_of(g.ys, 100)) };
  REQUIRE(ortho_search(g, from, to, 0, s, cheap));
  REQUIRE(ortho_search(g, from, to, 100000, s, dear));

  CHECK(path_cost(g, cheap, 0) <= path_cost(g, dear, 0));
  CHECK(path_cost(g, dear, 100000) <= path_cost(g, cheap, 100000));
}

TEST_CASE("ortho: no passable edge comes within a clearance of any box") {
  // The bumper's invariant over the whole table, not just the routes asked for.
  // Anchors sit on the box's own edges, the case that lays a border line at all.
  scav_rect const box{ rect(400, 400, 200, 200) };
  int32_t const clear{ 50 };
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 1000, 1000),
                     { box },
                     { pt(0, 400), pt(1000, 600), pt(400, 0) },
                     clear,
                     g));
  scav_rect const bumper{ .x = box.x - clear,
                          .y = box.y - clear,
                          .w = box.w + (2 * clear),
                          .h = box.h + (2 * clear) };
  for (uint32_t iy = 0; iy < g.ny(); ++iy) {
    for (uint32_t ix = 0; (ix + 1) < g.nx(); ++ix) {
      CAPTURE(ix);
      CAPTURE(iy);
      if (g.pass_h[(iy * (g.nx() - 1)) + ix] == 0) { continue; }
      CHECK(!enters_box(pt(g.xs[ix], g.ys[iy]), pt(g.xs[ix + 1], g.ys[iy]), bumper));
    }
  }
  for (uint32_t iy = 0; (iy + 1) < g.ny(); ++iy) {
    for (uint32_t ix = 0; ix < g.nx(); ++ix) {
      CAPTURE(ix);
      CAPTURE(iy);
      if (g.pass_v[(iy * g.nx()) + ix] == 0) { continue; }
      CHECK(!enters_box(pt(g.xs[ix], g.ys[iy]), pt(g.xs[ix], g.ys[iy + 1]), bumper));
    }
  }
}

TEST_CASE("ortho: zero clearance is a real mode, and it is the re-seat") {
  // Two boxes one clearance apart seal the channel when both are grown; no bumper
  // opens it again. That is 11.5's degradation, and why a second grid exists.
  std::vector<scav_rect> const boxes{ rect(0, 400, 400, 200), rect(600, 400, 400, 200) };
  std::vector<scav_point> const anchors{ pt(500, 0), pt(500, 1000) };
  OrthoGrid sealed;
  OrthoGrid open;
  REQUIRE(ortho_grid(rect(0, 0, 1000, 1000), boxes, anchors, 150, sealed));
  REQUIRE(ortho_grid(rect(0, 0, 1000, 1000), boxes, anchors, 0, open));

  OrthoScratch s;
  std::vector<uint32_t> path;
  auto const ends = [](OrthoGrid const &g) {
    return std::pair<uint32_t, uint32_t>{
      g.vertex(ortho_index_of(g.xs, 500), ortho_index_of(g.ys, 0)),
      g.vertex(ortho_index_of(g.xs, 500), ortho_index_of(g.ys, 1000))
    };
  };
  auto const [sa, sb] = ends(sealed);
  auto const [oa, ob] = ends(open);
  CHECK(!ortho_search(sealed, sa, sb, 500, s, path));
  CHECK(ortho_search(open, oa, ob, 500, s, path));
}

TEST_CASE("ortho: a reused scratch answers exactly as a fresh one does") {
  // The generation stamp is what makes reuse safe; a stale entry read as live
  // would be a wrong answer that only shows on the second call.
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 120, 120), { rect(40, 40, 30, 30) }, {}, 4, g));
  uint32_t const vertices{ g.nx() * g.ny() };

  OrthoScratch shared;
  for (uint32_t trial = 0; trial < 40; ++trial) {
    CAPTURE(trial);
    uint32_t const from{ mix(trial) % vertices };
    uint32_t const to{ mix(trial + 9999U) % vertices };
    std::vector<uint32_t> reused;
    std::vector<uint32_t> fresh;
    OrthoScratch alone;
    bool const a{ ortho_search(g, from, to, 300, shared, reused) };
    bool const b{ ortho_search(g, from, to, 300, alone, fresh) };
    CHECK(a == b);
    CHECK(reused == fresh);
  }
}

TEST_CASE("ortho: two identical searches return identical paths") {
  OrthoGrid g;
  REQUIRE(ortho_grid(rect(0, 0, 150, 150),
                     { rect(30, 30, 20, 20), rect(80, 60, 25, 40) },
                     {},
                     6,
                     g));
  uint32_t const vertices{ g.nx() * g.ny() };
  for (uint32_t trial = 0; trial < 30; ++trial) {
    CAPTURE(trial);
    uint32_t const from{ mix(trial * 3U) % vertices };
    uint32_t const to{ mix((trial * 5U) + 1U) % vertices };
    OrthoScratch s1;
    OrthoScratch s2;
    std::vector<uint32_t> a;
    std::vector<uint32_t> b;
    CHECK(ortho_search(g, from, to, 200, s1, a) == ortho_search(g, from, to, 200, s2, b));
    CHECK(a == b);
  }
}

// --- The whole verb ---

namespace {

OrthogonalRouter const ORTHO;

// Hugging is free on length and bends, so nothing in the cost vector forbids
// it; the grid has to, by never laying a lane there.
void check_no_segment_hugs_a_box(RouteInput const &in, RouteOutput const &out) {
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    CAPTURE(n);
    // A re-seated net gave up its clearance to route at all, so it is allowed
    // the flush lane this otherwise forbids (11.5).
    if (out.metrics[n].failed != RouteFailure::None) { continue; }
    if (out.metrics[n].reseated != 0) { continue; }
    scav_span const at{ out.net_points[n] };
    for (uint32_t k = 0; (k + 1) < at.len; ++k) {
      scav_point const a{ out.points[at.off + k] };
      scav_point const b{ out.points[at.off + k + 1] };
      for (uint32_t i = 0; i < in.obstacles.size(); ++i) {
        scav_rect const &r{ in.obstacles[i] };
        CAPTURE(i);
        CAPTURE(k);
        if (a.y == b.y) {
          bool const on_cap{ (a.y == r.y) || (a.y == (r.y + r.h)) };
          bool const overlaps{ (imin(a.x, b.x) < (r.x + r.w)) && (imax(a.x, b.x) > r.x) };
          CHECK(!(on_cap && overlaps));
        } else {
          bool const on_side{ (a.x == r.x) || (a.x == (r.x + r.w)) };
          bool const overlaps{ (imin(a.y, b.y) < (r.y + r.h)) && (imax(a.y, b.y) > r.y) };
          CHECK(!(on_side && overlaps));
        }
      }
    }
  }
}

// The bumper's promise over finished routes. The first and last legs run from a
// border out to the ring and cross it by design; nothing else may touch it.
void check_keeps_its_clearance(RouteInput const &in, RouteOutput const &out) {
  int32_t const clear{ ortho_clearance(in.profile) };
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    CAPTURE(n);
    if (out.metrics[n].failed != RouteFailure::None) { continue; }
    if (out.metrics[n].reseated != 0) { continue; }
    scav_span const at{ out.net_points[n] };
    if (at.len < 3) { continue; }
    for (uint32_t k = 1; (k + 2) < at.len; ++k) {
      scav_point const a{ out.points[at.off + k] };
      scav_point const b{ out.points[at.off + k + 1] };
      for (uint32_t i = 0; i < in.obstacles.size(); ++i) {
        scav_rect const &r{ in.obstacles[i] };
        CAPTURE(i);
        CAPTURE(k);
        CHECK(!enters_box(
            a,
            b,
            rect(r.x - clear, r.y - clear, r.w + (2 * clear), r.h + (2 * clear))));
      }
    }
  }
}

// A route ending on a border meets it square. Arriving *along* it is what an
// unconstrained search does whenever the border is also a lane.
void check_ends_meet_boxes_square(RouteInput const &in, RouteOutput const &out) {
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    CAPTURE(n);
    if (out.metrics[n].failed != RouteFailure::None) { continue; }
    scav_span const at{ out.net_points[n] };
    if (at.len < 2) { continue; }
    for (uint32_t end = 0; end < 2; ++end) {
      CAPTURE(end);
      scav_point const tip{ (end == 0) ? out.points[at.off]
                                       : out.points[at.off + at.len - 1] };
      scav_point const next{ (end == 0) ? out.points[at.off + 1]
                                        : out.points[at.off + at.len - 2] };
      uint32_t const box{ ortho_box_at(tip, in.obstacles) };
      if (box == INVALID) { continue; }
      scav_rect const &r{ in.obstacles[box] };
      bool const on_side{ (tip.x == r.x) || (tip.x == (r.x + r.w)) };
      bool const on_cap{ (tip.y == r.y) || (tip.y == (r.y + r.h)) };
      bool const leg_horizontal{ tip.y == next.y };
      // A corner satisfies either reading, so it is asked for neither.
      if (on_side && !on_cap) { CHECK(leg_horizontal); }
      if (on_cap && !on_side) { CHECK(!leg_horizontal); }
    }
  }
}

// Every segment of every routed net, against every obstacle it was given.
void check_no_net_enters_a_box(RouteInput const &in, RouteOutput const &out) {
  REQUIRE(out.net_points.size() == in.nets.size());
  REQUIRE(out.metrics.size() == in.nets.size());
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    CAPTURE(n);
    if (out.metrics[n].failed != RouteFailure::None) { continue; }
    scav_span const at{ out.net_points[n] };
    REQUIRE(at.len >= 1);
    for (uint32_t k = 0; (k + 1) < at.len; ++k) {
      scav_point const a{ out.points[at.off + k] };
      scav_point const b{ out.points[at.off + k + 1] };
      for (uint32_t i = 0; i < in.obstacles.size(); ++i) {
        CAPTURE(i);
        CHECK(!enters_box(a, b, in.obstacles[i]));
      }
    }
  }
}

}  // namespace

TEST_CASE("ortho: a router with nothing to do produces nothing") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 100, 100);
  RouteOutput out;
  out.points.push_back(pt(5, 5));  // cleared by the callee, per the contract
  ORTHO.route(in, out);
  CHECK(out.points.empty());
  CHECK(out.net_points.empty());
  CHECK(out.metrics.empty());
}

TEST_CASE("ortho: an unobstructed net is the straight line between its ends") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 1000, 1000);
  in.nets.push_back({ .src = pt(0, 500), .dst = pt(1000, 500) });
  RouteOutput out;
  ORTHO.route(in, out);

  REQUIRE(out.net_points.size() == 1);
  CHECK(out.metrics[0].failed == RouteFailure::None);
  CHECK(out.metrics[0].bends == 0);
  CHECK(out.metrics[0].length == 1000);
  CHECK(out.net_points[0].len == 2);
  CHECK((out.points[0] == pt(0, 500)));
  CHECK((out.points[1] == pt(1000, 500)));
}

TEST_CASE("ortho: a net whose ends name boxes starts and ends on their borders") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 1000, 400);
  in.obstacles.push_back(rect(100, 150, 100, 100));  // 0
  in.obstacles.push_back(rect(700, 150, 100, 100));  // 1
  in.nets.push_back(
      { .src = pt(150, 200), .dst = pt(750, 200), .src_obstacle = 0, .dst_obstacle = 1 });
  RouteOutput out;
  ORTHO.route(in, out);

  REQUIRE(out.net_points.size() == 1);
  CHECK(out.metrics[0].failed == RouteFailure::None);
  scav_span const at{ out.net_points[0] };
  // The centres it was handed are not where an edge meets a box.
  CHECK((out.points[at.off] == pt(200, 200)));
  CHECK((out.points[at.off + at.len - 1] == pt(700, 200)));
  check_no_net_enters_a_box(in, out);
}

TEST_CASE("ortho: a box between two ends is routed around it") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 1000, 1000);
  in.obstacles.push_back(rect(400, 300, 200, 400));
  in.nets.push_back({ .src = pt(0, 500), .dst = pt(1000, 500) });
  RouteOutput out;
  ORTHO.route(in, out);

  REQUIRE(out.net_points.size() == 1);
  CHECK(out.metrics[0].failed == RouteFailure::None);
  CHECK(out.metrics[0].bends >= 2);
  CHECK(out.net_points[0].len > 2);
  check_no_net_enters_a_box(in, out);
}

TEST_CASE("ortho: waypoints are threaded, in the order they were given") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 1000, 1000);
  in.waypoints.push_back(pt(300, 100));
  in.waypoints.push_back(pt(700, 900));
  in.nets.push_back(
      { .src = pt(0, 500), .dst = pt(1000, 500), .waypoint_off = 0, .waypoint_len = 2 });
  RouteOutput out;
  ORTHO.route(in, out);

  REQUIRE(out.net_points.size() == 1);
  CHECK(out.metrics[0].failed == RouteFailure::None);
  scav_span const at{ out.net_points[0] };
  // Both waypoints appear, and the first one before the second.
  uint32_t first{ INVALID };
  uint32_t second{ INVALID };
  for (uint32_t k = 0; k < at.len; ++k) {
    if (out.points[at.off + k] == pt(300, 100)) { first = k; }
    if (out.points[at.off + k] == pt(700, 900)) { second = k; }
  }
  REQUIRE(first != INVALID);
  REQUIRE(second != INVALID);
  CHECK(first < second);
}

TEST_CASE("ortho: an anchor outside the region degrades that net and only it") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 100, 100);
  in.nets.push_back({ .src = pt(0, 50), .dst = pt(100, 50) });
  in.nets.push_back({ .src = pt(0, 50), .dst = pt(5000, 50) });
  RouteOutput out;
  ORTHO.route(in, out);

  REQUIRE(out.net_points.size() == 2);
  CHECK(out.metrics[0].failed == RouteFailure::None);
  CHECK(out.metrics[1].failed == RouteFailure::OutsideRegion);
  // The fallback is the straight line it was asked for, so the cost scorer
  // sees the violation rather than a shape that hides it.
  scav_span const at{ out.net_points[1] };
  CHECK(at.len == 2);
  CHECK((out.points[at.off] == pt(0, 50)));
  CHECK((out.points[at.off + 1] == pt(5000, 50)));
}

TEST_CASE("ortho: a grid past the budget degrades every net in the frame") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 6000, 6000);
  // Coprime strides, so no box's side or clearance lane lands on another's and
  // the line sets really do multiply out past the cap.
  for (int32_t i = 0; i < 300; ++i) { in.obstacles.push_back(rect(i * 13, i * 11, 5, 7)); }
  in.nets.push_back({ .src = pt(0, 3000), .dst = pt(6000, 3000) });
  RouteOutput out;
  ORTHO.route(in, out);

  REQUIRE(out.metrics.size() == 1);
  CHECK(out.metrics[0].failed == RouteFailure::TooLarge);
  CHECK(out.net_points[0].len == 2);
}

TEST_CASE("ortho: the reported metrics match an independent recount") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 1000, 1000);
  in.obstacles.push_back(rect(400, 300, 200, 400));
  in.nets.push_back({ .src = pt(0, 500), .dst = pt(1000, 500) });
  in.nets.push_back({ .src = pt(0, 0), .dst = pt(1000, 1000) });
  RouteOutput out;
  ORTHO.route(in, out);

  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    CAPTURE(n);
    scav_span const at{ out.net_points[n] };
    Wide length{ 0 };
    int32_t bends{ 0 };
    uint32_t previous{ 9 };
    for (uint32_t k = 0; (k + 1) < at.len; ++k) {
      scav_point const a{ out.points[at.off + k] };
      scav_point const b{ out.points[at.off + k + 1] };
      Wide const dx{ (a.x < b.x) ? (Wide{ b.x } - a.x) : (Wide{ a.x } - b.x) };
      Wide const dy{ (a.y < b.y) ? (Wide{ b.y } - a.y) : (Wide{ a.y } - b.y) };
      length += dx + dy;
      uint32_t const axis{ (a.x == b.x) ? 1U : 0U };
      if ((k > 0) && (axis != previous)) { ++bends; }
      previous = axis;
    }
    CHECK(out.metrics[n].length == length);
    CHECK(out.metrics[n].bends == bends);
  }
}

TEST_CASE("ortho: a polyline never carries the same point twice in a row") {
  // A net whose two ends resolve to one place is a real input, and a zero-length
  // segment has no direction for anything downstream to read.
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 2000, 2000);
  in.obstacles.push_back(rect(500, 500, 400, 400));
  in.nets.push_back(
      { .src = pt(700, 700), .dst = pt(700, 700), .src_obstacle = 0, .dst_obstacle = 0 });
  RouteOutput out;
  ORTHO.route(in, out);

  REQUIRE(out.net_points.size() == 1);
  scav_span const at{ out.net_points[0] };
  REQUIRE(at.len >= 1);
  for (uint32_t k = 0; (k + 1) < at.len; ++k) {
    CAPTURE(k);
    bool const same{ (out.points[at.off + k].x == out.points[at.off + k + 1].x) &&
                     (out.points[at.off + k].y == out.points[at.off + k + 1].y) };
    CHECK(!same);
  }
}

TEST_CASE("ortho: the same frame routed twice comes out identical") {
  RouteInput in;
  in.profile = profile();
  in.region = rect(0, 0, 800, 800);
  in.obstacles.push_back(rect(100, 100, 200, 150));
  in.obstacles.push_back(rect(450, 300, 180, 220));
  for (uint32_t i = 0; i < 12; ++i) {
    uint32_t const q{ mix(i) };
    in.nets.push_back({ .src = pt(0, static_cast<int32_t>(q % 800U)),
                        .dst = pt(800, static_cast<int32_t>((q >> 12U) % 800U)) });
  }
  RouteOutput a;
  RouteOutput b;
  ORTHO.route(in, a);
  ORTHO.route(in, b);

  REQUIRE(a.points.size() == b.points.size());
  for (uint32_t i = 0; i < a.points.size(); ++i) { CHECK(a.points[i] == b.points[i]); }
  for (uint32_t i = 0; i < a.metrics.size(); ++i) {
    CHECK(a.metrics[i].bends == b.metrics[i].bends);
    CHECK(a.metrics[i].length == b.metrics[i].length);
    CHECK(a.metrics[i].failed == b.metrics[i].failed);
  }
}

TEST_CASE("ortho: no generated frame routes a segment through a box") {
  // The invariant the phase exists for, over frames the cases above do not
  // describe: rows of boxes with lanes between, nets between arbitrary pairs.
  scav_profile const p{ profile() };
  for (uint32_t seed = 0; seed < 50; ++seed) {
    CAPTURE(seed);
    uint32_t const r{ mix(seed) };
    RouteInput in;
    in.profile = p;
    in.region = rect(0, 0, 2000, 2000);

    uint32_t const rows{ 2 + (r % 3U) };
    uint32_t const cols{ 2 + ((r >> 4U) % 3U) };
    for (uint32_t row = 0; row < rows; ++row) {
      for (uint32_t col = 0; col < cols; ++col) {
        uint32_t const q{ mix((seed * 101U) + (row * 7U) + col) };
        in.obstacles.push_back(rect(static_cast<int32_t>(150 + (col * 500U)),
                                    static_cast<int32_t>(150 + (row * 500U)),
                                    static_cast<int32_t>(120 + (q % 200U)),
                                    static_cast<int32_t>(120 + ((q >> 8U) % 200U))));
      }
    }
    for (uint32_t net = 0; net < 8; ++net) {
      uint32_t const q{ mix((seed * 977U) + net) };
      auto const boxes{ static_cast<uint32_t>(in.obstacles.size()) };
      uint32_t const from{ q % boxes };
      uint32_t const to{ (q >> 8U) % boxes };
      scav_rect const &a{ in.obstacles[from] };
      scav_rect const &b{ in.obstacles[to] };
      in.nets.push_back({ .src = pt(a.x + (a.w / 2), a.y + (a.h / 2)),
                          .dst = pt(b.x + (b.w / 2), b.y + (b.h / 2)),
                          .src_obstacle = from,
                          .dst_obstacle = to });
    }

    RouteOutput out;
    ORTHO.route(in, out);
    check_no_net_enters_a_box(in, out);
    check_no_segment_hugs_a_box(in, out);
    check_ends_meet_boxes_square(in, out);
    check_keeps_its_clearance(in, out);
    // Every one of these is routable, so a fallback here is a real failure and
    // not a shape the generator happened to make impossible.
    for (uint32_t n = 0; n < in.nets.size(); ++n) {
      CAPTURE(n);
      if (in.nets[n].src_obstacle == in.nets[n].dst_obstacle) { continue; }
      CHECK(out.metrics[n].failed == RouteFailure::None);
    }
  }
}

TEST_CASE("ortho: every route stays inside the region it was given") {
  scav_profile const p{ profile() };
  for (uint32_t seed = 0; seed < 25; ++seed) {
    CAPTURE(seed);
    RouteInput in;
    in.profile = p;
    in.region = rect(-500, -500, 1500, 1200);
    in.obstacles.push_back(rect(-100, -100, 300, 300));
    in.obstacles.push_back(rect(400, 200, 250, 350));
    for (uint32_t net = 0; net < 6; ++net) {
      uint32_t const q{ mix((seed * 61U) + net) };
      in.nets.push_back(
          { .src = pt(-500, static_cast<int32_t>(q % 1200U) - 500),
            .dst = pt(1000, static_cast<int32_t>((q >> 11U) % 1200U) - 500) });
    }
    RouteOutput out;
    ORTHO.route(in, out);
    for (uint32_t n = 0; n < in.nets.size(); ++n) {
      CAPTURE(n);
      if (out.metrics[n].failed != RouteFailure::None) { continue; }
      scav_span const at{ out.net_points[n] };
      for (uint32_t k = 0; k < at.len; ++k) {
        scav_point const q{ out.points[at.off + k] };
        CHECK(q.x >= in.region.x);
        CHECK(q.x <= (in.region.x + in.region.w));
        CHECK(q.y >= in.region.y);
        CHECK(q.y <= (in.region.y + in.region.h));
      }
    }
    check_no_net_enters_a_box(in, out);
    check_no_segment_hugs_a_box(in, out);
  }
}

TEST_CASE("ortho: an end on a box leaves it square, one clearance out") {
  scav_profile const p{ profile() };
  int32_t const clear{ ortho_clearance(p) };
  RouteInput in;
  in.profile = p;
  in.region = rect(0, 0, 4000, 2000);
  in.obstacles.push_back(rect(500, 800, 400, 400));   // 0
  in.obstacles.push_back(rect(2500, 800, 400, 400));  // 1
  in.nets.push_back({ .src = pt(700, 1000),
                      .dst = pt(2700, 1000),
                      .src_obstacle = 0,
                      .dst_obstacle = 1 });
  RouteOutput out;
  ORTHO.route(in, out);

  REQUIRE(out.metrics[0].failed == RouteFailure::None);
  scav_span const at{ out.net_points[0] };
  REQUIRE(at.len >= 2);
  // The polyline touches the borders it was aimed at, not the centres.
  CHECK((out.points[at.off] == pt(900, 1000)));
  CHECK((out.points[at.off + at.len - 1] == pt(2500, 1000)));
  // The touching legs are perpendicular and one clearance long, which is why the
  // search runs between ring points rather than between the borders.
  CHECK(out.points[at.off + 1].y == 1000);
  CHECK(out.points[at.off + 1].x >= (900 + clear));
  check_ends_meet_boxes_square(in, out);
  check_no_segment_hugs_a_box(in, out);
}

TEST_CASE("ortho: a route squeezed past a box does not run along its edge") {
  // The shape that produced the complaint: a box directly in the way, with the
  // shortest legal path being the one flush against its side.
  scav_profile const p{ profile() };
  RouteInput in;
  in.profile = p;
  in.region = rect(0, 0, 4000, 3000);
  in.obstacles.push_back(rect(1000, 1000, 2000, 1000));
  in.nets.push_back({ .src = pt(0, 1000), .dst = pt(4000, 1000) });
  in.nets.push_back({ .src = pt(0, 2000), .dst = pt(4000, 2000) });
  in.nets.push_back({ .src = pt(1000, 0), .dst = pt(1000, 3000) });
  RouteOutput out;
  ORTHO.route(in, out);

  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    CAPTURE(n);
    CHECK(out.metrics[n].failed == RouteFailure::None);
  }
  check_no_net_enters_a_box(in, out);
  check_no_segment_hugs_a_box(in, out);
  check_keeps_its_clearance(in, out);
}

TEST_CASE("ortho: the margin a router asks for is the clearance it will use") {
  // Phase 3 grows the region by exactly this. Larger and the canvas carries
  // space nothing draws in; smaller and a box on the frame edge has no lane.
  scav_profile const p{ profile() };
  OrthogonalRouter const ortho;
  StraightRouter const straight;
  CHECK(ortho.margin(p) == ortho_clearance(p));
  // A router that lays no lanes needs no room to lay them in.
  CHECK(straight.margin(p) == 0);
}

TEST_CASE("ortho: the profile-derived knobs are the two documented numbers") {
  scav_profile p{ profile() };
  CHECK(ortho_bend_penalty(p) == p.rank_sep);
  CHECK(ortho_clearance(p) == (p.node_sep / 3));

  // The clearance must leave a channel between two boxes one node separation
  // apart, or the bumpers seal every lane the layering left.
  CHECK((2 * ortho_clearance(p)) < p.node_sep);

  // Neither may reach zero: a zero bend penalty makes a staircase free, and a
  // zero clearance would put two grid lines on top of each other.
  p.rank_sep = 0;
  p.node_sep = 0;
  CHECK(ortho_bend_penalty(p) == 1);
  CHECK(ortho_clearance(p) == 1);
}
