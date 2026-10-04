// The `straight` router.

#include "layout/router.h"

#include "scav_vec.h"

#include <cstdint>
#include <vector>

namespace scav {

void StraightRouter::route(RouteInput const &in, RouteOutput &out) const {
  out.points.clear();
  out.net_points.clear();
  out.metrics.clear();
  vec_reserve(out.net_points, in.nets.size());
  vec_reserve(out.metrics, in.nets.size());

  for (RouteNet const &net : in.nets) {
    uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
    vec_push_back(out.points, net.src);
    for (uint32_t k = 0; k < net.waypoint_len; ++k) {
      vec_push_back(out.points, in.waypoints[net.waypoint_off + k]);
    }
    vec_push_back(out.points, net.dst);
    scav_span const at{ .off = off,
                        .len = static_cast<uint32_t>(out.points.size()) - off };
    vec_push_back(out.net_points, at);
    vec_push_back(out.metrics, {});
  }
}

}  // namespace scav
