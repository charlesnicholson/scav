// The four phases in a line, then the geometry columns as the only output.
// Everything else here is the columns and the two hashes over them.

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/router.h"
#include "layout/size.h"
#include "layout/wire.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"
#include "scav_xxhash.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace scav {

namespace {

constexpr uint32_t RECT{ sizeof(scav_rect) };

// One name and the shape it is registered under. `write_rows` copies a row per
// entity through whichever column carries the name, so the shape has to be the
// one layout would have registered or the copy runs past the column's bytes.
struct GeomShape {
  char const *name;
  ElemKind entity;
  ValueKind kind;
  uint32_t elem_size;
};

// Index into GEOM, so the writer below and the check in `layout_run` name the
// same row rather than repeating its fields.
enum GeomColumnIndex : uint32_t {
  GEOM_STATE,
  GEOM_BEFORE,
  GEOM_AFTER,
  GEOM_SUB,
  GEOM_ROUTE,
  GEOM_PORT,
  GEOM_POINT,
  GEOM_PORTSLOT,
  GEOM_CHART,
  GEOM_INPUTS,
  GEOM_GEN,
  GEOM_COUNT,
};

constexpr std::array<GeomShape, GEOM_COUNT> GEOM{ {
    { "scav.geom.state", ElemKind::State, ValueKind::Pod, RECT },
    { "scav.geom.state_before", ElemKind::State, ValueKind::Pod, RECT },
    { "scav.geom.state_after", ElemKind::State, ValueKind::Pod, RECT },
    { "scav.geom.sub", ElemKind::Submachine, ValueKind::Pod, RECT },
    { "scav.geom.route", ElemKind::Transition, ValueKind::Span, 8 },
    { "scav.geom.port", ElemKind::Transition, ValueKind::Span, 8 },
    { "scav.geom.point", ElemKind::Point, ValueKind::Pod, 8 },
    { "scav.geom.portslot", ElemKind::Point, ValueKind::Pod, sizeof(scav_port_slot) },
    { "scav.geom.chart", ElemKind::Chart, ValueKind::Pod, RECT },
    { "scav.geom.inputs", ElemKind::Chart, ValueKind::U32, 4 },
    { "scav.geom.gen", ElemKind::Chart, ValueKind::U32, 4 },
} };

// Registered on first use, found thereafter; every run overwrites in place. The
// found column's shape was checked before any geometry was computed.
ColumnId geom_column(Chart &c, GeomShape const &g) {
  ColumnId const found{ column_find(c, g.name) };
  if (found.v != INVALID) { return found; }
  return column_register(c, g.name, g.entity, g.kind, g.elem_size, 4, COLUMN_DERIVED);
}

// The first name already registered under another entity, value kind, or
// element size. GEOM_COUNT when every one of them is layout's own to write.
uint32_t geom_column_clash(Chart const &c) {
  for (uint32_t i = 0; i < GEOM_COUNT; ++i) {
    ColumnId const found{ column_find(c, GEOM[i].name) };
    if (found.v == INVALID) { continue; }
    ColumnDesc const &d{ c.columns[found.v].desc };
    if ((d.entity != GEOM[i].entity) || (d.kind != GEOM[i].kind) ||
        (d.elem_size != GEOM[i].elem_size)) {
      return i;
    }
  }
  return GEOM_COUNT;
}

template <typename T>
void write_rows(Chart &c, ColumnId id, std::vector<T> const &rows) {
  if (!rows.empty()) {
    std::memcpy(column_data(c, id), rows.data(), rows.size() * sizeof(T));
  }
}

static_assert(sizeof(scav_profile) == 46 * sizeof(int32_t),
              "the profile must stay a flat block of int32 with no padding, or the "
              "inputs digest below would hash bytes whose values are unspecified");

// Every non-geometry input a golden depends on. Without it a hash names the
// numbers that came out and not the run that produced them.
uint32_t inputs_digest(scav_spaces const &s, scav_layout_opts const &o) {
  std::vector<scav_byte> b;
  // Padding is what forbids hashing a struct's bytes, and the assert above
  // proves there is none, so the copy reads all 46 knobs and nothing else.
  std::array<int32_t, sizeof(scav_profile) / sizeof(int32_t)> profile{};
  std::memcpy(profile.data(), &o.profile, sizeof(scav_profile));
  for (int32_t const field : profile) { append_i32(b, field); }

  scav_byte const *name{ nullptr };
  uint32_t name_len{ 0 };
  uint32_t version{ 0 };
  if (router_name(o.router, name, name_len) && router_version(o.router, version)) {
    append_u32(b, name_len);
    b.insert(b.end(), name, name + name_len);
    append_u32(b, version);
  }
  // The font reaches layout only as the integers it measured, so its identity
  // rides in here rather than as an argument layout would never read.
  append_u32(b, spaces_digest(s));
  return xxhash32(b.data(), b.size(), 0);
}

void write_columns(Chart &c, SizedLayout const &z, Routes const &r, uint32_t inputs) {
  write_rows(c, geom_column(c, GEOM[GEOM_STATE]), z.state);
  write_rows(c, geom_column(c, GEOM[GEOM_BEFORE]), z.before);
  write_rows(c, geom_column(c, GEOM[GEOM_AFTER]), z.after);
  write_rows(c, geom_column(c, GEOM[GEOM_SUB]), z.sub);
  write_rows(c, geom_column(c, GEOM[GEOM_ROUTE]), r.route);
  write_rows(c, geom_column(c, GEOM[GEOM_PORT]), r.port);

  ColumnId const pts{ geom_column(c, GEOM[GEOM_POINT]) };
  column_resize(c, pts, static_cast<uint32_t>(r.points.size()));
  write_rows(c, pts, r.points);
  ColumnId const slots{ geom_column(c, GEOM[GEOM_PORTSLOT]) };
  column_resize(c, slots, static_cast<uint32_t>(r.slots.size()));
  write_rows(c, slots, r.slots);

  ColumnId const chart{ geom_column(c, GEOM[GEOM_CHART]) };
  std::memcpy(column_data(c, chart), &z.chart, sizeof(z.chart));

  ColumnId const in{ geom_column(c, GEOM[GEOM_INPUTS]) };
  std::memcpy(column_data(c, in), &inputs, 4);

  ColumnId const gen{ geom_column(c, GEOM[GEOM_GEN]) };
  uint32_t n{ 0 };
  std::memcpy(&n, column_data(c, gen), 4);
  ++n;
  std::memcpy(column_data(c, gen), &n, 4);
}

}  // namespace

bool layout_run(Chart &c,
                scav_spaces const &s,
                scav_layout_opts const &o,
                std::vector<scav_placed> &placed,
                std::vector<Diagnostic> &diags) {
  scav_profile const &p{ o.profile };
  if (!profile_validate(p)) {
    diags.push_back({ .code = DiagCode::ProfileOutOfRange,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }
  if (!spaces_validate(c, s, diags)) { return false; }

  if (geom_column_clash(c) != GEOM_COUNT) {
    diags.push_back({ .code = DiagCode::GeometryColumnClash,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }

  Router const *const router{ router_at(o.router) };
  if (router == nullptr) {
    diags.push_back({ .code = DiagCode::RouterUnknown,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }

  SplitGraph const g{ decompose(c) };
  SubmachineOrders const orders{ phase1_order(c, g, s, p) };
  SizedLayout sized;
  if (!phase2_size(c, g, orders, s, p, sized, diags)) { return false; }
  Routes const routes{ phase3_route(c, g, orders, sized, s, p, *router) };
  placed = routes.placed;

  // Bounds everything laid out, not just the root submachine: a route bends into
  // a frame's padding and a path box centres on one, so both can reach past it.
  auto const cover = [&sized](int32_t x, int32_t y) {
    int32_t const right{ imax(sized.chart.x + sized.chart.w, x) };
    int32_t const bottom{ imax(sized.chart.y + sized.chart.h, y) };
    sized.chart.x = imin(sized.chart.x, x);
    sized.chart.y = imin(sized.chart.y, y);
    sized.chart.w = right - sized.chart.x;
    sized.chart.h = bottom - sized.chart.y;
  };
  for (scav_point const &at : routes.points) { cover(at.x, at.y); }
  for (scav_rect const &at : routes.placed) {
    cover(at.x, at.y);
    cover(at.x + at.w, at.y + at.h);
  }

  write_columns(c, sized, routes, inputs_digest(s, o));
  return true;
}

namespace {

// The column's rows, memcpy'd out so hashing never reads padding in place.
template <typename T>
std::vector<T> rows_of(Chart const &c, char const *name) {
  ColumnId const id{ column_find(c, name) };
  if (id.v == INVALID) { return {}; }
  std::vector<T> rows(column_count(c, id));
  if (!rows.empty()) {
    std::memcpy(rows.data(), column_data(c, id), rows.size() * sizeof(T));
  }
  return rows;
}

// 0, 1, or 2: decreasing, level, or increasing along one axis.
uint32_t direction_token(int32_t from, int32_t to) {
  if (to > from) { return 2U; }
  return (to < from) ? 0U : 1U;
}

}  // namespace

uint32_t layout_inputs_digest(Chart const &c) {
  ColumnId const id{ column_find(c, "scav.geom.inputs") };
  if ((id.v == INVALID) || (column_count(c, id) == 0)) { return 0; }
  uint32_t inputs{ 0 };
  std::memcpy(&inputs, column_data(c, id), 4);
  return inputs;
}

uint32_t layout_coordinate_hash(Chart const &c) {
  std::vector<scav_byte> b;
  for (char const *name : { "scav.geom.state",
                            "scav.geom.state_before",
                            "scav.geom.state_after",
                            "scav.geom.sub",
                            "scav.geom.chart" }) {
    for (scav_rect const &r : rows_of<scav_rect>(c, name)) {
      append_i32(b, r.x);
      append_i32(b, r.y);
      append_i32(b, r.w);
      append_i32(b, r.h);
    }
  }
  for (scav_point const &pt : rows_of<scav_point>(c, "scav.geom.point")) {
    append_i32(b, pt.x);
    append_i32(b, pt.y);
  }
  for (scav_port_slot const &sl : rows_of<scav_port_slot>(c, "scav.geom.portslot")) {
    append_i32(b, sl.x);
    append_i32(b, sl.y);
  }
  return xxhash32(b.data(), b.size(), 0);
}

uint32_t layout_structural_hash(Chart const &c) {
  std::vector<scav_byte> b;
  std::vector<scav_point> const points{ rows_of<scav_point>(c, "scav.geom.point") };
  for (scav_span const route : rows_of<scav_span>(c, "scav.geom.route")) {
    append_u32(b, route.len);
    // Direction tokens, not coordinates: a translation leaves these alone.
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      scav_point const &a{ points[route.off + k] };
      scav_point const &d{ points[route.off + k + 1] };
      append_u32(b, (direction_token(a.x, d.x) * 3U) + direction_token(a.y, d.y));
    }
  }
  for (scav_port_slot const &sl : rows_of<scav_port_slot>(c, "scav.geom.portslot")) {
    append_u32(b, sl.side);
    append_u32(b, sl.boundary_depth);
  }
  return xxhash32(b.data(), b.size(), 0);
}

}  // namespace scav
