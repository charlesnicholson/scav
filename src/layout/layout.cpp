// Sizes composed bottom-up by the box formula, boxes stacked top-down in
// document order, straight-line routes through the decomposition's ports, and
// the geometry columns as the only output.

#include "layout/decompose.h"
#include "layout/wire.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"
#include "scav_xxhash.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace scav {

namespace {

// Every box the pipeline computes, in the columns' own shapes. Sizing fills
// each rect's extent and its position relative to the parent submachine;
// placement makes positions root-absolute in one descent. Tombstones stay
// all-zero throughout.
struct Geometry {
  std::vector<scav_rect> state, before, after, sub;
  scav_rect chart{};
  std::vector<scav_point> points;
  std::vector<scav_port_slot> slots;
  std::vector<scav_span> routes, port_spans;  // parallel to transitions
};

scav_box_space box_of(scav_box_space const *rows, uint32_t count, uint32_t i) {
  return ((rows != nullptr) && (i < count)) ? rows[i] : scav_box_space{};
}

void overflow(std::vector<Diagnostic> &diags, ElemKind kind, uint32_t ordinal) {
  diags.push_back({ .code = DiagCode::CoordinateOverflow,
                    .subject = { .kind = kind, .ordinal = ordinal },
                    .doc = { INVALID },
                    .src = {} });
}

// One pass per depth level, deepest first: a submachine needs its children
// sized, a state needs its submachines, and depth orders both.
bool size_boxes(Chart const &c,
                SplitGraph const &g,
                scav_spaces const &s,
                scav_profile const &p,
                Geometry &geo,
                std::vector<Diagnostic> &diags) {
  geo.state.assign(c.states.size(), {});
  geo.before.assign(c.states.size(), {});
  geo.after.assign(c.states.size(), {});
  geo.sub.assign(c.submachines.size(), {});

  uint32_t max_depth{ 0 };
  for (uint32_t const d : g.state_depth) { max_depth = imax(max_depth, d); }
  std::vector<std::vector<uint32_t>> states_at(max_depth + 1);
  std::vector<std::vector<uint32_t>> subs_at(max_depth + 2);
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (c.states[i].live != 0) { states_at[g.state_depth[i]].push_back(i); }
  }
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    StateId const owner{ c.submachines[m].owner };
    subs_at[(owner.v == INVALID) ? 0 : g.state_depth[owner.v] + 1].push_back(m);
  }

  bool ok{ true };
  // Children pack into rows against the width approximation -- isqrt of the
  // total area scaled by the desired aspect ratio -- in document order.
  auto const size_sub = [&](uint32_t m) {
    Span const kids{ c.submachines[m].children };
    Wide area{ 0 };
    Wide widest{ 0 };
    for (uint32_t k = 0; k < kids.len; ++k) {
      scav_rect const &r{ geo.state[c.state_ids[kids.off + k].v] };
      area += static_cast<Wide>(r.w) * r.h;  // tombstones are zero-area
      widest = imax(widest, Wide{ r.w });
    }
    constexpr Wide AREA_MAX{ static_cast<Wide>(COORD_MAX) * COORD_MAX };
    if (area > AREA_MAX) {
      overflow(diags, ElemKind::Submachine, m);
      ok = false;
      return;
    }
    Wide const target{ imax(widest,
                            static_cast<Wide>(isqrt(static_cast<uint64_t>(
                                floor_div(area * p.dar_num, Wide{ p.dar_den }))))) };

    Wide cx{ 0 };
    Wide row_y{ 0 };
    Wide row_h{ 0 };
    Wide ext_w{ 0 };
    for (uint32_t k = 0; k < kids.len; ++k) {
      uint32_t const child{ c.state_ids[kids.off + k].v };
      if (c.states[child].live == 0) { continue; }
      scav_rect &r{ geo.state[child] };
      if ((cx > 0) && ((cx + r.w) > target)) {
        row_y += row_h + p.pad;
        cx = 0;
        row_h = 0;
      }
      if ((row_y + r.h) > COORD_MAX) {
        overflow(diags, ElemKind::Submachine, m);
        ok = false;
        return;
      }
      r.x = static_cast<int32_t>(cx);
      r.y = static_cast<int32_t>(row_y);
      cx += r.w + p.pad;
      row_h = imax(row_h, Wide{ r.h });
      ext_w = imax(ext_w, cx - p.pad);
    }
    // Width needs its own bound: the aspect target may exceed the domain, and
    // the root submachine has no wrapping state whose check would catch it.
    if (ext_w > COORD_MAX) {
      overflow(diags, ElemKind::Submachine, m);
      ok = false;
      return;
    }
    geo.sub[m].w = static_cast<int32_t>(ext_w);
    geo.sub[m].h = static_cast<int32_t>(row_y + row_h);
  };

  auto const size_state = [&](uint32_t i) {
    Wide subs_w{ 0 };
    Wide subs_h{ 0 };
    uint32_t n{ 0 };
    Span const subs{ c.states[i].submachines };
    for (uint32_t k = 0; k < subs.len; ++k) {
      uint32_t const m{ c.submachine_ids[subs.off + k].v };
      if (c.submachines[m].live == 0) { continue; }
      subs_w = imax(subs_w, Wide{ geo.sub[m].w });
      subs_h += geo.sub[m].h;
      ++n;
    }
    if (n > 1) { subs_h += static_cast<Wide>(p.pad) * (n - 1); }

    scav_box_space const b{ box_of(s.box_state, s.n_box_state, i) };
    uint32_t const kind{ static_cast<uint32_t>(c.states[i].kind) };
    Wide const w{ imax(imax(Wide{ b.min_w }, subs_w), Wide{ p.kind_min_w[kind] }) +
                  (2 * static_cast<Wide>(p.pad)) };
    Wide const h{ imax(Wide{ b.h_before } + subs_h + b.h_after,
                       Wide{ p.kind_min_h[kind] }) +
                  (2 * static_cast<Wide>(p.pad)) };
    if ((w > COORD_MAX) || (h > COORD_MAX)) {
      overflow(diags, ElemKind::State, i);
      ok = false;
      return;
    }
    geo.state[i].w = static_cast<int32_t>(w);
    geo.state[i].h = static_cast<int32_t>(h);
  };

  // Levels interleave: submachines whose children sit at this depth, then the
  // states one level up that wrap them; level 0 sizes the document roots.
  for (uint32_t level = max_depth + 2; level-- > 0;) {
    for (uint32_t const m : subs_at[level]) { size_sub(m); }
    if (level > 0) {
      for (uint32_t const i : states_at[level - 1]) { size_state(i); }
    }
  }
  return ok;
}

// One descent from the root, adding each frame's origin to the relative
// positions sizing left behind. Everything stays inside the sized extents,
// so int32 arithmetic cannot leave the domain here.
void place_boxes(Chart const &c,
                 scav_spaces const &s,
                 scav_profile const &p,
                 Geometry &geo) {
  struct Frame {
    uint32_t sub;
    int32_t x, y;
  };
  std::vector<Frame> work;
  if (c.root_submachine.v != INVALID) {
    geo.chart = geo.sub[c.root_submachine.v];
    work.push_back({ .sub = c.root_submachine.v, .x = 0, .y = 0 });
  }
  while (!work.empty()) {
    Frame const at{ work.back() };
    work.pop_back();
    geo.sub[at.sub].x = at.x;
    geo.sub[at.sub].y = at.y;
    Span const kids{ c.submachines[at.sub].children };
    for (uint32_t k = 0; k < kids.len; ++k) {
      uint32_t const i{ c.state_ids[kids.off + k].v };
      if (c.states[i].live == 0) { continue; }
      scav_rect &r{ geo.state[i] };
      r.x += at.x;
      r.y += at.y;

      scav_box_space const b{ box_of(s.box_state, s.n_box_state, i) };
      int32_t const ix{ r.x + p.pad };
      int32_t const iw{ r.w - (2 * p.pad) };
      geo.before[i] = { .x = ix, .y = r.y + p.pad, .w = iw, .h = b.h_before };
      int32_t sy{ r.y + p.pad + b.h_before };
      Span const subs{ c.states[i].submachines };
      uint32_t placed{ 0 };
      for (uint32_t u = 0; u < subs.len; ++u) {
        uint32_t const m{ c.submachine_ids[subs.off + u].v };
        if (c.submachines[m].live == 0) { continue; }
        work.push_back({ .sub = m, .x = ix, .y = sy });  // concurrent regions stack
        sy += geo.sub[m].h + p.pad;
        ++placed;
      }
      if (placed > 0) { sy -= p.pad; }
      geo.after[i] = { .x = ix, .y = sy, .w = iw, .h = b.h_after };
    }
  }
}

scav_point center(scav_rect const &r) {
  return { .x = r.x + floor_div(r.w, 2), .y = r.y + floor_div(r.h, 2) };
}

// A port sits at the midpoint of the boundary side facing `toward`, the side
// chosen by the dominant axis between the two centers.
scav_port_slot port_point(scav_rect const &b, scav_point toward, uint32_t depth) {
  scav_point const bc{ center(b) };
  int32_t const dx{ toward.x - bc.x };
  int32_t const dy{ toward.y - bc.y };
  int32_t const ax{ (dx < 0) ? -dx : dx };
  int32_t const ay{ (dy < 0) ? -dy : dy };
  if (ax >= ay) {
    return { .x = (dx >= 0) ? b.x + b.w : b.x,
             .y = bc.y,
             .side = (dx >= 0) ? 1U : 0U,
             .boundary_depth = depth };
  }
  return { .x = bc.x,
           .y = (dy >= 0) ? b.y + b.h : b.y,
           .side = (dy >= 0) ? 3U : 2U,
           .boundary_depth = depth };
}

// Moves `a` toward `b` by `amount`, capped at half the distance so the two
// ends cannot cross.
scav_point trim(scav_point a, scav_point b, int32_t amount) {
  if (amount <= 0) { return a; }
  Wide const dx{ static_cast<Wide>(b.x) - a.x };
  Wide const dy{ static_cast<Wide>(b.y) - a.y };
  Wide const len{ static_cast<Wide>(isqrt(static_cast<uint64_t>((dx * dx) + (dy * dy)))) };
  if (len == 0) { return a; }
  Wide const k{ imin(Wide{ amount }, floor_div(len, Wide{ 2 })) };
  return { .x = a.x + static_cast<int32_t>(floor_div(dx * k, len)),
           .y = a.y + static_cast<int32_t>(floor_div(dy * k, len)) };
}

void route_transitions(Chart const &c,
                       SplitGraph const &g,
                       scav_spaces const &s,
                       scav_profile const &p,
                       Geometry &geo) {
  uint32_t const n{ static_cast<uint32_t>(c.transitions.size()) };
  geo.routes.assign(n, {});
  geo.port_spans.assign(n, {});
  for (uint32_t t = 0; t < n; ++t) {
    Span const segs{ g.trans_segments[t] };
    if (segs.len == 0) { continue; }
    Transition const &tr{ c.transitions[t] };
    uint32_t const first_point{ static_cast<uint32_t>(geo.points.size()) };
    uint32_t const first_slot{ static_cast<uint32_t>(geo.slots.size()) };

    if (tr.src == tr.dst) {
      // The external self-loop: out the right side and back.
      scav_rect const r{ geo.state[tr.src.v] };
      scav_point const lip{ .x = r.x + r.w, .y = r.y + floor_div(r.h, 2) };
      geo.points.push_back(lip);
      geo.points.push_back({ .x = lip.x + (2 * p.pad), .y = lip.y });
    } else {
      scav_point const to{ center(geo.state[tr.dst.v]) };
      geo.points.push_back(center(geo.state[tr.src.v]));
      for (uint32_t k = 0; k + 1 < segs.len; ++k) {
        SplitPort const &port{ g.ports[g.segments[segs.off + k].dst_port] };
        bool const on_state{ port.state.v != INVALID };
        scav_rect const b{ on_state ? geo.state[port.state.v] : geo.sub[port.sub.v] };
        uint32_t const depth{ on_state
                                  ? g.state_depth[port.state.v]
                                  : g.state_depth[c.submachines[port.sub.v].owner.v] + 1 };
        scav_port_slot const slot{ port_point(b, to, depth) };
        geo.slots.push_back(slot);
        geo.points.push_back({ .x = slot.x, .y = slot.y });
      }
      geo.points.push_back(to);
    }

    if ((s.path_clear != nullptr) && (t < s.n_path_clear)) {
      scav_point *const pts{ geo.points.data() + first_point };
      uint32_t const count{ static_cast<uint32_t>(geo.points.size()) - first_point };
      pts[0] = trim(pts[0], pts[1], s.path_clear[t].src);
      pts[count - 1] = trim(pts[count - 1], pts[count - 2], s.path_clear[t].dst);
    }
    geo.routes[t] = { .off = first_point,
                      .len = static_cast<uint32_t>(geo.points.size()) - first_point };
    geo.port_spans[t] = { .off = first_slot,
                          .len = static_cast<uint32_t>(geo.slots.size()) - first_slot };
  }
}

// Registered on first use, found thereafter; every run overwrites in place.
ColumnId geom_column(Chart &c,
                     char const *name,
                     ElemKind entity,
                     ValueKind kind,
                     uint32_t elem_size) {
  ColumnId const found{ column_find(c, name) };
  if (found.v != INVALID) { return found; }
  return column_register(c, name, entity, kind, elem_size, 4, COLUMN_DERIVED);
}

template <typename T>
void write_rows(Chart &c, ColumnId id, std::vector<T> const &rows) {
  if (!rows.empty()) {
    std::memcpy(column_data(c, id), rows.data(), rows.size() * sizeof(T));
  }
}

void write_columns(Chart &c, Geometry const &geo) {
  constexpr uint32_t RECT{ sizeof(scav_rect) };
  write_rows(c,
             geom_column(c, "scav.geom.state", ElemKind::State, ValueKind::Pod, RECT),
             geo.state);
  write_rows(
      c,
      geom_column(c, "scav.geom.state_before", ElemKind::State, ValueKind::Pod, RECT),
      geo.before);
  write_rows(
      c,
      geom_column(c, "scav.geom.state_after", ElemKind::State, ValueKind::Pod, RECT),
      geo.after);
  write_rows(c,
             geom_column(c, "scav.geom.sub", ElemKind::Submachine, ValueKind::Pod, RECT),
             geo.sub);
  write_rows(c,
             geom_column(c, "scav.geom.route", ElemKind::Transition, ValueKind::Span, 8),
             geo.routes);
  write_rows(c,
             geom_column(c, "scav.geom.port", ElemKind::Transition, ValueKind::Span, 8),
             geo.port_spans);

  ColumnId const pts{
    geom_column(c, "scav.geom.point", ElemKind::Point, ValueKind::Pod, 8)
  };
  column_resize(c, pts, static_cast<uint32_t>(geo.points.size()));
  write_rows(c, pts, geo.points);
  ColumnId const slots{ geom_column(c,
                                    "scav.geom.portslot",
                                    ElemKind::Point,
                                    ValueKind::Pod,
                                    sizeof(scav_port_slot)) };
  column_resize(c, slots, static_cast<uint32_t>(geo.slots.size()));
  write_rows(c, slots, geo.slots);

  ColumnId const chart{
    geom_column(c, "scav.geom.chart", ElemKind::Chart, ValueKind::Pod, RECT)
  };
  std::memcpy(column_data(c, chart), &geo.chart, sizeof(geo.chart));

  ColumnId const gen{
    geom_column(c, "scav.geom.gen", ElemKind::Chart, ValueKind::U32, 4)
  };
  uint32_t n{ 0 };
  std::memcpy(&n, column_data(c, gen), 4);
  ++n;
  std::memcpy(column_data(c, gen), &n, 4);
}

}  // namespace

bool layout_run(Chart &c,
                scav_spaces const &s,
                scav_profile const &p,
                std::vector<scav_placed> &placed,
                std::vector<Diagnostic> &diags) {
  if (!profile_validate(p)) {
    diags.push_back({ .code = DiagCode::ProfileOutOfRange,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }
  if (!spaces_validate(c, s, diags)) { return false; }

  SplitGraph const g{ decompose(c) };
  Geometry geo;
  if (!size_boxes(c, g, s, p, geo, diags)) { return false; }
  place_boxes(c, s, p, geo);
  route_transitions(c, g, s, p, geo);

  placed.assign(s.n_path_box, {});
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    scav_path_box const &pb{ s.path_box[i] };
    scav_span const route{ geo.routes[pb.subject] };
    scav_point const mid{ (route.len == 0) ? scav_point{}
                                           : geo.points[route.off + (route.len / 2)] };
    placed[i] = { .x = mid.x - floor_div(pb.w, 2),
                  .y = mid.y - floor_div(pb.h, 2),
                  .w = pb.w,
                  .h = pb.h };
  }

  write_columns(c, geo);
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
