// The `straight` router, and the polyline measurement every router shares.

#include "layout/router.h"

#include "scav_int.h"

#include <cstdint>
#include <vector>

namespace scav {

namespace {

// 0, 1, or 2 per axis, so a bend is a change in the pair -- the same token the
// structural hash and the cost scorer both count with.
uint32_t direction(scav_point a, scav_point b) {
  auto const axis = [](int32_t from, int32_t to) {
    if (to > from) { return 2U; }
    return (to < from) ? 0U : 1U;
  };
  return (axis(a.x, b.x) * 3U) + axis(a.y, b.y);
}

}  // namespace

void measure(std::vector<scav_point> const &points, scav_span at, RouteMetrics &out) {
  for (uint32_t k = 0; (k + 1) < at.len; ++k) {
    scav_point const &a{ points[at.off + k] };
    scav_point const &b{ points[at.off + k + 1] };
    Wide const dx{ Wide{ b.x } - a.x };
    Wide const dy{ Wide{ b.y } - a.y };
    out.length += static_cast<Wide>(isqrt(static_cast<uint64_t>((dx * dx) + (dy * dy))));
    if ((k + 2) < at.len) {
      if (direction(a, b) != direction(b, points[at.off + k + 2])) { ++out.bends; }
    }
  }
}

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
    RouteMetrics m;
    measure(out.points, at, m);
    out.metrics.push_back(m);
  }
}

}  // namespace scav
