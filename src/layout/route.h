#ifndef SCAV_LAYOUT_ROUTE_H_INCLUDED
#define SCAV_LAYOUT_ROUTE_H_INCLUDED

// Phase 3: one polyline per transition, one port slot per boundary it crosses,
// and the path boxes slid onto the finished routes.

#include "layout/decompose.h"
#include "layout/label.h"
#include "layout/order.h"
#include "layout/router.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include <cstdint>
#include <vector>

namespace scav {

struct Routes {
  std::vector<scav_point> points;
  std::vector<scav_port_slot> slots;
  std::vector<scav_span> route, port;  // parallel to transitions
  std::vector<scav_rect> placed;       // parallel to the path boxes

  // Nets the router fell back on, by cause. A fallback is a straight line, and a
  // straight line is what Tier 0 counts.
  uint32_t outside_region{ 0 }, unreachable{ 0 }, too_large{ 0 };
  std::vector<uint8_t> failed;  // parallel to transitions; 1 = a net of it fell back
  [[nodiscard]] uint32_t degraded() const {
    return outside_region + unreachable + too_large;
  }

  // Routed only after giving up the requested clearance (11.5). Not a failure; a
  // frame full of them means the boxes are packed tighter than the profile says.
  uint32_t reseated{ 0 };
};

// Per frame, the exact question the router and the nudger were asked and the
// answer they gave. A Level 1 move changes one frame and leaves every other one
// translated -- measured at 19 of 20 on `mill` -- so a frame whose question only
// moved is answered by moving its answer, which is what makes a candidate cost
// the change rather than the chart (11.10c).
struct RouteFrameCache {
  uint8_t valid{ 0 };
  scav_rect frame{};  // what the nudger bounds this frame's lanes by
  RouteInput in;
  std::vector<scav_point> points;
  std::vector<scav_span> net_points;
  std::vector<RouteMetrics> metrics;
};

// Parallel to submachines. `reuse` is read by every candidate of a round at
// once and never written; `fill` is written by the one run that establishes the
// incumbent.
struct RouteCache {
  std::vector<RouteFrameCache> frame;
  // Two per segment, its source end then its destination: the router's
  // `effective_faces` there. A face pin outside it changes nothing drawn.
  std::vector<uint8_t> faceable;
};

// One net per segment, routed in that segment's frame, laid end to end. The
// planning is the router's input, so two routers see the same problem. Frames
// are sharded across `threads` workers and merged in frame order, so the
// result is one value at every worker count. Without `labels` the routes are final and
// `placed` is empty.
Routes route_transitions(Chart const &c,
                         SplitGraph const &g,
                         SubmachineOrders const &o,
                         SizedLayout const &z,
                         scav_spaces const &s,
                         scav_profile const &p,
                         Router const &router,
                         uint32_t threads = 0,
                         RouteCache const *reuse = nullptr,
                         RouteCache *fill = nullptr,
                         SearchPins const *pins = nullptr,
                         bool labels = true);

// The same into `out`, reusing its capacity.
void route_transitions(Routes &out,
                       Chart const &c,
                       SplitGraph const &g,
                       SubmachineOrders const &o,
                       SizedLayout const &z,
                       scav_spaces const &s,
                       scav_profile const &p,
                       Router const &router,
                       uint32_t threads,
                       RouteCache const *reuse,
                       RouteCache *fill,
                       SearchPins const *pins,
                       bool labels);

// The path boxes placed on `out`'s finished routes over `z`, as `route_transitions` places
// them.
void label_routes(Routes &out,
                  Chart const &c,
                  SplitGraph const &g,
                  SizedLayout const &z,
                  scav_spaces const &s,
                  scav_profile const &p);

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTE_H_INCLUDED
