// The `straight` router.

#include "layout/router.h"

#include <cstdint>

namespace scav {

void StraightRouter::route(RouteInput const &in, RouteOutput &out) const {
  out.points.clear();
  out.net_points.clear();
  out.metrics.clear();
  out.net_points.reserve(in.nets.size());
  out.metrics.reserve(in.nets.size());

  for (RouteNet const &net : in.nets) {
    uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
    out.points.push_back(net.src);
    for (uint32_t k = 0; k < net.waypoint_len; ++k) {
      out.points.push_back(in.waypoints[net.waypoint_off + k]);
    }
    out.points.push_back(net.dst);
    scav_span const at{ .off = off,
                        .len = static_cast<uint32_t>(out.points.size()) - off };
    out.net_points.push_back(at);
    out.metrics.push_back({});
  }
}

}  // namespace scav
