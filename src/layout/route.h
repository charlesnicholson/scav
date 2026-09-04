#ifndef SCAV_LAYOUT_ROUTE_H_INCLUDED
#define SCAV_LAYOUT_ROUTE_H_INCLUDED

// Phase 3: one polyline per transition, one port slot per boundary it crosses,
// and the path boxes slid onto the finished routes.

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/router.h"
#include "layout/size.h"
#include "scav/scav_core.h"
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
  [[nodiscard]] uint32_t degraded() const {
    return outside_region + unreachable + too_large;
  }

  // Routed only after giving up the requested clearance (11.5). Not a failure; a
  // frame full of them means the boxes are packed tighter than the profile says.
  uint32_t reseated{ 0 };
};

// One net per segment, routed in that segment's frame, laid end to end. The
// planning is the router's input, so two routers see the same problem.
Routes phase3_route(Chart const &c,
                    SplitGraph const &g,
                    SubmachineOrders const &o,
                    SizedLayout const &z,
                    scav_spaces const &s,
                    scav_profile const &p,
                    Router const &router);

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTE_H_INCLUDED
