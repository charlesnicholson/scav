#ifndef SCAV_LAYOUT_ROUTE_H_INCLUDED
#define SCAV_LAYOUT_ROUTE_H_INCLUDED

// Phase 3: one polyline per transition through the nodes its segments were
// chained across, one port slot per boundary it crosses, and the path boxes
// slid onto the finished routes.

#include "layout/decompose.h"
#include "layout/order.h"
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
};

// Straight between consecutive nodes: the router this phase runs is
// `straight`, so what the ranks decided is the whole of the shape.
Routes phase3_route(Chart const &c,
                    SplitGraph const &g,
                    SubmachineOrders const &o,
                    SizedLayout const &z,
                    scav_spaces const &s,
                    scav_profile const &p);

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTE_H_INCLUDED
