#ifndef SCAV_LAYOUT_ROUTER_H_INCLUDED
#define SCAV_LAYOUT_ROUTER_H_INCLUDED

// The router boundary: one frame's obstacles and nets in, one polyline per net out.
// Internal; the ABI selects a router by name.

#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"
#include "scav_int.h"
#include "scav_vector.h"

#include <cstdint>
#include <vector>

namespace scav {

// An end that names a box sits at its centre; a router may move it onto the border.
struct RouteNet {
  scav_point src{}, dst{};
  uint32_t src_obstacle{ INVALID }, dst_obstacle{ INVALID };  // -> obstacles
  uint32_t waypoint_off{ 0 }, waypoint_len{ 0 };  // -> waypoints, phase 1's corridor
  // Named face per end: 0 left, 1 right, 2 top, 3 bottom; INVALID lets the router choose.
  uint32_t src_face{ INVALID }, dst_face{ INVALID };
  // 1 seats a straight leg between parallel faces at the low end of the run both faces
  // seat; 0 seats it midway between the face centres.
  uint32_t lean{ 0 };
  // A loop's reach: its corridor runs this far out from each of its two seats, on its
  // points' face, else its box's least-used face unless one is named.
  int32_t loop{ 0 };
  int32_t src_clear{ 0 }, dst_clear{ 0 };     // the least straight run at each end
  uint32_t trans{ INVALID }, seg{ INVALID };  // the caller's ids, read only by the trace
  uint32_t apart{ INVALID };                  // -> an earlier net this one keeps clear of
  // -> obstacles: the box a loop between two points on its border runs off.
  uint32_t loop_box{ INVALID };
  // Per end naming no box, 1 where its leg runs straight to `src_stub` or `dst_stub` and
  // the route meets that point along the leg's line.
  uint32_t src_stubbed{ 0 }, dst_stubbed{ 0 };
  scav_point src_stub{}, dst_stub{};
};

// Face of `r` that `at` lies on: 0 left, 1 right, 2 top, 3 bottom, else INVALID. Corners
// resolve as in `ortho_ring`.
uint32_t face_of(scav_point at, scav_rect const &r);

// True when face `face` of `r` exceeds twice the inset `max(clear, arc)` a seat keeps from
// each corner.
bool face_seats(scav_rect const &r, uint32_t face, int32_t clear, int32_t arc);

// Appends to `out` the run between a loop's `ends` on each face of `r` they touch, padded
// by `clear`, as spans of obstacle `st`.
void loop_occupied(std::array<scav_point, 2> const &ends,
                   scav_rect const &r,
                   uint32_t st,
                   int32_t clear,
                   std::vector<OccupiedSpan> &out);

// Whether `pos` lies inside a span on `face` of `obstacle`.
bool occupied_at(std::vector<OccupiedSpan> const &spans,
                 uint32_t obstacle,
                 uint32_t face,
                 int32_t pos);

// `pos` moved to the nearest coordinate in `[lo, hi]` that `occupied_at` rejects, ties to
// the lower; false and `pos` unchanged when none is.
bool occupied_free(std::vector<OccupiedSpan> const &spans,
                   uint32_t obstacle,
                   uint32_t face,
                   int32_t lo,
                   int32_t hi,
                   int32_t &pos);

struct RouteInput {
  scav_rect region{};           // the frame's rect; a route stays inside it
  Vector<scav_rect> obstacles;  // the boxes and walls routes keep out of
  // Parallel to `obstacles`, or empty for none: nonzero for a disc or diamond inscribed
  // in its box, which a route meets only at a face midpoint.
  Vector<uint8_t> inscribed;
  // Parallel to `obstacles`, or empty for all zero: the corner arc radius of the shape
  // drawn in that box, which seats are held off.
  Vector<int32_t> corner;
  uint32_t first_wall{ INVALID };  // obstacles from this index on are walls; INVALID: none
  Vector<RouteNet> nets;           // in (transition, ordinal) order
  Vector<scav_point> waypoints;
  scav_profile profile{};
  // The box the frame's routes are drawn inside, zero-sized for the root. Routes keep
  // out of its `border_band`; an end in that band leaves square to the border.
  scav_rect enclosure{};
  std::vector<OccupiedSpan> occupied;  // inner loops' legs on box faces
  // Walls a route meets only along its border or an end's stub: the gaps between the
  // frame's region and its sibling regions.
  Vector<scav_rect> gaps;
};

enum class RouteFailure : int32_t {
  None = 0,
  OutsideRegion,  // an end the caller placed outside the region it supplied
  Unreachable,    // the obstacles enclose one of the ends
  TooLarge,       // the routing graph would exceed this router's budget
};

struct RouteMetrics {
  RouteFailure failed{ RouteFailure::None };
  // 1 when routed only at zero clearance; the shape may run flush against a box.
  int32_t reseated{ 0 };
  int32_t occupied{ 0 };  // ends left inside an occupied span, with no free position
};

struct RouteOutput {
  Vector<scav_point> points;
  Vector<scav_span> net_points;  // parallel to RouteInput::nets
  Vector<RouteMetrics> metrics;
};

struct RouterName {
  char const *bytes;  // a string literal, valid for the program's lifetime
  uint32_t len;
};

// Stateless and const; `route` clears the caller-owned `out` before filling it.
class Router {
 public:
  Router() = default;
  Router(Router const &) = delete;
  Router(Router &&) = delete;
  Router &operator=(Router const &) = delete;
  Router &operator=(Router &&) = delete;
  virtual ~Router() = default;

  // Both are hashed layout inputs.
  [[nodiscard]] virtual RouterName name() const = 0;
  [[nodiscard]] virtual uint32_t version() const = 0;

  // How far the caller grows `region` on every side for this router's lanes.
  [[nodiscard]] virtual int32_t margin(scav_profile const & /*p*/) const { return 0; }

  // True when undegraded nets run axis-aligned and leave each box end square from a seat
  // `max(arc, 1)` off a face's corners, or at an inscribed glyph's face middle.
  [[nodiscard]] virtual bool rectilinear() const { return false; }

  // The face an end on box `r` with no seatable named face is seated on, aimed at `aim`,
  // on a box neither inscribed nor looped; 4 for any face.
  [[nodiscard]] virtual uint32_t seat_face(scav_rect const & /*r*/,
                                           scav_point /*aim*/) const {
    return 4;
  }

  // Bit f set where naming face f at end `end` (0 source, 1 destination) of
  // `in.nets[net]` can change the route; independent of that end's own named face.
  [[nodiscard]] virtual uint32_t effective_faces(RouteInput const & /*in*/,
                                                 uint32_t /*net*/,
                                                 uint32_t /*end*/) const {
    return 0;
  }

  // Pure in `in` and reentrant. One polyline per net in `in.nets` order, from `src` to
  // `dst` or a named box's border; consecutive nets share their joining point.
  virtual void route(RouteInput const &in, RouteOutput &out) const = 0;
};

// Joins each net's `src`, corridor waypoints and `dst` with straight segments; ignores
// obstacles.
class StraightRouter final : public Router {
 public:
  [[nodiscard]] RouterName name() const override {
    return { .bytes = "straight", .len = 8 };
  }
  [[nodiscard]] uint32_t version() const override { return 1; }
  void route(RouteInput const &in, RouteOutput &out) const override;
};

// A* over a separated orthogonal visibility graph per frame whose edges stay out of boxes.
class OrthogonalRouter final : public Router {
 public:
  [[nodiscard]] RouterName name() const override {
    return { .bytes = "orthogonal", .len = 10 };
  }
  [[nodiscard]] uint32_t version() const override { return 1; }
  [[nodiscard]] int32_t margin(scav_profile const &p) const override;
  [[nodiscard]] bool rectilinear() const override { return true; }
  [[nodiscard]] uint32_t seat_face(scav_rect const &r, scav_point aim) const override;
  [[nodiscard]] uint32_t effective_faces(RouteInput const &in,
                                         uint32_t net,
                                         uint32_t end) const override;
  void route(RouteInput const &in, RouteOutput &out) const override;
};

Router const *router_at(uint32_t index);  // null past the end

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTER_H_INCLUDED
