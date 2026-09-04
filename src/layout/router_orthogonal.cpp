// The `orthogonal` router: a visibility graph per frame, h-plane and v-plane
// copies joined by an edge weighing the bend penalty, and A* over that (11.5).

#include "layout/router_orthogonal.h"

#include "layout/router.h"
#include "scav_int.h"
#include "scav_stable_sort.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

// `f`, then `g`, then node. `node` carries vertex and plane, so no two states
// compare equal and the pop order cannot depend on the push order.
bool frontier_before(Wide fa, Wide ga, uint32_t na, Wide fb, Wide gb, uint32_t nb) {
  if (fa != fb) { return fa < fb; }
  if (ga != gb) { return ga < gb; }
  return na < nb;
}

void heap_swap(OrthoScratch &s, uint32_t a, uint32_t b) {
  Wide const f{ s.heap_f[a] };
  Wide const g{ s.heap_g[a] };
  uint32_t const node{ s.heap_node[a] };
  s.heap_f[a] = s.heap_f[b];
  s.heap_g[a] = s.heap_g[b];
  s.heap_node[a] = s.heap_node[b];
  s.heap_f[b] = f;
  s.heap_g[b] = g;
  s.heap_node[b] = node;
}

// `<algorithm>` is outside this library's header subset (6).
void heap_push(OrthoScratch &s, Wide f, Wide g, uint32_t node) {
  s.heap_f.push_back(f);
  s.heap_g.push_back(g);
  s.heap_node.push_back(node);
  uint32_t i{ static_cast<uint32_t>(s.heap_node.size()) - 1 };
  while (i > 0) {
    uint32_t const parent{ (i - 1) / 2 };
    if (!frontier_before(s.heap_f[i],
                         s.heap_g[i],
                         s.heap_node[i],
                         s.heap_f[parent],
                         s.heap_g[parent],
                         s.heap_node[parent])) {
      break;
    }
    heap_swap(s, i, parent);
    i = parent;
  }
}

void heap_pop(OrthoScratch &s, Wide &f, Wide &g, uint32_t &node) {
  f = s.heap_f[0];
  g = s.heap_g[0];
  node = s.heap_node[0];
  s.heap_f[0] = s.heap_f.back();
  s.heap_g[0] = s.heap_g.back();
  s.heap_node[0] = s.heap_node.back();
  s.heap_f.pop_back();
  s.heap_g.pop_back();
  s.heap_node.pop_back();
  uint32_t i{ 0 };
  auto const n{ static_cast<uint32_t>(s.heap_node.size()) };
  for (;;) {
    uint32_t const left{ (2 * i) + 1 };
    uint32_t best{ i };
    if ((left < n) && frontier_before(s.heap_f[left],
                                      s.heap_g[left],
                                      s.heap_node[left],
                                      s.heap_f[best],
                                      s.heap_g[best],
                                      s.heap_node[best])) {
      best = left;
    }
    if (((left + 1) < n) && frontier_before(s.heap_f[left + 1],
                                            s.heap_g[left + 1],
                                            s.heap_node[left + 1],
                                            s.heap_f[best],
                                            s.heap_g[best],
                                            s.heap_node[best])) {
      best = left + 1;
    }
    if (best == i) { break; }
    heap_swap(s, i, best);
    i = best;
  }
}

bool strictly_inside(scav_point p, scav_rect const &r) {
  return (p.x > r.x) && (p.x < (r.x + r.w)) && (p.y > r.y) && (p.y < (r.y + r.h));
}

}  // namespace

bool ortho_blocks_h(scav_rect const &r, int32_t y, int32_t x0, int32_t x1) {
  // `w > 0` is not implied by the straddle: a zero-width box has two sides a
  // segment passes between, and blocking there forbids what the scorer allows.
  return (r.w > 0) && (y > r.y) && (y < (r.y + r.h)) && (x0 < (r.x + r.w)) && (x1 > r.x);
}

bool ortho_blocks_v(scav_rect const &r, int32_t x, int32_t y0, int32_t y1) {
  return (r.h > 0) && (x > r.x) && (x < (r.x + r.w)) && (y0 < (r.y + r.h)) && (y1 > r.y);
}

void ortho_sort_unique(std::vector<int32_t> &v) {
  scav_stable_sort(v, [](int32_t a, int32_t b) { return a < b; });
  uint32_t kept{ 0 };
  for (uint32_t i = 0; i < v.size(); ++i) {
    if ((kept == 0) || (v[i] != v[kept - 1])) { v[kept++] = v[i]; }
  }
  v.resize(kept);
}

uint32_t ortho_index_of(std::vector<int32_t> const &v, int32_t at) {
  uint32_t lo{ 0 };
  uint32_t hi{ static_cast<uint32_t>(v.size()) };
  while ((hi - lo) > 1) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (v[mid] <= at) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

scav_point ortho_escape_box(scav_point at, scav_point toward, scav_rect const &r) {
  std::array<scav_point, 4> const options{ { { .x = r.x, .y = at.y },
                                             { .x = r.x + r.w, .y = at.y },
                                             { .x = at.x, .y = r.y },
                                             { .x = at.x, .y = r.y + r.h } } };
  scav_point best{ options[0] };
  Wide best_d{ -1 };
  for (scav_point const &option : options) {
    Wide const dx{ Wide{ option.x } - toward.x };
    Wide const dy{ Wide{ option.y } - toward.y };
    Wide const d{ (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) };
    // Strictly less: of two equal exits the earlier wins, so the order is the
    // fixed left, right, top, bottom.
    if ((best_d < 0) || (d < best_d)) {
      best_d = d;
      best = option;
    }
  }
  return best;
}

scav_point ortho_escape(scav_point at,
                        scav_point toward,
                        std::vector<scav_rect> const &boxes) {
  // Innermost by area, then by list order: the shortest stub out crosses the
  // fewest boxes it is not excused from crossing.
  uint32_t chosen{ INVALID };
  Wide smallest{ -1 };
  for (uint32_t i = 0; i < boxes.size(); ++i) {
    if (!strictly_inside(at, boxes[i])) { continue; }
    Wide const area{ Wide{ boxes[i].w } * boxes[i].h };
    if ((smallest < 0) || (area < smallest)) {
      smallest = area;
      chosen = i;
    }
  }
  return (chosen == INVALID) ? at : ortho_escape_box(at, toward, boxes[chosen]);
}

uint32_t ortho_box_at(scav_point at, std::vector<scav_rect> const &boxes) {
  uint32_t chosen{ INVALID };
  Wide smallest{ -1 };
  for (uint32_t i = 0; i < boxes.size(); ++i) {
    scav_rect const &r{ boxes[i] };
    bool const on_side{ ((at.x == r.x) || (at.x == (r.x + r.w))) && (at.y >= r.y) &&
                        (at.y <= (r.y + r.h)) };
    bool const on_cap{ ((at.y == r.y) || (at.y == (r.y + r.h))) && (at.x >= r.x) &&
                       (at.x <= (r.x + r.w)) };
    if (!on_side && !on_cap) { continue; }
    Wide const area{ Wide{ r.w } * r.h };
    if ((smallest < 0) || (area < smallest)) {
      smallest = area;
      chosen = i;
    }
  }
  return chosen;
}

scav_point ortho_ring(scav_point at, scav_rect const &r, int32_t clear) {
  if (at.x == r.x) { return { .x = at.x - clear, .y = at.y }; }
  if (at.x == (r.x + r.w)) { return { .x = at.x + clear, .y = at.y }; }
  if (at.y == r.y) { return { .x = at.x, .y = at.y - clear }; }
  if (at.y == (r.y + r.h)) { return { .x = at.x, .y = at.y + clear }; }
  return at;
}

void ortho_simplify(std::vector<scav_point> const &from, std::vector<scav_point> &to) {
  for (scav_point const &at : from) {
    // Both rules to a fixed point: one pop can expose a duplicate underneath,
    // which is what a route doubling back on its own stub produces.
    bool skip{ false };
    for (;;) {
      if (!to.empty() && (to.back().x == at.x) && (to.back().y == at.y)) {
        skip = true;
        break;
      }
      if (to.size() < 2) { break; }
      scav_point const &a{ to[to.size() - 2] };
      scav_point const &b{ to[to.size() - 1] };
      bool const flat{ (a.y == b.y) && (b.y == at.y) };
      bool const upright{ (a.x == b.x) && (b.x == at.x) };
      if (!flat && !upright) { break; }
      to.pop_back();
    }
    if (!skip) { to.push_back(at); }
  }
}

bool ortho_grid(scav_rect const &region,
                std::vector<scav_rect> const &obstacles,
                std::vector<scav_point> const &anchors,
                int32_t clear,
                OrthoGrid &out) {
  int32_t const lo_x{ region.x };
  int32_t const hi_x{ region.x + region.w };
  int32_t const lo_y{ region.y };
  int32_t const hi_y{ region.y + region.h };

  out.xs.clear();
  out.ys.clear();
  out.pass_h.clear();
  out.pass_v.clear();
  out.xs.push_back(lo_x);
  out.xs.push_back(hi_x);
  out.ys.push_back(lo_y);
  out.ys.push_back(hi_y);
  // Grown sides only: a line on a box edge is a lane, and hugging is free. Ring
  // points sit here, so denying the true border costs no reachability.
  for (scav_rect const &r : obstacles) {
    for (int32_t const at : { r.x - clear, r.x + r.w + clear }) {
      if ((at > lo_x) && (at < hi_x)) { out.xs.push_back(at); }
    }
    for (int32_t const at : { r.y - clear, r.y + r.h + clear }) {
      if ((at > lo_y) && (at < hi_y)) { out.ys.push_back(at); }
    }
  }
  for (scav_point const &at : anchors) {
    // Both coordinates or neither: an anchor outside the region is rejected
    // anyway and should not seed half a crossing inside it.
    if ((at.x < lo_x) || (at.x > hi_x) || (at.y < lo_y) || (at.y > hi_y)) { continue; }
    out.xs.push_back(at.x);
    out.ys.push_back(at.y);
  }
  ortho_sort_unique(out.xs);
  ortho_sort_unique(out.ys);
  if ((Wide{ out.nx() } * out.ny()) > ORTHO_VERTEX_BUDGET) { return false; }

  out.pass_h.assign(static_cast<size_t>(out.ny()) * (out.nx() - 1), 1);
  out.pass_v.assign(static_cast<size_t>(out.ny() - 1) * out.nx(), 1);
  for (scav_rect const &box : obstacles) {
    // The bumper: blocking against it makes "no closer than `clear` to a box" a
    // property of the graph.
    scav_rect const r{ .x = box.x - clear,
                       .y = box.y - clear,
                       .w = box.w + (2 * clear),
                       .h = box.h + (2 * clear) };
    for (uint32_t iy = 0; iy < out.ny(); ++iy) {
      for (uint32_t ix = 0; (ix + 1) < out.nx(); ++ix) {
        if (ortho_blocks_h(r, out.ys[iy], out.xs[ix], out.xs[ix + 1])) {
          out.pass_h[(iy * (out.nx() - 1)) + ix] = 0;
        }
      }
    }
    for (uint32_t iy = 0; (iy + 1) < out.ny(); ++iy) {
      for (uint32_t ix = 0; ix < out.nx(); ++ix) {
        if (ortho_blocks_v(r, out.xs[ix], out.ys[iy], out.ys[iy + 1])) {
          out.pass_v[(iy * out.nx()) + ix] = 0;
        }
      }
    }
  }
  return true;
}

bool ortho_search(OrthoGrid const &g,
                  uint32_t from,
                  uint32_t to,
                  Wide bend,
                  OrthoScratch &s,
                  std::vector<uint32_t> &out) {
  out.clear();
  uint32_t const vertices{ g.nx() * g.ny() };
  if ((vertices == 0) || (from >= vertices) || (to >= vertices)) { return false; }
  if (g.pass_h.empty() && (g.nx() > 1)) { return false; }
  uint32_t const nodes{ vertices * 2 };
  if (s.stamp.size() != nodes) {
    s.stamp.assign(nodes, 0);
    s.best.assign(nodes, 0);
    s.parent.assign(nodes, INVALID);
    s.generation = 0;
  }
  ++s.generation;
  uint32_t const gen{ s.generation };
  scav_point const goal{ g.point(to) };

  // Manhattan plus one bend for an axis this plane cannot cover alone. Both are
  // lower bounds, so the sum admits.
  auto const heuristic = [&](uint32_t node) {
    scav_point const at{ g.point(node / 2) };
    Wide const dx{ (at.x < goal.x) ? (Wide{ goal.x } - at.x) : (Wide{ at.x } - goal.x) };
    Wide const dy{ (at.y < goal.y) ? (Wide{ goal.y } - at.y) : (Wide{ at.y } - goal.y) };
    bool const turn{ ((node % 2) == 0) ? (dy != 0) : (dx != 0) };
    return dx + dy + (turn ? bend : Wide{ 0 });
  };

  s.heap_f.clear();
  s.heap_g.clear();
  s.heap_node.clear();
  for (uint32_t plane = 0; plane < 2; ++plane) {
    uint32_t const node{ (from * 2) + plane };
    s.stamp[node] = gen;
    s.best[node] = 0;
    s.parent[node] = INVALID;
    heap_push(s, heuristic(node), 0, node);
  }

  uint32_t expansions{ 0 };
  uint32_t reached{ INVALID };
  while (!s.heap_node.empty()) {
    Wide top_f{ 0 };
    Wide top_g{ 0 };
    uint32_t node{ 0 };
    heap_pop(s, top_f, top_g, node);
    if ((s.stamp[node] != gen) || (top_g != s.best[node])) { continue; }
    if ((node / 2) == to) {
      reached = node;
      break;
    }
    if (++expansions > ORTHO_EXPANSION_BUDGET) { return false; }

    uint32_t const v{ node / 2 };
    uint32_t const plane{ node % 2 };
    uint32_t const ix{ v % g.nx() };
    uint32_t const iy{ v / g.nx() };

    auto const relax = [&](uint32_t next, Wide step) {
      Wide const g_next{ top_g + step };
      if ((s.stamp[next] == gen) && (s.best[next] <= g_next)) { return; }
      s.stamp[next] = gen;
      s.best[next] = g_next;
      s.parent[next] = node;
      heap_push(s, g_next + heuristic(next), g_next, next);
    };

    relax((v * 2) + (1 - plane), bend);  // the turn, then this plane's moves
    if (plane == 0) {
      if (((ix + 1) < g.nx()) && (g.pass_h[(iy * (g.nx() - 1)) + ix] != 0)) {
        relax(g.vertex(ix + 1, iy) * 2, Wide{ g.xs[ix + 1] } - g.xs[ix]);
      }
      if ((ix > 0) && (g.pass_h[(iy * (g.nx() - 1)) + (ix - 1)] != 0)) {
        relax(g.vertex(ix - 1, iy) * 2, Wide{ g.xs[ix] } - g.xs[ix - 1]);
      }
    } else {
      if (((iy + 1) < g.ny()) && (g.pass_v[(iy * g.nx()) + ix] != 0)) {
        relax((g.vertex(ix, iy + 1) * 2) + 1, Wide{ g.ys[iy + 1] } - g.ys[iy]);
      }
      if ((iy > 0) && (g.pass_v[((iy - 1) * g.nx()) + ix] != 0)) {
        relax((g.vertex(ix, iy - 1) * 2) + 1, Wide{ g.ys[iy] } - g.ys[iy - 1]);
      }
    }
  }
  if (reached == INVALID) { return false; }

  s.path.clear();
  for (uint32_t node = reached; node != INVALID; node = s.parent[node]) {
    s.path.push_back(node / 2);
  }
  for (uint32_t i = static_cast<uint32_t>(s.path.size()); i-- > 0;) {
    if (out.empty() || (out.back() != s.path[i])) { out.push_back(s.path[i]); }
  }
  return true;
}

Wide ortho_bend_penalty(scav_profile const &p) {
  // Not 11.6's exchange rate, which is sixteen grid units and buys a staircase
  // wherever the grid offers one. A profile field for it is P9's calibration.
  return imax(Wide{ p.rank_sep }, Wide{ 1 });
}

int32_t ortho_clearance(scav_profile const &p) { return imax(p.node_sep / 3, 1); }

int32_t OrthogonalRouter::margin(scav_profile const &p) const {
  return ortho_clearance(p);
}

void OrthogonalRouter::route(RouteInput const &in, RouteOutput &out) const {
  out.points.clear();
  out.net_points.clear();
  out.metrics.clear();
  out.net_points.reserve(in.nets.size());
  out.metrics.reserve(in.nets.size());

  Wide const bend{ ortho_bend_penalty(in.profile) };
  int32_t const clear{ ortho_clearance(in.profile) };

  // A route never leaves the region, which makes "avoid this frame's obstacles"
  // mean "avoid every box". An anchor outside it degrades that one net.
  int32_t const lo_x{ in.region.x };
  int32_t const hi_x{ in.region.x + in.region.w };
  int32_t const lo_y{ in.region.y };
  int32_t const hi_y{ in.region.y + in.region.h };
  auto const in_region = [&](scav_point at) {
    return (at.x >= lo_x) && (at.x <= hi_x) && (at.y >= lo_y) && (at.y <= hi_y);
  };

  // Each end is up to three points: the caller's, the border it attaches to, and
  // the ring `clear` out. Only the ring is searched, so that leg is square.
  std::vector<scav_point> lead;
  std::vector<scav_point> anchors;
  std::vector<scav_span> net_anchors(in.nets.size(), scav_span{});
  std::vector<scav_span> net_lead(in.nets.size(), scav_span{});
  std::vector<scav_span> net_tail(in.nets.size(), scav_span{});

  auto const approach = [&](scav_point exact, uint32_t named, scav_point toward) {
    uint32_t box{ named };
    scav_point attach{ exact };
    if (box < in.obstacles.size()) {
      // A named box means `exact` is its centre, so the centre is dropped.
      attach = ortho_escape_box(exact, toward, in.obstacles[box]);
    } else {
      scav_point const moved{ ortho_escape(exact, toward, in.obstacles) };
      if ((moved.x != exact.x) || (moved.y != exact.y)) {
        // An exact end inside a box is 11.14's carve-out: keep it, and stub out to
        // the border to make it reachable.
        lead.push_back(exact);
        attach = moved;
      }
      box = ortho_box_at(attach, in.obstacles);
    }
    scav_point at{ attach };
    if (box < in.obstacles.size()) {
      // Clamped, not refused: a box on the frame's edge still gets a square approach.
      // Refusing puts the search back on the border line the grid exists to deny.
      scav_point ring{ ortho_ring(attach, in.obstacles[box], clear) };
      ring.x = imin(imax(ring.x, lo_x), hi_x);
      ring.y = imin(imax(ring.y, lo_y), hi_y);
      bool ok{ (ring.x != attach.x) || (ring.y != attach.y) };
      for (scav_rect const &r : in.obstacles) {
        if (strictly_inside(ring, r)) { ok = false; }
      }
      if (ok) {
        lead.push_back(attach);
        at = ring;
      }
    }
    return at;
  };

  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    scav_point const after{ (net.waypoint_len != 0) ? in.waypoints[net.waypoint_off]
                                                    : net.dst };
    scav_point const before{ (net.waypoint_len != 0)
                                 ? in.waypoints[net.waypoint_off + net.waypoint_len - 1]
                                 : net.src };
    uint32_t const off{ static_cast<uint32_t>(anchors.size()) };
    uint32_t const lead_first{ static_cast<uint32_t>(lead.size()) };
    scav_point const from{ approach(net.src, net.src_obstacle, after) };
    net_lead[n] = { .off = lead_first,
                    .len = static_cast<uint32_t>(lead.size()) - lead_first };

    anchors.push_back(from);
    for (uint32_t k = 0; k < net.waypoint_len; ++k) {
      anchors.push_back(in.waypoints[net.waypoint_off + k]);
    }
    uint32_t const tail_first{ static_cast<uint32_t>(lead.size()) };
    anchors.push_back(approach(net.dst, net.dst_obstacle, before));
    net_tail[n] = { .off = tail_first,
                    .len = static_cast<uint32_t>(lead.size()) - tail_first };
    net_anchors[n] = { .off = off, .len = static_cast<uint32_t>(anchors.size()) - off };
  }

  OrthoGrid g;
  bool const affordable{ ortho_grid(in.region, in.obstacles, anchors, clear, g) };

  // 11.5's re-seat, built only when needed: the same graph without bumpers. Two
  // boxes closer than twice the clearance seal the channel between them.
  OrthoGrid tight;
  bool tight_built{ false };
  bool tight_ok{ false };

  OrthoScratch scratch;
  std::vector<uint32_t> hop;
  std::vector<scav_point> piece;
  std::vector<scav_point> shape;
  for (uint32_t n = 0; n < in.nets.size(); ++n) {
    RouteNet const &net{ in.nets[n] };
    scav_span const at{ net_anchors[n] };
    RouteFailure why{ affordable ? RouteFailure::None : RouteFailure::TooLarge };
    for (uint32_t k = 0; (why == RouteFailure::None) && (k < at.len); ++k) {
      if (!in_region(anchors[at.off + k])) { why = RouteFailure::OutsideRegion; }
    }
    for (uint32_t k = 0; (why == RouteFailure::None) && (k < net_lead[n].len); ++k) {
      if (!in_region(lead[net_lead[n].off + k])) { why = RouteFailure::OutsideRegion; }
    }
    for (uint32_t k = 0; (why == RouteFailure::None) && (k < net_tail[n].len); ++k) {
      if (!in_region(lead[net_tail[n].off + k])) { why = RouteFailure::OutsideRegion; }
    }

    auto const attempt = [&](OrthoGrid const &use) {
      // Through the simplifier like every other run, so a net whose ends resolve to
      // one place emits one point: a zero-length segment has no direction.
      shape.clear();
      piece.clear();
      for (uint32_t k = 0; k < net_lead[n].len; ++k) {
        piece.push_back(lead[net_lead[n].off + k]);
      }
      piece.push_back(anchors[at.off]);
      ortho_simplify(piece, shape);
      for (uint32_t k = 0; (k + 1) < at.len; ++k) {
        scav_point const a{ anchors[at.off + k] };
        scav_point const b{ anchors[at.off + k + 1] };
        uint32_t const v_from{ use.vertex(ortho_index_of(use.xs, a.x),
                                          ortho_index_of(use.ys, a.y)) };
        uint32_t const v_to{ use.vertex(ortho_index_of(use.xs, b.x),
                                        ortho_index_of(use.ys, b.y)) };
        if (!ortho_search(use, v_from, v_to, bend, scratch, hop)) {
          shape.clear();
          return false;
        }
        piece.clear();
        for (uint32_t const v : hop) { piece.push_back(use.point(v)); }
        ortho_simplify(piece, shape);
      }
      // The tail was built outward from the box, so it comes back inward.
      piece.clear();
      for (uint32_t k = net_tail[n].len; k-- > 0;) {
        piece.push_back(lead[net_tail[n].off + k]);
      }
      ortho_simplify(piece, shape);
      return true;
    };

    int32_t reseated{ 0 };
    bool ok{ (why == RouteFailure::None) && attempt(g) };
    if ((why == RouteFailure::None) && !ok) {
      if (!tight_built) {
        tight_built = true;
        tight_ok = ortho_grid(in.region, in.obstacles, anchors, 0, tight);
      }
      ok = tight_ok && attempt(tight);
      if (ok) { reseated = 1; }
      if (!ok) { why = RouteFailure::Unreachable; }
    }

    if (!ok) {
      // Deterministic degradation, never a silent overlap: the straight line is what
      // the cost vector then scores as a Tier-0 violation.
      shape.clear();
      shape.push_back(net.src);
      for (uint32_t k = 0; k < net.waypoint_len; ++k) {
        shape.push_back(in.waypoints[net.waypoint_off + k]);
      }
      shape.push_back(net.dst);
    }

    uint32_t const off{ static_cast<uint32_t>(out.points.size()) };
    for (scav_point const &pt : shape) { out.points.push_back(pt); }
    scav_span const span{ .off = off,
                          .len = static_cast<uint32_t>(out.points.size()) - off };
    out.net_points.push_back(span);
    RouteMetrics m{ .bends = 0, .length = 0, .failed = why, .reseated = reseated };
    measure(out.points, span, m);
    out.metrics.push_back(m);
  }
}

}  // namespace scav
