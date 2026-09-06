// The nudging stage on its own: two hand-written polylines and a box, with no
// model, no profile and no router in the way.

#include "layout/nudge.h"

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

// Two nets both crossing y=100 between x=0 and x=200, each with a leg at either
// end so the crossing segment is interior and therefore movable.
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

// One net per polyline, laid end to end as the router hands them over.
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

// Far enough out that only the obstacles bound a fixture.
scav_rect const OPEN{ rect(-1000, -1000, 3000, 3000) };

// `scav_point` is a POD with no equality of its own, and giving it one for a
// test would put an operator in the public vocabulary to serve this file.
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
  nudge_lanes(OPEN, OPEN, {}, 48, 0, l.nets, l.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.spread == 1);
  CHECK(s.moved == 2);
  // Centred on where the router put them, so neither slides wholesale.
  CHECK(lane_y(l, 0) != lane_y(l, 1));
  CHECK((lane_y(l, 0) + lane_y(l, 1)) == 200);
  // The legs that were dragged still meet the segment they lead to.
  for (uint32_t net = 0; net < 2; ++net) {
    scav_span const at{ l.nets[net] };
    for (uint32_t k = 0; (k + 1) < at.len; ++k) {
      scav_point const a{ l.points[at.off + k] };
      scav_point const b{ l.points[at.off + k + 1] };
      CHECK(((a.x == b.x) || (a.y == b.y)));
    }
  }
}

TEST_CASE("nudge: a lane with no room keeps its members stacked") {
  // Boxes meeting at the lane leave nowhere to go, and a shared lane is worth
  // more than a lane through a box.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const walls{ rect(0, 0, 200, 100), rect(0, 100, 200, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, walls, 48, 0, l.nets, l.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.spread == 0);
  CHECK(s.moved == 0);
  CHECK(lane_y(l, 0) == 100);
  CHECK(lane_y(l, 1) == 100);
}

TEST_CASE("nudge: a lane with room on one side only slides onto that side") {
  // A box above and open space below is the ordinary case, and requiring room
  // both sides would give up on it. The spread sizes to the whole window and
  // then slides back towards the lane as far as it will go.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const wall{ rect(0, 0, 200, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, wall, 48, 0, l.nets, l.points, s);

  CHECK(s.spread == 1);
  CHECK(lane_y(l, 0) != lane_y(l, 1));
  // Nothing went up into the box, and the lower member is one gap down.
  CHECK(lane_y(l, 0) >= 100);
  CHECK(lane_y(l, 1) >= 100);
  int32_t const lo{ lane_y(l, 0) < lane_y(l, 1) ? lane_y(l, 0) : lane_y(l, 1) };
  int32_t const hi{ lane_y(l, 0) < lane_y(l, 1) ? lane_y(l, 1) : lane_y(l, 0) };
  CHECK(lo == 100);
  CHECK(hi == 148);
}

TEST_CASE("nudge: clearance is kept, so a displacement never ends up flush") {
  // The router stands its routes off by `clear` (11.5) and a nudge may not undo
  // that: with the box 60 above, a 48 step upward would land 12 short.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const wall{ rect(0, 0, 200, 40) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, wall, 48, 48, l.nets, l.points, s);
  for (uint32_t net = 0; net < 2; ++net) { CHECK(lane_y(l, net) >= 88); }
}

TEST_CASE("nudge: the step shrinks to the room rather than being refused") {
  // 20 units either side and two members: the widest centred step that fits is
  // 40, well under the 480 asked for.
  Lane l{ two_over(100) };
  std::vector<scav_rect> const walls{ rect(0, 0, 200, 80), rect(0, 120, 200, 80) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, walls, 480, 0, l.nets, l.points, s);

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
  nudge_lanes(rect(0, 90, 200, 20), rect(0, 90, 200, 20), {}, 480, 0, l.nets, l.points, s);
  // Room is 10 either side, so the step is 20 and both stay inside.
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
  nudge_lanes(OPEN, OPEN, {}, 48, 0, nets, points, s);
  CHECK(s.lanes == 0);
  CHECK(s.moved == 0);
  CHECK(same(points, before));
}

TEST_CASE("nudge: nets that only touch at a point are not one lane") {
  // Abutting, not overlapping: separating them would buy nothing and cost two
  // bends.
  std::vector<scav_point> points{ pt(0, 0),     pt(0, 100),   pt(100, 100), pt(100, 300),
                                  pt(200, 400), pt(200, 100), pt(300, 100), pt(300, 500) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 } };
  std::vector<scav_point> const before{ points };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, nets, points, s);
  CHECK(s.lanes == 0);
  CHECK(same(points, before));
}

TEST_CASE("nudge: a displacement that would enter a box is dropped, not clamped") {
  // Room says 100 either side, but one member's dragged leg would sweep into a
  // box that sits beside the lane rather than across it. That member stays.
  Lane l{ two_over(100) };
  // Beside the second net's trailing leg at x=200, below the lane.
  std::vector<scav_rect> const walls{ rect(150, 130, 100, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, walls, 48, 0, l.nets, l.points, s);
  CHECK(s.lanes == 1);
  // Whatever it decided, nothing ends up inside the box.
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
  // Nets offered in the other order: the ordering key is the geometry, not the
  // arrival order, so the answer cannot depend on it.
  std::vector<scav_span> const swapped{ b.nets[1], b.nets[0] };
  NudgeStats sa;
  NudgeStats sb;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, a.nets, a.points, sa);
  nudge_lanes(OPEN, OPEN, {}, 48, 0, swapped, b.points, sb);
  CHECK(same(a.points, b.points));
  CHECK(sa.moved == sb.moved);
}

TEST_CASE("nudge: a gap of nothing is a stage that does nothing") {
  Lane l{ two_over(100) };
  std::vector<scav_point> const before{ l.points };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 0, 0, l.nets, l.points, s);
  CHECK(same(l.points, before));
  CHECK(s.lanes == 0);
}

TEST_CASE("nudge: the lane sizes to the shortest leg it has to drag") {
  // The first net reaches the lane over a leg of 9. A step of 48 either side
  // would take that leg past its own start and fold the polyline back on itself,
  // so the whole lane sizes to the 8 it can spare.
  std::vector<scav_point> points{ pt(0, 91),  pt(0, 100), pt(200, 100), pt(200, 300),
                                  pt(0, 300), pt(0, 100), pt(200, 100), pt(200, 500) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 } };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, nets, points, s);

  CHECK(s.moved == 2);
  CHECK(points[1].y == 92);
  CHECK(points[5].y == 140);
  // Both legs still run the way they ran.
  CHECK(points[0].y < points[1].y);
  CHECK(points[3].y > points[2].y);
  CHECK(points[4].y > points[5].y);
  CHECK(points[7].y > points[6].y);
}

TEST_CASE("nudge: the frame's own box bounds a lane the obstacles do not") {
  // The owner of a frame is never an obstacle in it and the region reaches past
  // it, so the box comes in on its own. Room below is 9: the border at 110 is a
  // line the frame draws, and a lane on it is drawn over it.
  Lane l{ two_over(100) };
  NudgeStats s;
  nudge_lanes(OPEN, rect(-1000, 0, 3000, 110), {}, 48, 0, l.nets, l.points, s);

  CHECK(s.moved == 2);
  CHECK(lane_y(l, 0) == 61);
  CHECK(lane_y(l, 1) == 109);
}

TEST_CASE("nudge: a lane inside a box's bumper may not close on the box") {
  // The lane sits 40 above a box and 8 inside the 48 bumper the router stands
  // off by, which is a re-seated route (11.5). The room towards the box is none,
  // not the whole of it.
  std::vector<scav_point> points{ pt(0, -400), pt(0, 100), pt(200, 100), pt(200, -300),
                                  pt(0, -600), pt(0, 100), pt(200, 100), pt(200, -500) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 } };
  std::vector<scav_rect> const wall{ rect(0, 140, 200, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, wall, 200, 48, nets, points, s);

  CHECK(s.moved == 1);
  CHECK(points[1].y == 100);
  CHECK(points[5].y == -100);
}

TEST_CASE("nudge: a vertical lane is measured after the horizontal one has moved") {
  // The move at y=100 drags the vertical legs at x=300 down past y=100 with it,
  // and the box to their right is beside the lane only at that new extent.
  std::vector<scav_point> points{ pt(0, 0),      pt(0, 100),    pt(300, 100),
                                  pt(300, -400), pt(500, -400), pt(500, -900),
                                  pt(0, 900),    pt(0, 100),    pt(300, 100),
                                  pt(300, -500), pt(500, -500), pt(500, -1000) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 6 },
                                     scav_span{ .off = 6, .len = 6 } };
  std::vector<scav_rect> const wall{ rect(320, 105, 80, 15) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, wall, 48, 0, nets, points, s);

  CHECK(s.lanes == 2);
  CHECK(s.moved == 4);
  CHECK(points[1].y == 76);
  CHECK(points[7].y == 124);
  // Extents read before the horizontal move stop the lane at y=100, short of the
  // box, and take this member 4 inside it.
  CHECK(points[2].x == 272);
  CHECK(points[8].x == 320);
}

TEST_CASE("nudge: a displacement onto another net's segment is refused") {
  // The lower member's 24 lands it exactly along a third net, trading the lane
  // it left for the one it makes.
  std::vector<scav_point> points{ pt(0, 0),   pt(0, 100),  pt(200, 100), pt(200, 300),
                                  pt(0, 400), pt(0, 100),  pt(200, 100), pt(200, 500),
                                  pt(0, 124), pt(200, 124) };
  std::vector<scav_span> const nets{ scav_span{ .off = 0, .len = 4 },
                                     scav_span{ .off = 4, .len = 4 },
                                     scav_span{ .off = 8, .len = 2 } };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, nets, points, s);

  CHECK(s.lanes == 1);
  CHECK(s.moved == 1);
  CHECK(points[1].y == 76);
  CHECK(points[5].y == 100);
  CHECK(points[8].y == 124);
}

TEST_CASE("nudge: two nets with one tail take one offset between them") {
  // Both reach (200,100) and run to (200,300) as one line; a third arrives on the
  // same lane from elsewhere. The two that share the tail move together.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.bundles == 1);
  CHECK(s.refused == 0);
  CHECK(s.moved == 3);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: two nets with one head take one offset between them") {
  // A fork's fan-out is the fan-in read backwards: identical from the net's start
  // to the segment, and apart after it.
  // The loner's leg is off the bundle's so the lane is about the bundle alone:
  // two legs on one line share a run whichever way the lane is ordered.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 0), pt(0, 100), pt(150, 100), pt(150, 400) },
                      { pt(-40, 400), pt(-40, 100), pt(200, 100), pt(200, 500) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

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
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 0);
  CHECK(s.moved == 2);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 124);
}

TEST_CASE("nudge: a lane that is one bundle is left where the router put it") {
  // Nothing to spread: the lane is two nets drawn as one line, which is what the
  // reader is meant to see.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

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
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.moved == 4);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 76);
  CHECK(net_pt(f, 3, 1).y == 124);
}

TEST_CASE("nudge: a net bundled by its head and another by its tail are one bundle") {
  // The first two share a head and the last two a tail, and the outer pair shares
  // neither -- so the bundle is the closure rather than either relation.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 0), pt(0, 100), pt(150, 100), pt(150, 400) },
                      { pt(30, 700), pt(30, 100), pt(150, 100), pt(150, 400) },
                      { pt(-40, 900), pt(-40, 100), pt(200, 100), pt(200, 1200) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.moved == 4);
  // The loner goes above rather than below: the bundle's middle member leaves
  // downward at x=30, inside the loner's extent, so the other way round the
  // loner's segment is crossed by it.
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
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.refused == 0);
  CHECK(s.moved == 3);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: a bundle is ordered by its least toward, not by its first member") {
  // The bundle holds the lane's highest arrival and its lowest; the lowest is
  // what places it, so the third net stays on the far side of it.
  Frame f{ frame_of({ { pt(0, 500), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 10), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 300), pt(0, 100), pt(200, 100), pt(200, 700) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(f, 1, 1).y == 76);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: both axes of one frame bundle") {
  // The first two run as one from (300,100) on, so they hold together over the
  // horizontal lane at y=100 and the vertical one at x=300 alike.
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
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  // Three lanes: y=100 and x=300 hold all three nets, and the pair's own
  // (300,-400)-(500,-400) is a lane of nothing but the bundle.
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
  // The bundle's offset lands its members exactly along a fourth net, which is
  // the one thing a displacement may not buy. Neither member moves.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) },
                      { pt(0, 76), pt(200, 76) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.refused == 1);
  CHECK(s.moved == 1);
  CHECK(net_pt(f, 0, 1).y == 100);
  CHECK(net_pt(f, 1, 1).y == 100);
  CHECK(net_pt(f, 2, 1).y == 124);
}

TEST_CASE("nudge: a bundle the region does not hold stays whole") {
  // One member's leg reaches left of the region, so its displacement is not a
  // known-good one however much room the lane has, and its sibling waits.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(-50, 50), pt(-50, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 400), pt(0, 100), pt(200, 100), pt(200, 500) } }) };
  NudgeStats s;
  nudge_lanes(rect(0, -1000, 3000, 3000), OPEN, {}, 48, 0, f.nets, f.points, s);

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
  nudge_lanes(OPEN, OPEN, walls, 48, 0, f.nets, f.points, s);

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
  nudge_lanes(OPEN, OPEN, {}, 48, 0, a.nets, a.points, sa);
  nudge_lanes(OPEN, OPEN, {}, 48, 0, shuffled, b.points, sb);
  CHECK(same(a.points, b.points));
  CHECK(sa.bundles == sb.bundles);
  CHECK(sa.moved == sb.moved);
}

TEST_CASE("nudge: a leg crossing the other member's segment settles the order") {
  // Two members overlapping over x in [100,200] rather than end to end. The key
  // reads only the low end, where both legs go up and the first goes further, so
  // it puts net 0 above; both crossings say the opposite. Net 0's leg down at
  // x=200 is inside net 1's extent, and net 1's leg up at x=100 is inside net
  // 0's, so either one of them is crossed unless net 1 goes above.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(100, 50), pt(100, 100), pt(300, 100), pt(300, 400) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

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
  // Neither member's leg lands inside the other's extent, so no crossing is
  // available to order them by and the low-end key is the whole answer.
  Lane l{ two_over(100) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, l.nets, l.points, s);

  CHECK(s.spread == 1);
  CHECK(s.reordered == 0);
  CHECK(lane_y(l, 0) == 76);
  CHECK(lane_y(l, 1) == 124);
}

TEST_CASE("nudge: a pair that must cross either way is left in the key's order") {
  // Net 0 runs inside net 1 and leaves up at one end and down at the other, so
  // one of its two legs is crossed whichever way round they go. The votes cancel
  // and the key decides, rather than the first constraint found winning.
  Frame f{ frame_of({ { pt(100, 0), pt(100, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 50), pt(0, 100), pt(300, 100), pt(300, 150) } }) };
  Frame mirror{ frame_of({ { pt(0, 50), pt(0, 100), pt(300, 100), pt(300, 150) },
                           { pt(100, 0), pt(100, 100), pt(200, 100), pt(200, 300) } }) };
  NudgeStats s;
  NudgeStats t;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);
  nudge_lanes(OPEN, OPEN, {}, 48, 0, mirror.nets, mirror.points, t);

  CHECK(s.reordered == 0);
  CHECK(t.reordered == 0);
  // Keyed on the low-end leg, which is net 0's y=0 against net 1's y=50 either
  // way round, so the two frames put the same net on top.
  CHECK(net_pt(f, 0, 1).y == 76);
  CHECK(net_pt(mirror, 1, 1).y == 76);
}

TEST_CASE("nudge: a chain of votes orders a lane the key cannot") {
  // Staggered extents: net 0 over [0,100] leaves up at its high end inside net
  // 1's [50,150], net 1 over [50,150] leaves up at its high end inside net 2's
  // [120,200], and nets 0 and 2 share no coordinate at all, so nothing votes on
  // that pair. Keyed on the arrivals at 149, 200 and 260 the lane enters as 2,
  // 0, 1, and the chain 0 before 1 before 2 has one linear extension.
  Frame f{ frame_of({ { pt(0, 200), pt(0, 100), pt(100, 100), pt(100, -400) },
                      { pt(50, 260), pt(50, 100), pt(150, 100), pt(150, -500) },
                      { pt(120, 149), pt(120, 100), pt(200, 100), pt(200, 400) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.reordered == 1);
  // The middle of the three is already where it belongs, so two of them move.
  CHECK(s.moved == 2);
  CHECK(net_pt(f, 0, 1).y == 52);
  CHECK(net_pt(f, 1, 1).y == 100);
  CHECK(net_pt(f, 2, 1).y == 148);
}

TEST_CASE("nudge: a lane whose crossing order is not known good keeps its place") {
  // Same shape as the bundle above with the loner's leg back on the bundle's own
  // line. The crossings still want the loner above, and taking that order would
  // lay its leg along the bundle's, so both refuse and the lane stays stacked --
  // the order is chosen on crossings and taken only where the room is good.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(0, 0), pt(0, 100), pt(150, 100), pt(150, 400) },
                      { pt(30, 700), pt(30, 100), pt(150, 100), pt(150, 400) },
                      { pt(0, 900), pt(0, 100), pt(200, 100), pt(200, 1200) } }) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, {}, 48, 0, f.nets, f.points, s);

  CHECK(s.bundles == 1);
  CHECK(s.moved == 0);
  CHECK(s.reordered == 1);
  for (uint32_t net = 0; net < 4; ++net) { CHECK(net_pt(f, net, 1).y == 100); }
}

TEST_CASE("nudge: a lane the votes reorder counts as one whatever the room says") {
  // The pair the crossings settle, walled top and bottom so the lane has no
  // window to spread into at all: `reordered` is counted where the order is
  // decided, so this lane is one of them and none of `spread`.
  Frame f{ frame_of({ { pt(0, 0), pt(0, 100), pt(200, 100), pt(200, 300) },
                      { pt(100, 50), pt(100, 100), pt(300, 100), pt(300, 400) } }) };
  std::vector<scav_rect> const walls{ rect(0, 0, 300, 100), rect(0, 100, 300, 100) };
  NudgeStats s;
  nudge_lanes(OPEN, OPEN, walls, 48, 0, f.nets, f.points, s);

  CHECK(s.lanes == 1);
  CHECK(s.spread == 0);
  CHECK(s.moved == 0);
  CHECK(s.reordered == 1);
}
