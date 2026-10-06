#ifndef SCAV_LAYOUT_ROUTE_H_INCLUDED
#define SCAV_LAYOUT_ROUTE_H_INCLUDED

// Phase 3: one polyline per transition, one port slot per boundary it crosses, and the
// path boxes placed on the finished routes.

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

  // Nets that fell back to a straight line, by cause.
  uint32_t outside_region{ 0 }, unreachable{ 0 }, too_large{ 0 };
  std::vector<uint8_t> failed;  // parallel to transitions; 1 = a net of it fell back
  [[nodiscard]] uint32_t degraded() const {
    return outside_region + unreachable + too_large;
  }

  // Nets routed only at zero clearance.
  uint32_t reseated{ 0 };
  uint32_t occupied{ 0 };  // ends and port slots left inside an occupied span
};

// One frame's router and nudger input and output; a later run whose input is this one
// translated reuses the output, shifted.
struct RouteFrameCache {
  uint8_t valid{ 0 };
  scav_rect frame{};  // what the nudger bounds this frame's lanes by
  RouteInput in;
  std::vector<scav_point> points;
  std::vector<scav_span> net_points;
  std::vector<RouteMetrics> metrics;
};

// Where routing may stop: once `floor` plus `per_bend` per bend, each transition taking
// the larger of its `bends` and its routed nets' turns, reaches `at`; `reached` is that.
struct RouteStop {
  std::vector<int32_t> const *bends{ nullptr };
  int64_t floor{ 0 };
  int64_t per_bend{ 0 };
  int64_t at{ 0 };
  bool stopped{ false };
  int64_t reached{ 0 };
  int64_t sum{ 0 };  // the bends `reached` counts
};

// As `reuse`, shared read-only by a round's candidates; as `fill`, written by the one
// run that sets the incumbent.
struct RouteCache {
  std::vector<RouteFrameCache> frame;  // parallel to submachines
  // Two per segment, source end then destination: the router's `effective_faces` there.
  // An end pin outside those bits changes nothing drawn.
  std::vector<uint8_t> faceable;
};

// Moves each slot off its route to where the route next meets the slot's face inside its
// span, else any face of the slot's box; a divider port's box is its divider line.
void reseat_slots(SplitGraph const &g, SizedLayout const &z, Routes &out);

// Per segment, its bend nodes from source to destination as routing reads them; `reversed`
// gets each segment's `OrderEdge::reversed`.
void segment_bends(SubmachineOrders const &o,
                   uint32_t segments,
                   std::vector<uint32_t> &reversed,
                   std::vector<std::vector<uint32_t>> &bends);

// One net per segment, routed in its frame, laid end to end; the result is the same at
// every `threads`. With `labels` false, `placed` is empty.
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

// The same into `out`, reusing its capacity; with `stop`, frames route in order on this
// thread, and a stop leaves `out` unfinished.
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
                       bool labels,
                       RouteStop *stop = nullptr);

// Places the path boxes on `out`'s finished routes, as `route_transitions` does.
void label_routes(Routes &out,
                  Chart const &c,
                  SplitGraph const &g,
                  SizedLayout const &z,
                  scav_spaces const &s,
                  scav_profile const &p);

}  // namespace scav

#endif  // SCAV_LAYOUT_ROUTE_H_INCLUDED
