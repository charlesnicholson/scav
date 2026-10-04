#ifndef SCAV_LAYOUT_ROUTER_H_INCLUDED
#define SCAV_LAYOUT_ROUTER_H_INCLUDED

// The router boundary: one frame's obstacles and nets in, one polyline per net out.
// Internal; the ABI selects a router by name.

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"
#include "scav_int.h"

#include <cstdint>
#include <vector>

namespace scav {

// The room a route keeps from a box it passes.
constexpr int32_t route_clearance(scav_profile const &p) {
  return imax(p.node_sep / 3, 1);
}

// Width of the band inside an enclosure's border that routes keep out of.
constexpr int32_t border_band(scav_profile const &p) {
  return imax(imin(route_clearance(p), p.pad) / 2, 1);
}

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
  // A self-loop's reach: its corridor runs this far out from each of its two seats,
  // on its box's least-used face unless a face is named.
  int32_t loop{ 0 };
  uint32_t trans{ INVALID }, seg{ INVALID };  // the caller's ids, read only by the trace
};

struct RouteInput {
  scav_rect region{};                // the frame's rect; a route stays inside it
  std::vector<scav_rect> obstacles;  // the boxes and walls routes keep out of
  // Parallel to `obstacles`, or empty for none: nonzero for a disc or diamond inscribed
  // in its box, which a route meets only at a face midpoint.
  std::vector<uint8_t> inscribed;
  // Parallel to `obstacles`, or empty for all zero: the corner arc radius of the shape
  // drawn in that box, which seats are held off.
  std::vector<int32_t> corner;
  std::vector<RouteNet> nets;  // in (transition, ordinal) order
  std::vector<scav_point> waypoints;
  scav_profile profile{};
  // The box the frame's routes are drawn inside, zero-sized for the root. Routes keep
  // out of its `border_band`; an end in that band leaves square to the border.
  scav_rect enclosure{};
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
};

struct RouteOutput {
  std::vector<scav_point> points;
  std::vector<scav_span> net_points;  // parallel to RouteInput::nets
  std::vector<RouteMetrics> metrics;
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
  [[nodiscard]] uint32_t effective_faces(RouteInput const &in,
                                         uint32_t net,
                                         uint32_t end) const override;
  void route(RouteInput const &in, RouteOutput &out) const override;
};

Router const *router_at(uint32_t index);  // null past the end

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTER_H_INCLUDED
