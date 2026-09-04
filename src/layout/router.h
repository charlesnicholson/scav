#ifndef SCAV_LAYOUT_ROUTER_H_INCLUDED
#define SCAV_LAYOUT_ROUTER_H_INCLUDED

// The router boundary: one frame's obstacles and nets in, one polyline per net
// out. Internal, so no `scav_` prefix; the ABI sees a router by name (11.5).

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

// An end at a box centre names that box, so a router can move the point onto
// its border instead.
struct RouteNet {
  scav_point src{}, dst{};
  uint32_t src_obstacle{ INVALID }, dst_obstacle{ INVALID };  // -> obstacles
  uint32_t waypoint_off{ 0 }, waypoint_len{ 0 };  // -> waypoints, phase 1's corridor
};

struct RouteInput {
  scav_rect region{};                // the frame's rect; a route stays inside it
  std::vector<scav_rect> obstacles;  // every box overlapping the region
  std::vector<RouteNet> nets;        // in (transition, ordinal) order
  std::vector<scav_point> waypoints;
  scav_profile profile{};
};

enum class RouteFailure : int32_t {
  None = 0,
  OutsideRegion,  // an end the caller placed outside the region it supplied
  Unreachable,    // the obstacles enclose one of the ends
  TooLarge,       // the routing graph would exceed this router's budget
};

struct RouteMetrics {
  int32_t bends{ 0 };
  int64_t length{ 0 };
  RouteFailure failed{ RouteFailure::None };
  // Routed, but only after giving up the clearance it wanted (11.5), so this
  // shape may run flush against a box. Not a failure.
  int32_t reseated{ 0 };
};

struct RouteOutput {
  std::vector<scav_point> points;
  std::vector<scav_span> net_points;  // parallel to RouteInput::nets
  std::vector<RouteMetrics> metrics;
};

struct RouterName {
  char const *bytes;  // a literal: static storage, so it outlives every caller
  uint32_t len;
};

// Stateless and const, so two routers differ only in the shape they return.
// `out` is the caller's and the callee clears it, so one is reused per frame.
class Router {
 public:
  Router() = default;
  Router(Router const &) = delete;
  Router(Router &&) = delete;
  Router &operator=(Router const &) = delete;
  Router &operator=(Router &&) = delete;
  virtual ~Router() = default;

  // Both are hashed layout inputs (6). Bytes and a length, not a
  // `string_view`, which is outside this library's header subset (6).
  [[nodiscard]] virtual RouterName name() const = 0;
  [[nodiscard]] virtual uint32_t version() const = 0;

  // How far outside `region` this router needs lanes; the caller grows the region
  // by exactly this. A box on the frame's own edge has no room otherwise.
  [[nodiscard]] virtual int32_t margin(scav_profile const & /*p*/) const { return 0; }

  // Pure in `in`, reentrant, no global state, must not unwind.
  //
  // One polyline per net, in `in.nets` order, at least two points each. A
  // polyline runs from `net.src` to `net.dst`; at an end naming an obstacle it
  // runs from or to a point on that box's border instead, the caller having
  // put the named end at the box's centre. Phase 3 lays a transition's nets
  // end to end on that: consecutive nets are handed the same shared point and
  // it appears once in the joined route.
  virtual void route(RouteInput const &in, RouteOutput &out) const = 0;
};

// Threads phase 1's corridor and takes both ends where phase 3 put them: no
// obstacle set, no opinion of its own.
class StraightRouter final : public Router {
 public:
  [[nodiscard]] RouterName name() const override { return { "straight", 8 }; }
  [[nodiscard]] uint32_t version() const override { return 1; }
  void route(RouteInput const &in, RouteOutput &out) const override;
};

// A separated orthogonal visibility graph per frame and A* over it, so an edge
// through a box is unrepresentable rather than priced (11.5).
class OrthogonalRouter final : public Router {
 public:
  [[nodiscard]] RouterName name() const override { return { "orthogonal", 10 }; }
  [[nodiscard]] uint32_t version() const override { return 1; }
  [[nodiscard]] int32_t margin(scav_profile const &p) const override;
  void route(RouteInput const &in, RouteOutput &out) const override;
};

Router const *router_at(uint32_t index);  // null past the end

// Shared, so every router counts a bend and a length the same way.
void measure(std::vector<scav_point> const &points, scav_span at, RouteMetrics &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTER_H_INCLUDED
