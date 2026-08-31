// The reference builder: the measurement pass upstream of layout, and the
// per-element-kind emitters downstream of it. Standard appearance, which is one
// builder's choice and not a claim on what any builder must draw.

#include "scav/scav_draw.h"

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"
#include "scav_int.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace scav {

namespace {

// One column's rows, copied out so nothing reads past a stride it did not
// register. Empty when layout has not run.
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

// Where a line's baseline sits inside the rect that reserved it. One em below
// the top: font vertical metrics are off the table, and builder and backend
// have to agree on *some* integer, so it is this one and it is pinned by the
// golden.
int32_t baseline_of(int32_t top, int32_t font_size_grid) { return top + font_size_grid; }

ElemRef state_ref(uint32_t i) { return { .kind = ElemKind::State, .ordinal = i }; }

ElemRef sub_ref(uint32_t i) { return { .kind = ElemKind::Submachine, .ordinal = i }; }

ElemRef trans_ref(uint32_t i) { return { .kind = ElemKind::Transition, .ordinal = i }; }

uint32_t style_for_kind(StateKind kind) {
  return (kind == StateKind::Normal) ? SCAV_STYLE_STATE : SCAV_STYLE_PSEUDO;
}

// A self-transition that does not cross its source border is drawn entirely
// inside that box, so layout gives it no route and nothing can slide a box
// along one. Its label lives in the source's own reserved space instead.
bool routeless(Chart const &c, uint32_t trans) {
  Transition const &t{ c.transitions[trans] };
  return (t.src == t.dst) && (t.kind != TransKind::External);
}

bool claims_after(Chart const &c, uint32_t trans, uint32_t src) {
  return (c.transitions[trans].live != 0U) && (c.transitions[trans].label.len != 0U) &&
         routeless(c, trans) && (c.transitions[trans].src.v == src);
}

// Which line of the source's after-band a label occupies, and how many lines
// that band was reserved for: every routeless labelled transition on the same
// source takes one, in transition order.
struct AfterSlot {
  uint32_t index, total;
};

AfterSlot after_slot(Chart const &c, uint32_t trans) {
  uint32_t const src{ c.transitions[trans].src.v };
  AfterSlot slot{ .index = 0, .total = 0 };
  for (uint32_t i = 0; i < c.transitions.size(); ++i) {
    if (!claims_after(c, i, src)) { continue; }
    if (i < trans) { ++slot.index; }
    ++slot.total;
  }
  return slot;
}

}  // namespace

Palette palette_standard() {
  Palette p(SCAV_STYLE_COUNT);
  constexpr int32_t PT{ 16 };  // one point in grid units
  constexpr uint32_t INK{ 0x1A1A1AFFU };
  constexpr uint32_t PAPER{ 0xFFFFFFFFU };
  constexpr uint32_t MUTED{ 0x808080FFU };
  constexpr uint32_t NONE{ 0x00000000U };

  p[SCAV_STYLE_STATE] = { .stroke_rgba = INK,
                          .fill_rgba = PAPER,
                          .stroke_w = PT,
                          .dash = 0,
                          .font_size_grid = 0 };
  p[SCAV_STYLE_SUB] = { .stroke_rgba = MUTED,
                        .fill_rgba = NONE,
                        .stroke_w = PT,
                        .dash = 1,  // dashed, per the submachine divider
                        .font_size_grid = 0 };
  p[SCAV_STYLE_ROUTE] = { .stroke_rgba = INK,
                          .fill_rgba = INK,
                          .stroke_w = PT,
                          .dash = 0,
                          .font_size_grid = 0 };
  p[SCAV_STYLE_TITLE] = { .stroke_rgba = NONE,
                          .fill_rgba = INK,
                          .stroke_w = 0,
                          .dash = 0,
                          .font_size_grid = 12 * PT };
  p[SCAV_STYLE_LABEL] = { .stroke_rgba = NONE,
                          .fill_rgba = INK,
                          .stroke_w = 0,
                          .dash = 0,
                          .font_size_grid = 10 * PT };
  p[SCAV_STYLE_PSEUDO] = { .stroke_rgba = INK,
                           .fill_rgba = INK,
                           .stroke_w = PT,
                           .dash = 0,
                           .font_size_grid = 0 };
  return p;
}

bool measure_chart(Chart const &c, Metrics const &m, scav_profile const &p, Spaces &out) {
  out = {};
  int32_t const fs{ p.font_size_grid };
  int32_t const pad{ p.pad };
  int32_t const lh{ line_height(fs, p.line_height_k_num, p.line_height_k_den) };
  if (lh == 0) { return false; }

  // The stated policy, and the whole of it: a state reserves its name above the
  // submachine area, a submachine reserves its own name, every transition
  // leaves room for an arrowhead, and a labelled transition asks for one path
  // box. Nothing else requests anything, so a golden against this is
  // reproducible from the profile and the font alone.
  auto const measure = [&](StrRef ref, scav_extent &ext) {
    std::string_view const text{ chart_string(c, ref) };
    return measure_block(m,
                         reinterpret_cast<scav_byte const *>(text.data()),
                         static_cast<uint32_t>(text.size()),
                         fs,
                         p.line_height_k_num,
                         p.line_height_k_den,
                         ext) == MeasureStatus::Ok;
  };
  auto const fits = [](int32_t v) { return (v >= 0) && (v <= (COORD_MAX / 4)); };

  out.box_state.assign(c.states.size(), {});
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (c.states[i].live == 0U) { continue; }  // tombstones request nothing
    scav_extent title{};
    if (!measure(c.states[i].name, title)) { return false; }
    if (title.w == 0) { continue; }  // a pseudostate has no name to reserve for
    scav_box_space const box{ .min_w = title.w + (2 * pad),
                              .h_before = title.h + pad,
                              .h_after = 0 };
    if (!fits(box.min_w) || !fits(box.h_before)) { return false; }
    out.box_state[i] = box;
  }

  out.box_sub.assign(c.submachines.size(), {});
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    if (c.submachines[i].live == 0U) { continue; }
    scav_extent name{};
    if (!measure(c.submachines[i].name, name)) { return false; }
    if (name.w == 0) { continue; }
    scav_box_space const box{ .min_w = name.w + (2 * pad),
                              .h_before = name.h + pad,
                              .h_after = 0 };
    if (!fits(box.min_w) || !fits(box.h_before)) { return false; }
    out.box_sub[i] = box;
  }

  // Half an em at the destination, which is where the arrowhead goes.
  int32_t const arrow{ ceil_div(fs, 2) };
  out.path_clear.assign(c.transitions.size(), {});
  for (uint32_t i = 0; i < c.transitions.size(); ++i) {
    if (c.transitions[i].live == 0U) { continue; }
    out.path_clear[i] = { .src = 0, .dst = arrow };
  }

  for (uint32_t i = 0; i < c.transitions.size(); ++i) {
    if (c.transitions[i].live == 0U) { continue; }
    StrRef const label{ c.transitions[i].label };
    if (label.len == 0U) { continue; }
    scav_extent ext{};
    if (!measure(label, ext)) { return false; }
    if (routeless(c, i)) {
      // No route to slide along, so the source box reserves the room after its
      // submachine area and the builder draws the label there.
      scav_box_space &box{ out.box_state[c.transitions[i].src.v] };
      box.min_w = imax(box.min_w, ext.w + (2 * pad));
      box.h_after += ext.h;
      if (!fits(box.min_w) || !fits(box.h_after)) { return false; }
      continue;
    }
    scav_path_box const box{ .subject = i, .w = ext.w + pad, .h = ext.h, .order = 0 };
    if (!fits(box.w) || !fits(box.h)) { return false; }
    out.path_box.push_back(box);
    out.label.push_back(label);
  }
  return true;
}

scav_spaces as_spaces(Spaces const &s) {
  return { .box_state = s.box_state.empty() ? nullptr : s.box_state.data(),
           .n_box_state = static_cast<uint32_t>(s.box_state.size()),
           .box_sub = s.box_sub.empty() ? nullptr : s.box_sub.data(),
           .n_box_sub = static_cast<uint32_t>(s.box_sub.size()),
           .path_clear = s.path_clear.empty() ? nullptr : s.path_clear.data(),
           .n_path_clear = static_cast<uint32_t>(s.path_clear.size()),
           .path_box = s.path_box.empty() ? nullptr : s.path_box.data(),
           .n_path_box = static_cast<uint32_t>(s.path_box.size()) };
}

void emit_state(DrawList &d,
                Chart const &c,
                Metrics const &m,
                Palette const &p,
                uint32_t state,
                int32_t depth) {
  if ((state >= c.states.size()) || (c.states[state].live == 0U) ||
      (p.size() < SCAV_STYLE_COUNT)) {
    return;
  }
  std::vector<scav_rect> const boxes{ rows_of<scav_rect>(c, "scav.geom.state") };
  std::vector<scav_rect> const befores{ rows_of<scav_rect>(c, "scav.geom.state_before") };
  if (state >= boxes.size()) { return; }
  scav_rect const box{ boxes[state] };
  if ((box.w == 0) || (box.h == 0)) { return; }

  StateKind const kind{ c.states[state].kind };
  uint32_t const shape{ drawlist_style(d, p[style_for_kind(kind)]) };
  ElemRef const origin{ state_ref(state) };
  int32_t const radius{ imin(box.w, box.h) / 8 };

  switch (kind) {
    case StateKind::Normal: push_rrect(d, depth, shape, box, radius, origin); break;
    case StateKind::Initial:
    case StateKind::Junction:
      push_circle(d,
                  depth,
                  shape,
                  { .x = box.x + (box.w / 2), .y = box.y + (box.h / 2) },
                  imin(box.w, box.h) / 2,
                  origin);
      break;
    case StateKind::Final: {
      scav_point const centre{ .x = box.x + (box.w / 2), .y = box.y + (box.h / 2) };
      int32_t const outer{ imin(box.w, box.h) / 2 };
      push_circle(d, depth, drawlist_style(d, p[SCAV_STYLE_STATE]), centre, outer, origin);
      push_circle(d, depth, shape, centre, imax(1, (outer * 3) / 5), origin);
      break;
    }
    case StateKind::Choice: {
      std::array<scav_point, 4> const pts{
        { { .x = box.x + (box.w / 2), .y = box.y },
          { .x = box.x + box.w, .y = box.y + (box.h / 2) },
          { .x = box.x + (box.w / 2), .y = box.y + box.h },
          { .x = box.x, .y = box.y + (box.h / 2) } }
      };
      push_path(d, depth, drawlist_style(d, p[SCAV_STYLE_STATE]), pts.data(), 4, origin);
      break;
    }
    case StateKind::Fork:
    case StateKind::Join: push_rect(d, depth, shape, box, origin); break;
    case StateKind::History:
    case StateKind::DeepHistory: {
      scav_point const centre{ .x = box.x + (box.w / 2), .y = box.y + (box.h / 2) };
      push_circle(d,
                  depth,
                  drawlist_style(d, p[SCAV_STYLE_STATE]),
                  centre,
                  imin(box.w, box.h) / 2,
                  origin);
      std::string_view const glyph{ (kind == StateKind::History) ? "H" : "H*" };
      scav_extent ext{};
      scav_style const title{ p[SCAV_STYLE_TITLE] };
      if (measure_text(m,
                       reinterpret_cast<scav_byte const *>(glyph.data()),
                       static_cast<uint32_t>(glyph.size()),
                       title.font_size_grid,
                       ext) == MeasureStatus::Ok) {
        push_text(d,
                  depth,
                  drawlist_style(d, title),
                  { .x = centre.x - (ext.w / 2),
                    .y = baseline_of(centre.y - (ext.h / 2), title.font_size_grid) },
                  glyph,
                  origin);
      }
      break;
    }
  }

  // The name goes in the rect its own h_before reserved. Layout never learned
  // what a title is; it reserved the space and this decides what fills it.
  std::string_view const name{ chart_string(c, c.states[state].name) };
  if (name.empty() || (state >= befores.size())) { return; }
  scav_rect const before{ befores[state] };
  if (before.h == 0) { return; }
  scav_style const title{ p[SCAV_STYLE_TITLE] };
  uint32_t const title_style{ drawlist_style(d, title) };
  int32_t const lh{ line_height(title.font_size_grid, 1, 1) };
  int32_t line{ 0 };
  for (std::string_view const &text : text_lines(name)) {
    push_text(d,
              depth,
              title_style,
              { .x = before.x + (before.w / 8),
                .y = baseline_of(before.y + (line * lh), title.font_size_grid) },
              text,
              origin);
    ++line;
  }
}

void emit_submachine(DrawList &d,
                     Chart const &c,
                     Palette const &p,
                     uint32_t sub,
                     int32_t depth) {
  if ((sub >= c.submachines.size()) || (c.submachines[sub].live == 0U) ||
      (p.size() < SCAV_STYLE_COUNT)) {
    return;
  }
  std::vector<scav_rect> const rects{ rows_of<scav_rect>(c, "scav.geom.sub") };
  if (sub >= rects.size()) { return; }
  scav_rect const r{ rects[sub] };
  if ((r.w == 0) || (r.h == 0)) { return; }
  // Only a sibling draws a divider: the first submachine of a state has no
  // boundary above it, and a lone submachine is not a concurrent region.
  if (c.submachines[sub].ordinal == 0U) { return; }
  push_line(d,
            depth,
            drawlist_style(d, p[SCAV_STYLE_SUB]),
            { .x = r.x, .y = r.y },
            { .x = r.x + r.w, .y = r.y },
            sub_ref(sub));
}

void emit_route(DrawList &d,
                Chart const &c,
                Palette const &p,
                uint32_t trans,
                int32_t depth) {
  if ((trans >= c.transitions.size()) || (c.transitions[trans].live == 0U) ||
      (p.size() < SCAV_STYLE_COUNT)) {
    return;
  }
  std::vector<scav_span> const routes{ rows_of<scav_span>(c, "scav.geom.route") };
  std::vector<scav_point> const points{ rows_of<scav_point>(c, "scav.geom.point") };
  if (trans >= routes.size()) { return; }
  scav_span const r{ routes[trans] };
  if (r.len < 2U) { return; }

  uint32_t const style{ drawlist_style(d, p[SCAV_STYLE_ROUTE]) };
  ElemRef const origin{ trans_ref(trans) };
  push_polyline(d, depth, style, points.data() + r.off, r.len, origin);
  push_arrowhead(d,
                 depth,
                 style,
                 points[r.off + r.len - 1U],
                 points[r.off + r.len - 2U],
                 p[SCAV_STYLE_LABEL].font_size_grid / 2,
                 origin);
}

bool label_box(Chart const &c,
               scav_spaces const &s,
               scav_placed const *placed,
               uint32_t placed_count,
               uint32_t trans,
               scav_rect &out) {
  if ((trans >= c.transitions.size()) || (c.transitions[trans].live == 0U)) {
    return false;
  }
  if (routeless(c, trans)) {
    // The band the source reserved, sliced into one line per label that claimed
    // it. Nothing placed a box, because there was no route to slide one along.
    if (c.transitions[trans].label.len == 0U) { return false; }
    std::vector<scav_rect> const afters{ rows_of<scav_rect>(c, "scav.geom.state_after") };
    uint32_t const src{ c.transitions[trans].src.v };
    if ((src >= afters.size()) || (afters[src].h == 0)) { return false; }
    AfterSlot const slot{ after_slot(c, trans) };
    scav_rect const band{ afters[src] };
    int32_t const each{ (slot.total > 0U) ? (band.h / static_cast<int32_t>(slot.total))
                                          : band.h };
    out = { .x = band.x,
            .y = band.y + (static_cast<int32_t>(slot.index) * each),
            .w = band.w,
            .h = each };
    return true;
  }
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    if (s.path_box[i].subject != trans) { continue; }
    if ((i >= placed_count) || (placed == nullptr)) { return false; }
    out = placed[i];
    return true;
  }
  return false;
}

void emit_label(DrawList &d,
                Chart const &c,
                Metrics const &m,
                Palette const &p,
                uint32_t trans,
                scav_rect box,
                int32_t depth) {
  if ((trans >= c.transitions.size()) || (c.transitions[trans].live == 0U) ||
      (p.size() < SCAV_STYLE_COUNT)) {
    return;
  }
  std::string_view const text{ chart_string(c, c.transitions[trans].label) };
  if (text.empty()) { return; }

  scav_style const style{ p[SCAV_STYLE_LABEL] };
  uint32_t const label_style{ drawlist_style(d, style) };
  int32_t const lh{ line_height(style.font_size_grid, 1, 1) };
  ElemRef const origin{ trans_ref(trans) };
  std::vector<std::string_view> const lines{ text_lines(text) };
  int32_t const block_h{ lh * static_cast<int32_t>(lines.size()) };

  // Centred in the rect layout placed, which is where the room actually is. A
  // placed box may exceed what was asked for, so recomputing one here would
  // drift the moment a router stops centring on the route midpoint.
  int32_t const top{ box.y + floor_div(box.h - block_h, 2) };
  int32_t line{ 0 };
  for (std::string_view const &one : lines) {
    scav_extent ext{};
    if (measure_text(m,
                     reinterpret_cast<scav_byte const *>(one.data()),
                     static_cast<uint32_t>(one.size()),
                     style.font_size_grid,
                     ext) != MeasureStatus::Ok) {
      return;
    }
    push_text(d,
              depth,
              label_style,
              { .x = box.x + floor_div(box.w - ext.w, 2),
                .y = baseline_of(top + (line * lh), style.font_size_grid) },
              one,
              origin);
    ++line;
  }
}

bool emit_chart(DrawList &d,
                Chart const &c,
                Metrics const &m,
                Palette const &p,
                scav_spaces const &s,
                scav_placed const *placed,
                uint32_t placed_count,
                int32_t depth) {
  if ((p.size() < SCAV_STYLE_COUNT) || (column_find(c, "scav.geom.state").v == INVALID)) {
    return false;
  }
  // Submachines, states, routes, then labels: an order this function documents
  // and nothing else depends on. Every primitive lands at the one depth it was
  // given, so a caller that wants layering calls the emitters itself.
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    emit_submachine(d, c, p, i, depth);
  }
  for (uint32_t i = 0; i < c.states.size(); ++i) { emit_state(d, c, m, p, i, depth); }
  for (uint32_t i = 0; i < c.transitions.size(); ++i) { emit_route(d, c, p, i, depth); }
  for (uint32_t i = 0; i < c.transitions.size(); ++i) {
    scav_rect box{};
    if (label_box(c, s, placed, placed_count, i, box)) {
      emit_label(d, c, m, p, i, box, depth);
    }
  }
  return true;
}

}  // namespace scav
