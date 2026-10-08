// The router registry; a router id is an index into `ROUTERS`.

#include "layout/router.h"

#include "scav/scav_layout.h"
#include "scav/scav_types.h"
#include "scav_pod_vector.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace scav {

namespace {

constinit StraightRouter const STRAIGHT;
constinit OrthogonalRouter const ORTHOGONAL;

// Index 0 is the default router.
constexpr std::array<Router const *, 2> ROUTERS{ { &ORTHOGONAL, &STRAIGHT } };

}  // namespace

int32_t seat_pitch(scav_profile const &p) {
  return imax(route_clearance(p), label_line_height(p));
}

uint32_t face_capacity(int32_t len, int32_t arc, scav_profile const &p) {
  int32_t const inset{ imin(seat_inset(route_clearance(p), arc), len / 2) };
  return static_cast<uint32_t>(((Wide{ len } - (Wide{ 2 } * inset)) / seat_pitch(p)) + 1);
}

uint32_t box_capacity(int32_t w, int32_t h, int32_t arc, scav_profile const &p) {
  return 2 * (face_capacity(w, arc, p) + face_capacity(h, arc, p));
}

Wide face_length(uint32_t seats, int32_t arc, scav_profile const &p) {
  if (seats == 0) { return 0; }
  return (Wide{ seats - 1 } * seat_pitch(p)) +
         (Wide{ 2 } * seat_inset(route_clearance(p), arc));
}

bool face_seats(scav_rect const &r, uint32_t face, int32_t clear, int32_t arc) {
  int32_t const len{ (face < 2) ? r.h : r.w };
  return len > (2 * imin(seat_inset(clear, arc), len / 2));
}

uint32_t face_of(scav_point at, scav_rect const &r) {
  if (at.x == r.x) { return 0; }
  if (at.x == (r.x + r.w)) { return 1; }
  if (at.y == r.y) { return 2; }
  if (at.y == (r.y + r.h)) { return 3; }
  return INVALID;
}

void loop_occupied(std::array<scav_point, 2> const &ends,
                   scav_rect const &r,
                   uint32_t st,
                   int32_t clear,
                   std::vector<OccupiedSpan> &out) {
  std::array<uint32_t, 2> const face{ face_of(ends[0], r), face_of(ends[1], r) };
  for (uint32_t k = 0; k < 2; ++k) {
    if ((face[k] == INVALID) || ((k == 1) && (face[1] == face[0]))) { continue; }
    bool const along_y{ face[k] < 2 };
    int32_t const a{ along_y ? ends[k].y : ends[k].x };
    int32_t const other{ along_y ? ends[1 - k].y : ends[1 - k].x };
    int32_t const b{ (face[1 - k] == face[k]) ? other : a };
    vec_push_back(out,
                  { .obstacle = st,
                    .face = face[k],
                    .lo = imin(a, b) - clear,
                    .len = (imax(a, b) - imin(a, b)) + (2 * clear) });
  }
}

bool occupied_at(std::vector<OccupiedSpan> const &spans,
                 uint32_t obstacle,
                 uint32_t face,
                 int32_t pos) {
  for (OccupiedSpan const &o : spans) {
    if ((o.obstacle == obstacle) && (o.face == face) && (pos > o.lo) &&
        (Wide{ pos } < (Wide{ o.lo } + o.len))) {
      return true;
    }
  }
  return false;
}

bool occupied_free(std::vector<OccupiedSpan> const &spans,
                   uint32_t obstacle,
                   uint32_t face,
                   int32_t lo,
                   int32_t hi,
                   int32_t &pos) {
  int32_t best{ pos };
  Wide gap{ -1 };
  auto const consider = [&](Wide v) {
    if ((v < lo) || (v > hi)) { return; }
    int32_t const at{ static_cast<int32_t>(v) };
    if (occupied_at(spans, obstacle, face, at)) { return; }
    Wide const d{ (v < pos) ? (Wide{ pos } - v) : (v - pos) };
    if ((gap < 0) || (d < gap) || ((d == gap) && (at < best))) {
      best = at;
      gap = d;
    }
  };
  consider(imin(imax(Wide{ pos }, Wide{ lo }), Wide{ hi }));
  for (OccupiedSpan const &o : spans) {
    if ((o.obstacle != obstacle) || (o.face != face)) { continue; }
    consider(o.lo);
    consider(Wide{ o.lo } + o.len);
  }
  if (gap < 0) { return false; }
  pos = best;
  return true;
}

Router const *router_at(uint32_t index) {
  return (index < ROUTERS.size()) ? ROUTERS[index] : nullptr;
}

uint32_t router_count() { return static_cast<uint32_t>(ROUTERS.size()); }

bool router_name(uint32_t index, scav_byte const *&out, uint32_t &len) {
  if (index >= ROUTERS.size()) { return false; }
  RouterName const name{ ROUTERS[index]->name() };
  out = reinterpret_cast<scav_byte const *>(name.bytes);
  len = name.len;
  return true;
}

bool router_version(uint32_t index, uint32_t &out) {
  if (index >= ROUTERS.size()) { return false; }
  out = ROUTERS[index]->version();
  return true;
}

bool router_by_name(scav_byte const *name, uint32_t len, scav_router_id &out) {
  if (name == nullptr) { return false; }
  for (uint32_t i = 0; i < ROUTERS.size(); ++i) {
    RouterName const at{ ROUTERS[i]->name() };
    if ((at.len == len) && (std::memcmp(at.bytes, name, len) == 0)) {
      out = i;
      return true;
    }
  }
  return false;
}

}  // namespace scav
