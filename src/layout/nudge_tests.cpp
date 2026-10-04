// The nudging stage alone, on hand-written polylines and boxes.

#include "layout/nudge.h"

#include "layout/geom.h"

#include "scav_int.h"

#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace scav;

scav_point pt(int32_t x, int32_t y) { return { .x = x, .y = y }; }

scav_rect rect(int32_t x, int32_t y, int32_t w, int32_t h) {
  return { .x = x, .y = y, .w = w, .h = h };
}

// Two nets sharing the interior segment (0,y)-(200,y), with a leg at either end.
struct Lane {
  std::vector<scav_point> points;
  std::vector<scav_span> nets;
};

Lane two_over(int32_t y) {
  Lane l;
  l.points = { pt(0, 0),   pt(0, y), pt(200, y), pt(200, 300),
               pt(0, 400), pt(0, y), pt(200, y), pt(200, 500) };
  l.nets = { scav_span{ .off = 0, .len = 4 }, scav_span{ .off = 4, .len = 4 } };
  return l;
}

int32_t lane_y(Lane const &l, uint32_t net) { return l.points[l.nets[net].off + 1].y; }

// One net per polyline, laid end to end in one point list.
struct Frame {
  std::vector<scav_point> points;
  std::vector<scav_span> nets;
};

Frame frame_of(std::vector<std::vector<scav_point>> const &lines) {
  Frame f;
  for (std::vector<scav_point> const &line : lines) {
    f.nets.push_back({ .off = static_cast<uint32_t>(f.points.size()),
                       .len = static_cast<uint32_t>(line.size()) });
    for (scav_point const &at : line) { f.points.push_back(at); }
  }
  return f;
}

scav_point net_pt(Frame const &f, uint32_t net, uint32_t k) {
  return f.points[f.nets[net].off + k];
}

// One frame's box repeated for every net it routes, which is what a per-frame
// call passes; the chart-wide pass gives each net its own.
std::vector<scav_rect> bounds_of(scav_rect const &box,
                                 std::vector<scav_span> const &nets) {
  std::vector<scav_rect> every(nets.size(), box);
  return every;
}

// Far enough out that only the obstacles bound a fixture.
scav_rect const OPEN{ rect(-1000, -1000, 3000, 3000) };

bool same(std::vector<scav_point> const &a, std::vector<scav_point> const &b) {
  if (a.size() != b.size()) { return false; }
  for (size_t i = 0; i < a.size(); ++i) {
    if ((a[i].x != b[i].x) || (a[i].y != b[i].y)) { return false; }
  }
  return true;
}

}  // namespace

TEST_CASE("nudge: two nets sharing a lane come off it in opposite directions") {
  Lane l{ two_over(100) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), {}, 48, 0, l.nets, l.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.spread == 1);
  CHECK(s.moved == 2);
  // Spread apart and centred on y=100.
  CHECK(lane_y(l, 0) != lane_y(l, 1));
  CHECK((lane_y(l, 0) + lane_y(l, 1)) == 200);
  // The dragged legs stay axis-aligned.
  for (uint32_t net = 0; net < 2; ++net) {
    scav_span const at{ l.nets[net] };
    for (uint32_t k = 0; (k + 1) < at.len; ++k) {
      scav_point const a{ l.points[at.off + k] };
      scav_point const b{ l.points[at.off + k + 1] };
      CHECK(((a.x == b.x) || (a.y == b.y)));
    }
  }
}

TEST_CASE("nudge: routes a hair apart are one lane, not two") {
  // Lane membership is by distance: segments 6 apart, within the 160 gap, form one lane.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(400, 100), pt(400, 300) },
                      { pt(0, 500), pt(0, 106), pt(400, 106), pt(400, 800) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 160, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(imax(net_pt(f, 0, 1).y - net_pt(f, 1, 1).y,
             net_pt(f, 1, 1).y - net_pt(f, 0, 1).y) == 160);
}

TEST_CASE("nudge: a lane is every member that overlaps, not a run that stops") {
  // Sorted by y the members are nets 0, 1, 2; net 2 overlaps net 0 in x and net 1
  // overlaps neither.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 400) },
                      { pt(900, 0), pt(900, 104), pt(1100, 104), pt(1100, 400) },
                      { pt(50, 600), pt(50, 108), pt(250, 108), pt(250, 900) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 160, 0, f.nets, f.points, s);

  // Nets 0 and 2 land a pitch apart; net 1 stays at y=104.
  CHECK(imax(net_pt(f, 0, 1).y - net_pt(f, 2, 1).y,
             net_pt(f, 2, 1).y - net_pt(f, 0, 1).y) == 160);
  CHECK(net_pt(f, 1, 1).y == 104);
}

TEST_CASE("nudge: two lanes interleaved by coordinate keep every member of each") {
  // Sorted by y the members alternate between a lane on the left and one on the right;
  // their legs' crossing votes agree with the key order.
  Frame f{ frame_of({ { pt(0, 600), pt(0, 100), pt(200, 100), pt(200, -500) },
                      { pt(900, 600), pt(900, 102), pt(1100, 102), pt(1100, -500) },
                      { pt(20, 600), pt(20, 104), pt(220, 104), pt(220, -500) },
                      { pt(920, 600), pt(920, 106), pt(1120, 106), pt(1120, -500) },
                      { pt(40, 600), pt(40, 108), pt(240, 108), pt(240, -500) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 160, 0, f.nets, f.points, s);

  CHECK(s.lanes == 2);
  CHECK(s.spread == 2);
  CHECK(s.moved == 5);
  // Left lane: three members a pitch apart centred on y=100; right lane: two centred
  // on y=102.
  CHECK(net_pt(f, 0, 1).y == -60);
  CHECK(net_pt(f, 2, 1).y == 100);
  CHECK(net_pt(f, 4, 1).y == 260);
  CHECK(net_pt(f, 1, 1).y == 22);
  CHECK(net_pt(f, 3, 1).y == 182);
}

TEST_CASE("nudge: a run's one-sided reach does not bundle disjoint extents") {
  // Disjoint extents in x: no lane forms and neither member moves.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 400) },
                      { pt(900, 0), pt(900, 104), pt(1100, 104), pt(1100, 400) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 160, 0, f.nets, f.points, s);

  CHECK(s.lanes == 0);
  CHECK(s.moved == 0);
  CHECK(net_pt(f, 0, 1).y == 100);
  CHECK(net_pt(f, 1, 1).y == 104);
}

TEST_CASE("nudge: a lane two coordinates wide spreads by the pitch, not past it") {
  // Members 150 apart in one lane, with the slot order the reverse of the `at` order.
  Frame f{ frame_of({ { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 600) },
                      { pt(0, 200), pt(0, 250), pt(200, 250), pt(200, -100) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 160, 0, f.nets, f.points, s);

  REQUIRE(s.lanes == 1);
  int32_t const one{ net_pt(f, 0, 1).y };
  int32_t const two{ net_pt(f, 1, 1).y };
  CHECK(imax(one - two, two - one) == 160);
}

TEST_CASE("nudge: a lane with no room keeps its members stacked") {
  // Boxes on both sides touch the lane at y=100: no room either way.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const walls{ rect(0, 0, 200, 100), rect(0, 100, 200, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), walls, 48, 0, l.nets, l.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.spread == 0);
  CHECK(s.moved == 0);
  CHECK(lane_y(l, 0) == 100);
  CHECK(lane_y(l, 1) == 100);
}

TEST_CASE("nudge: a lane with room on one side only slides onto that side") {
  // A box touches the lane from above: the members spread downward from y=100.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const wall{ rect(0, 0, 200, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), wall, 48, 0, l.nets, l.points, s);

  CHECK(s.spread == 1);
  CHECK(lane_y(l, 0) != lane_y(l, 1));
  // Both stay at y >= 100, below the box, one gap apart.
  CHECK(lane_y(l, 0) >= 100);
  CHECK(lane_y(l, 1) >= 100);
  int32_t const lo{ lane_y(l, 0) < lane_y(l, 1) ? lane_y(l, 0) : lane_y(l, 1) };
  int32_t const hi{ lane_y(l, 0) < lane_y(l, 1) ? lane_y(l, 1) : lane_y(l, 0) };
  CHECK(lo == 100);
  CHECK(hi == 148);
}

TEST_CASE("nudge: clearance is kept, so a displacement never ends up flush") {
  // `clear` 48 below a box ending at y=40 bounds the lane at y=88; a centred 48 step
  // would put the upper member at 76.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const wall{ rect(0, 0, 200, 40) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), wall, 48, 48, l.nets, l.points, s);
  for (uint32_t net = 0; net < 2; ++net) { CHECK(lane_y(l, net) >= 88); }
}

TEST_CASE("nudge: a displacement never drags a leg onto a box's border") {
  // The box's left side lies on x=200, the line of the lane's right legs; moving the upper
  // member up 48, half the pitch, runs its leg 18 units along that side.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const wall{ rect(200, 30, 100, 40) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), wall, 96, 0, l.nets, l.points, s);

  CHECK(lane_y(l, 0) != lane_y(l, 1));
  for (uint32_t net = 0; net < 2; ++net) {
    scav_span const at{ l.nets[net] };
    for (uint32_t k = 0; (k + 1) < at.len; ++k) {
      CAPTURE(net);
      CAPTURE(k);
      CHECK_FALSE(along_border(l.points[at.off + k], l.points[at.off + k + 1], wall[0]));
    }
  }
}

TEST_CASE("nudge: the step shrinks to the room rather than being refused") {
  // Room is 19 either side, one unit short of each box: the step is 38 of the 480
  // asked for.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const walls{ rect(0, 0, 200, 80), rect(0, 120, 200, 80) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), walls, 480, 0, l.nets, l.points, s);

  CHECK(s.spread == 1);
  CHECK(s.moved == 2);
  int32_t const lo{ lane_y(l, 0) < lane_y(l, 1) ? lane_y(l, 0) : lane_y(l, 1) };
  int32_t const hi{ lane_y(l, 0) < lane_y(l, 1) ? lane_y(l, 1) : lane_y(l, 0) };
  CHECK(lo >= 80);
  CHECK(hi <= 120);
  CHECK(lo != hi);
}

TEST_CASE("nudge: the region bounds a lane the obstacles do not") {
  Lane l{ two_over(100) };
  NudgeStats s;
  nudge_lanes(rect(0, 90, 200, 20),
              bounds_of(rect(0, 90, 200, 20), l.nets),
              {},
              480,
              0,
              l.nets,
              l.points,
              s);
  // Room is 9 either side of y=100, one unit inside the region; both members stay in it.
  for (uint32_t net = 0; net < 2; ++net) {
    CHECK(lane_y(l, net) >= 90);
    CHECK(lane_y(l, net) <= 110);
  }
}

TEST_CASE("nudge: an end segment is left alone, having a border to hold") {
  // Three points is one interior-free polyline: both segments touch an end.
  std::vector<scav_point> points{ pt(0, 100), pt(200, 100), pt(200, 300),
                                  pt(0, 100), pt(200, 100), pt(200, 500) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 3 },
                                     scav_span{ .off = 3, .len = 3 } };
  std::vector<scav_point> const before{ points };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, nets), {}, 48, 0, nets, points, s);
  CHECK(s.lanes == 0);
  CHECK(s.moved == 0);
  CHECK(same(points, before));
}

TEST_CASE("nudge: nets that only touch at a point are not one lane") {
  // Collinear segments on y=100 over x [0,100] and [200,300], 100 apart.
  std::vector<scav_point> points{ pt(0, 0),     pt(0, 100),   pt(100, 100), pt(100, 300),
                                  pt(200, 400), pt(200, 100), pt(300, 100), pt(300, 500) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 } };
  std::vector<scav_point> const before{ points };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, nets), {}, 48, 0, nets, points, s);
  CHECK(s.lanes == 0);
  CHECK(same(points, before));
}

TEST_CASE("nudge: a displacement that would enter a box is dropped, not clamped") {
  Lane l{ two_over(100) };
  // A box below the lane, across both nets' trailing legs at x=200.
  std::vector<scav_rect> const walls{ rect(150, 130, 100, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), walls, 48, 0, l.nets, l.points, s);
  CHECK(s.lanes == 1);
  // No segment lies wholly inside the box.
  for (scav_span const &net : l.nets) {
    for (uint32_t k = 0; (k + 1) < net.len; ++k) {
      scav_point const a{ l.points[net.off + k] };
      scav_point const b{ l.points[net.off + k + 1] };
      bool const in_x{ (a.x > 150) && (a.x < 250) && (b.x > 150) && (b.x < 250) };
      bool const in_y{ (a.y > 130) && (a.y < 230) && (b.y > 130) && (b.y < 230) };
      CHECK(!(in_x && in_y));
    }
  }
}

TEST_CASE("nudge: the same input twice is the same output") {
  Lane a{ two_over(100) };
  Lane b{ two_over(100) };
  // The same nets offered in the other order; the keys read points, not net order.
  std::vector<scav_span> const swapped{ b.nets[1], b.nets[0] };
  NudgeStats sa;
  NudgeStats sb;
  nudge_lanes(OPEN, bounds_of(OPEN, a.nets), {}, 48, 0, a.nets, a.points, sa);
  nudge_lanes(OPEN, bounds_of(OPEN, swapped), {}, 48, 0, swapped, b.points, sb);
  CHECK(same(a.points, b.points));
  CHECK(sa.moved == sb.moved);
}

TEST_CASE("nudge: a gap of nothing is a stage that does nothing") {
  Lane l{ two_over(100) };
  std::vector<scav_point> const before{ l.points };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), {}, 0, 0, l.nets, l.points, s);
  CHECK(same(l.points, before));
  CHECK(s.lanes == 0);
}

TEST_CASE("nudge: the lane sizes to the shortest leg it has to drag") {
  // Net 0 reaches the lane over a leg of 9, which caps its move up at 8; the lane
  // slides down to fit.
  std::vector<scav_point> points{ pt(0, 91),  pt(0, 100), pt(200, 100), pt(200, 300),
                                  pt(0, 300), pt(0, 100), pt(200, 100), pt(200, 500) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 } };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, nets), {}, 48, 0, nets, points, s);

  CHECK(s.moved == 2);
  CHECK(points[1].y == 92);
  CHECK(points[5].y == 140);
  // Every leg keeps its direction.
  CHECK(points[0].y < points[1].y);
  CHECK(points[3].y > points[2].y);
  CHECK(points[4].y > points[5].y);
  CHECK(points[7].y > points[6].y);
}

TEST_CASE("nudge: the frame's own box bounds a lane the obstacles do not") {
  // The frame box bounds the lane: room below is 9, one unit short of its border at y=110.
  Lane l{ two_over(100) };
  NudgeStats s;
  nudge_lanes(OPEN,
              bounds_of(rect(-1000, 0, 3000, 110), l.nets),
              {},
              48,
              0,
              l.nets,
              l.points,
              s);

  CHECK(s.moved == 2);
  CHECK(lane_y(l, 0) == 61);
  CHECK(lane_y(l, 1) == 109);
}

TEST_CASE("nudge: a lane inside a box's bumper may not close on the box") {
  // The lane sits 40 above a box, 8 inside its 48 bumper: no room towards the box.
  std::vector<scav_point> points{ pt(0, -400), pt(0, 100), pt(200, 100), pt(200, -300),
                                  pt(0, -600), pt(0, 100), pt(200, 100), pt(200, -500) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 } };
  std::vector<scav_rect> const wall{ rect(0, 140, 200, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, nets), wall, 200, 48, nets, points, s);

  CHECK(s.moved == 1);
  CHECK(points[1].y == 100);
  CHECK(points[5].y == -100);
}

TEST_CASE("nudge: a vertical lane is measured after the horizontal one has moved") {
  // The y=100 move extends net 1's leg at x=300 down to y=124; the box at y 105..120 is
  // beside the vertical lane only over that new extent.
  std::vector<scav_point> points{ pt(0, 0),      pt(0, 100),    pt(300, 100),
                                  pt(300, -400), pt(500, -400), pt(500, -900),
                                  pt(0, 900),    pt(0, 100),    pt(300, 100),
                                  pt(300, -500), pt(500, -500), pt(500, -1000) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 6 },
                                     scav_span{ .off = 6, .len = 6 } };
  std::vector<scav_rect> const wall{ rect(320, 105, 80, 15) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, nets), wall, 48, 0, nets, points, s);

  CHECK(s.lanes == 2);
  CHECK(s.moved == 4);
  CHECK(points[1].y == 76);
  CHECK(points[7].y == 124);
  // Centred, net 1 would land at x=324, 4 inside the box; the lane slides to put it at
  // 319, one unit short of the box's left side.
  CHECK(points[2].x == 271);
  CHECK(points[8].x == 319);
}

TEST_CASE("nudge: a displacement onto another net's segment is refused") {
  // Net 1's move down 24 would lay its segment on net 2 at y=124, so net 1 stays.
  std::vector<scav_point> points{ pt(0, 0),   pt(0, 100),  pt(200, 100), pt(200, 300),
                                  pt(0, 400), pt(0, 100),  pt(200, 100), pt(200, 500),
                                  pt(0, 124), pt(200, 124) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 },
                                     scav_span{ .off = 8, .len = 2 } };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, nets), {}, 48, 0, nets, points, s);

  CHECK(s.lanes == 1);
  CHECK(s.moved == 1);
  CHECK(points[1].y == 76);
  CHECK(points[5].y == 100);
  CHECK(points[8].y == 124);
}

TEST_CASE("nudge: two nets with one tail take one offset between them") {
  // Nets 0 and 1 run as one line from (200,100) to (200,300); net 2 shares only the lane.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.bundles == 1);
  CHECK(s.refused == 0);
  CHECK(s.moved == 3);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: two nets with one head take one offset between them") {
  // Nets 0 and 1 share their head, (0,0) to (0,100), and part after it; net 2's leg at
  // x=-40 is off the bundle's leg at x=0.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 0), pt(0, 100), pt(150, 100), pt(150, 400) },
                      { pt(-40, 400), pt(-40, 100), pt(200, 100), pt(200, 500) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.moved == 3);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: two nets with different tails are spread as they always were") {
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 0);
  CHECK(s.moved == 2);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 124);
}

TEST_CASE("nudge: a lane that is one bundle is left where the router put it") {
  // The lane is one bundle: two nets drawn as one line.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.bundles == 1);
  CHECK(s.spread == 0);
  CHECK(s.moved == 0);
  CHECK(net_pt(f, 0, 1).y == 100);
  CHECK(net_pt(f, 1, 1).y == 100);
}

TEST_CASE("nudge: three nets with one tail are one bundle") {
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 150), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 600), pt(0, 100), pt(200, 100), pt(200, 900) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.moved == 4);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 76);
  CHECK(net_pt(f, 3, 1).y == 124);
}

TEST_CASE("nudge: a net bundled by its head and another by its tail are one bundle") {
  // Nets 0 and 1 share a head and nets 1 and 2 a tail; 0 and 2 share neither and
  // bundle by transitive closure.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 0), pt(0, 100), pt(150, 100), pt(150, 400) },
                      { pt(30, 700), pt(30, 100), pt(150, 100), pt(150, 400) },
                      { pt(-40, 900), pt(-40, 100), pt(200, 100), pt(200, 1200) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.moved == 4);
  // Net 3 goes above: inside its extent, net 2's two downward legs outvote net 0's
  // upward leg at x=0.
  CHECK(net_pt(f, 3, 1).y == 76);
  CHECK(net_pt(f, 0, 1).y == 124);
  CHECK(net_pt(f, 1, 1).y == 124);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: a bundle's own legs may land on each other") {
  // The two reach the lane at x=0 from opposite sides, so displacing them takes
  // each onto the run the other still holds.
  Frame f{ frame_of({ { pt(0, 200), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 600), pt(0, 100), pt(200, 100), pt(200, 500) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.refused == 0);
  CHECK(s.moved == 3);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: a bundle is ordered by its least toward, not by its first member") {
  // The bundle holds the largest `toward` (500) and the smallest (10); ordered by 10, it
  // goes above net 2, whose `toward` is 300.
  Frame f{ frame_of({ { pt(0, 500), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 10), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 300), pt(0, 100), pt(200, 100), pt(200, 700) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: both axes of one frame bundle") {
  // Nets 0 and 1 run as one from (300,100) on: they bundle in the lane at y=100 and in
  // the one at x=300.
  Frame f{ frame_of({ { pt(0, 0),
                        pt(0, 100),
                        pt(300, 100),
                        pt(300, -400),
                        pt(500, -400),
                        pt(500, -900) },
                      { pt(0, 900),
                        pt(0, 100),
                        pt(300, 100),
                        pt(300, -400),
                        pt(500, -400),
                        pt(500, -900) },
                      { pt(0, -900),
                        pt(0, 100),
                        pt(300, 100),
                        pt(300, -500),
                        pt(500, -500),
                        pt(500, -1000) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  // Three lanes: y=100 and x=300 hold all three nets; y=-400 holds only the pair.
  CHECK(s.lanes == 3);
  CHECK(s.bundles == 3);
  CHECK(s.refused == 0);
  // The pair still runs as one, and the third net is off both lanes.
  CHECK(same({ net_pt(f, 0, 1), net_pt(f, 0, 2), net_pt(f, 0, 3) },
             { net_pt(f, 1, 1), net_pt(f, 1, 2), net_pt(f, 1, 3) }));
  CHECK(net_pt(f, 2, 1).y != net_pt(f, 0, 1).y);
  CHECK(net_pt(f, 2, 2).x != net_pt(f, 0, 2).x);
}

TEST_CASE("nudge: a bundle another net's run would be traded for stays whole") {
  // The bundle's move to y=76 would lay it on net 3 there, so neither member moves.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) },
                      { pt(0, 76), pt(200, 76) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.refused == 1);
  CHECK(s.moved == 1);
  CHECK(net_pt(f, 0, 1).y == 100);
  CHECK(net_pt(f, 1, 1).y == 100);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: a bundle the region does not hold stays whole") {
  // Net 1's leg at x=-50 lies left of the region: its move is refused and the bundle
  // stays.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(-50, 50), pt(-50, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) } }) };
  NudgeStats s;
  nudge_lanes(rect(0, -1000, 3000, 3000),
              bounds_of(OPEN, f.nets),
              {},
              48,
              0,
              f.nets,
              f.points,
              s);

  CHECK(s.bundles == 1);
  CHECK(s.refused == 1);
  CHECK(s.moved == 1);
  CHECK(net_pt(f, 0, 1).y == 100);
  CHECK(net_pt(f, 1, 1).y == 100);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: a bundle a box leaves no room for stays where it is") {
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) } }) };
  std::vector<scav_rect> const walls{ rect(0, 0, 200, 100), rect(0, 100, 200, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), walls, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.spread == 0);
  CHECK(s.moved == 0);
  for (uint32_t net = 0; net < 3; ++net) { CHECK(net_pt(f, net, 1).y == 100); }
}

TEST_CASE("nudge: bundles do not depend on the order the nets arrive in") {
  Frame a{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) } }) };
  Frame b{ a };
  std::vector<scav_span> const shuffled{ b.nets[2], b.nets[0], b.nets[1] };
  NudgeStats sa;
  NudgeStats sb;
  nudge_lanes(OPEN, bounds_of(OPEN, a.nets), {}, 48, 0, a.nets, a.points, sa);
  nudge_lanes(OPEN, bounds_of(OPEN, shuffled), {}, 48, 0, shuffled, b.points, sb);
  CHECK(same(a.points, b.points));
  CHECK(sa.bundles == sb.bundles);
  CHECK(sa.moved == sb.moved);
}

TEST_CASE("nudge: a leg crossing the other member's segment settles the order") {
  // Overlap on x in [100,200]: the key puts net 0 above, but net 0's leg down at x=200
  // and net 1's leg up at x=100 each cross the other's segment unless net 1 is above.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(100, 50), pt(100, 100), pt(300, 100), pt(300, 400) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.spread == 1);
  CHECK(s.reordered == 1);
  CHECK(s.moved == 2);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 1, 2).y == 76);
  CHECK(net_pt(f, 0, 1).y == 124);
  CHECK(net_pt(f, 0, 2).y == 124);
}

TEST_CASE("nudge: a lane whose members share both ends keeps the key's order") {
  // Equal extents: no leg lies strictly inside the other's, so the key sets the order.
  Lane l{ two_over(100) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, l.nets), {}, 48, 0, l.nets, l.points, s);

  CHECK(s.spread == 1);
  CHECK(s.reordered == 0);
  CHECK(lane_y(l, 0) == 76);
  CHECK(lane_y(l, 1) == 124);
}

TEST_CASE("nudge: a pair that must cross either way is left in the key's order") {
  // Net 0 runs inside net 1 and leaves up at one end and down at the other, so either
  // order crosses one leg; the votes cancel and the key decides.
  Frame f{ frame_of({ { pt(100, 0), pt(100, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(300, 100), pt(300, 150) } }) };
  Frame mirror{ frame_of({ { pt(0, 50), pt(0, 100), pt(300, 100), pt(300, 150) },
                           { pt(100, 0), pt(100, 100), pt(200, 100), pt(200, 300) } }) };
  NudgeStats s;
  NudgeStats t;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);
  nudge_lanes(OPEN,
              bounds_of(OPEN, mirror.nets),
              {},
              48,
              0,
              mirror.nets,
              mirror.points,
              t);

  CHECK(s.reordered == 0);
  CHECK(t.reordered == 0);
  // The key reads the low-end legs, y=0 for the inner net and y=50 for the outer, so
  // the inner net is on top in both frames.
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(mirror, 1, 1).y == 76);
}

TEST_CASE("nudge: a chain of votes orders a lane the key cannot") {
  // Staggered extents vote 0 before 1 before 2, with no vote between 0 and 2; the key
  // order is 2, 0, 1, and the chain has one linear extension.
  Frame f{ frame_of({ { pt(0, 200), pt(0, 100), pt(100, 100), pt(100, -400) },
                      { pt(50, 260), pt(50, 100), pt(150, 100), pt(150, -500) },
                      { pt(120, 149), pt(120, 100), pt(200, 100), pt(200, 400) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.reordered == 1);
  // Net 1's slot is its own y=100, so two members move.
  CHECK(s.moved == 2);
  CHECK(net_pt(f, 0, 1).y == 52);
  CHECK(net_pt(f, 1, 1).y == 100);
  CHECK(net_pt(f, 2, 1).y == 148);
}

TEST_CASE("nudge: a lane whose crossing order is not known good keeps its place") {
  // The head-and-tail bundle with net 3's leg on the bundle's line x=0: the voted order
  // lays legs along each other, so both moves are refused.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 0), pt(0, 100), pt(150, 100), pt(150, 400) },
                      { pt(30, 700), pt(30, 100), pt(150, 100), pt(150, 400) },
                      { pt(0, 900), pt(0, 100), pt(200, 100), pt(200, 1200) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.moved == 0);
  CHECK(s.reordered == 1);
  for (uint32_t net = 0; net < 4; ++net) { CHECK(net_pt(f, net, 1).y == 100); }
}

TEST_CASE("nudge: a lane the votes reorder counts as one whatever the room says") {
  // The crossing-ordered pair, walled above and below with no window to spread into:
  // `reordered` counts it and `spread` does not.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(100, 50), pt(100, 100), pt(300, 100), pt(300, 400) } }) };
  std::vector<scav_rect> const walls{ rect(0, 0, 300, 100), rect(0, 100, 300, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), walls, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.spread == 0);
  CHECK(s.moved == 0);
  CHECK(s.reordered == 1);
}

namespace {

// Three members on y=100 whose crossing votes form a cycle; the first net carries two
// of them, both with legs at x=200.
std::vector<std::vector<scav_point>> cyclic_lane() {
  return { { pt(100, 400),
             pt(100, 100),
             pt(200, 100),
             pt(200, -400),
             pt(600, -400),
             pt(600, -300),
             pt(0, -300),
             pt(0, 100),
             pt(200, 100),
             pt(200, 500) },
           { pt(100, -200), pt(100, 100), pt(300, 100), pt(300, -100) } };
}

}  // namespace

TEST_CASE("nudge: votes that run in a circle are settled by fewest contradictions") {
  // Keyed [0,200], [100,300], [100,200]; the votes put 1 before 0 by two, 2 before 1
  // and 0 before 2 by one: a cycle, so Kahn's algorithm finds no source.
  Frame f{ frame_of(cyclic_lane()) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.bundles == 0);
  CHECK(s.reordered == 1);
  CHECK(s.spread == 1);
  CHECK(s.moved == 2);

  // [100,300] on top, then [0,200], then [100,200]: it contradicts a vote of weight
  // one; the key order contradicts weight three.
  CHECK(net_pt(f, 1, 1).y == 52);
  CHECK(net_pt(f, 1, 2).y == 52);
  CHECK(net_pt(f, 0, 7).y == 100);
  CHECK(net_pt(f, 0, 8).y == 100);
  CHECK(net_pt(f, 0, 1).y == 148);
  CHECK(net_pt(f, 0, 2).y == 148);

  // The same nets offered in the other order.
  Frame g{ frame_of(cyclic_lane()) };
  std::vector<scav_span> const swapped{ g.nets[1], g.nets[0] };
  NudgeStats t;
  nudge_lanes(OPEN, bounds_of(OPEN, swapped), {}, 48, 0, swapped, g.points, t);
  CHECK(same(f.points, g.points));
  CHECK(t.reordered == s.reordered);
  CHECK(t.moved == s.moved);
}

TEST_CASE("nudge: a box the lane already runs through does not bound it") {
  // A box straddling the lane bounds nothing; a box below it bounds the room down.
  Lane through{ two_over(100) };
  std::vector<scav_rect> const across{ rect(50, 60, 100, 80) };
  NudgeStats s;
  nudge_lanes(OPEN,
              bounds_of(OPEN, through.nets),
              across,
              48,
              0,
              through.nets,
              through.points,
              s);
  CHECK(s.spread == 1);
  CHECK(lane_y(through, 0) == 76);
  CHECK(lane_y(through, 1) == 124);

  Lane beside{ two_over(100) };
  std::vector<scav_rect> const under{ rect(50, 110, 100, 80) };
  NudgeStats t;
  nudge_lanes(OPEN,
              bounds_of(OPEN, beside.nets),
              under,
              48,
              0,
              beside.nets,
              beside.points,
              t);
  CHECK(t.spread == 1);
  // One unit short of the box's top at y=110.
  CHECK(lane_y(beside, 0) == 61);
  CHECK(lane_y(beside, 1) == 109);
}

TEST_CASE("nudge: a leg outside the region is refused before a box is consulted") {
  // Net 0's leg at x=-50 lies left of the region, so its move is refused; the box at
  // x 400..500 is clear of the lane.
  std::vector<scav_point> points{ pt(-50, 0), pt(-50, 100), pt(200, 100), pt(200, 300),
                                  pt(0, 400), pt(0, 100),   pt(200, 100), pt(200, 500) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 } };
  std::vector<scav_rect> const away{ rect(400, 0, 100, 100) };
  NudgeStats s;
  nudge_lanes(rect(0, -1000, 3000, 3000),
              bounds_of(OPEN, nets),
              away,
              48,
              0,
              nets,
              points,
              s);

  CHECK(s.lanes == 1);
  CHECK(s.spread == 1);
  CHECK(s.moved == 1);
  CHECK(points[1].y == 100);
  CHECK(points[5].y == 124);
}

TEST_CASE("nudge: three bundles and one unit of room stay stacked") {
  // The legs leave a window of 1 for three bundles: 1 / 2 truncates to a step of 0.
  Frame f{ frame_of({ { pt(0, 99), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 102) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 400) } }) };
  std::vector<scav_point> const before{ f.points };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.bundles == 0);
  CHECK(s.spread == 0);
  CHECK(s.moved == 0);
  CHECK(same(f.points, before));
}

TEST_CASE("nudge: a leg an earlier lane shortened is not folded onto its own end") {
  // Two lanes of one net, joined by the leg (200,100)-(200,148). Room is read before
  // either moves; the first lane's move cuts that leg from 48 to 24.
  Frame f{ frame_of(
      { { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 148), pt(400, 148), pt(400, 600) },
        { pt(10, -100), pt(10, 100), pt(210, 100), pt(210, 500) },
        { pt(250, 300), pt(250, 148), pt(450, 148), pt(450, 600) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 2);
  CHECK(s.spread == 2);
  CHECK(s.moved == 3);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 0, 1).y == 124);
  CHECK(net_pt(f, 0, 2).y == 124);
  // Its move up 24 would collapse the joining leg at (200,124), so it stays; net 2 takes
  // its own offset to 172.
  CHECK(net_pt(f, 0, 3).y == 148);
  CHECK(net_pt(f, 0, 4).y == 148);
  CHECK(net_pt(f, 2, 1).y == 172);
}

TEST_CASE("nudge: a trailing leg an earlier lane shortened is refused the same way") {
  // The trailing-leg mirror: the lane at y=100 moves first and brings the leg's far
  // end to y=124, 24 nearer.
  Frame f{ frame_of(
      { { pt(0, 0), pt(0, 148), pt(200, 148), pt(200, 100), pt(400, 100), pt(400, 600) },
        { pt(210, -300), pt(210, 100), pt(410, 100), pt(410, 500) },
        { pt(50, 800), pt(50, 148), pt(250, 148), pt(250, 700) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, bounds_of(OPEN, f.nets), {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 2);
  CHECK(s.spread == 2);
  CHECK(s.moved == 3);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 0, 3).y == 124);
  CHECK(net_pt(f, 0, 4).y == 124);
  CHECK(net_pt(f, 0, 1).y == 148);
  CHECK(net_pt(f, 0, 2).y == 148);
  CHECK(net_pt(f, 2, 1).y == 172);
}
