// The measurement pass that becomes the goldens' stated policy, and the
// emitters that read back what layout did with it.

#include "scav/scav_draw.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include "draw/handles.h"
#include "scav_c_handles.h"

#include "doctest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

Metrics bundled() {
  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  return m;
}

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

scav_layout_opts opts(scav_profile const &p) {
  return { .profile = p, .router = 0, .threads = 0 };
}

// A state, its submachine, two children and a labelled transition: the smallest
// chart that exercises every table the measurement pass fills.
Chart small_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const on{ build_state(c, root, "Running", StateKind::Normal, {}) };
  SubmachineId const main{ build_submachine(c, on, "main", {}) };
  StateId const idle{ build_state(c, main, "Idle", StateKind::Normal, {}) };
  StateId const busy{ build_state(c, main, "Busy", StateKind::Normal, {}) };
  build_trans(c, idle, busy, TransKind::External, "work arrived");
  build_trans(c, busy, idle, TransKind::External, {});  // no label, so no box
  return c;
}

// Measure, lay out, then build: the whole pipeline, which is the only way the
// emitters have geometry to read.
struct Built {
  Chart chart;
  Spaces spaces;
  std::vector<scav_placed> placed;
  DrawList list;
};

Built pipeline(Chart c, scav_profile const &p) {
  Built b{ .chart = std::move(c), .spaces = {}, .placed = {}, .list = {} };
  Metrics const m{ bundled() };
  REQUIRE(measure_chart(b.chart, m, p, b.spaces));
  std::vector<Diagnostic> diags;
  REQUIRE(layout_run(b.chart, as_spaces(b.spaces), opts(p), b.placed, diags));
  REQUIRE(emit_chart(b.list,
                     b.chart,
                     m,
                     palette_standard(),
                     as_spaces(b.spaces),
                     b.placed.data(),
                     static_cast<uint32_t>(b.placed.size()),
                     0));
  uint32_t bad{ 0 };
  REQUIRE_MESSAGE(drawlist_validate(b.list, bad), bad);
  return b;
}

uint32_t kind_count(DrawList const &d, uint32_t kind) {
  uint32_t n{ 0 };
  for (scav_prim const &p : d.prims) {
    if (p.kind == kind) { ++n; }
  }
  return n;
}

std::string payload(DrawList const &d, scav_prim const &p) {
  return { reinterpret_cast<char const *>(d.text.bytes.data() + p.payload.off),
           p.payload.len };
}

bool has_text(DrawList const &d, std::string_view want) {
  for (scav_prim const &p : d.prims) {
    if ((p.kind == SCAV_PRIM_TEXT) && (payload(d, p) == want)) { return true; }
  }
  return false;
}

}  // namespace

TEST_CASE("builder: the standard palette fills every slot the builder indexes") {
  Palette const p{ palette_standard() };
  REQUIRE(p.size() == SCAV_STYLE_COUNT);
  // Text styles carry a size and shape styles do not, which is what tells the
  // backend whether a primitive is glyphs or geometry.
  CHECK(p[SCAV_STYLE_TITLE].font_size_grid > 0);
  CHECK(p[SCAV_STYLE_LABEL].font_size_grid > 0);
  CHECK(p[SCAV_STYLE_STATE].font_size_grid == 0);
  CHECK(p[SCAV_STYLE_STATE].stroke_w > 0);
  CHECK(p[SCAV_STYLE_SUB].dash != 0);  // the submachine divider is dashed
}

TEST_CASE("builder: the measurement pass reserves a name and nothing else") {
  Chart c{ small_chart() };
  scav_profile const p{ readable() };
  Metrics const m{ bundled() };
  Spaces s;
  REQUIRE(measure_chart(c, m, p, s));

  REQUIRE(s.box_state.size() == c.states.size());
  REQUIRE(s.box_sub.size() == c.submachines.size());
  REQUIRE(s.path_clear.size() == c.transitions.size());

  scav_extent title{};
  REQUIRE(measure_block(m,
                        reinterpret_cast<scav_byte const *>("Running"),
                        7,
                        p.font_size_grid,
                        p.line_height_k_num,
                        p.line_height_k_den,
                        title) == MeasureStatus::Ok);
  // The whole policy: the title plus a pad each side, the title's height plus
  // one pad above the submachine area, and nothing after it.
  CHECK(s.box_state[0].min_w == (title.w + (2 * p.pad)));
  CHECK(s.box_state[0].h_before == (title.h + p.pad));
  CHECK(s.box_state[0].h_after == 0);

  // One path box, for the one labelled transition.
  REQUIRE(s.path_box.size() == 1);
  CHECK(s.path_box[0].subject == 0);
  CHECK(s.path_box[0].order == 0);
  REQUIRE(s.label.size() == 1);
  CHECK(chart_string(c, s.label[0]) == "work arrived");
  // Every transition still gets arrowhead room at its destination.
  CHECK(s.path_clear[0].dst > 0);
  CHECK(s.path_clear[0].src == 0);
  CHECK(s.path_clear[1].dst == s.path_clear[0].dst);
}

TEST_CASE("builder: what the measurement pass asks for is what layout accepts") {
  Chart c{ small_chart() };
  scav_profile const p{ readable() };
  Spaces s;
  REQUIRE(measure_chart(c, bundled(), p, s));
  std::vector<Diagnostic> diags;
  CHECK(spaces_validate(c, as_spaces(s), diags));
  CHECK(diags.empty());
}

TEST_CASE("builder: an unnamed state and a tombstone request nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const named{ build_state(c, root, "Named", StateKind::Normal, {}) };
  build_state(c, root, {}, StateKind::Initial, {});  // a pseudostate has no name
  StateId const dead{ build_state(c, root, "Dead", StateKind::Normal, {}) };
  c.states[dead.v].live = 0;

  Spaces s;
  REQUIRE(measure_chart(c, bundled(), readable(), s));
  CHECK(s.box_state[named.v].min_w > 0);
  CHECK(s.box_state[1].min_w == 0);
  CHECK(s.box_state[1].h_before == 0);
  CHECK(s.box_state[dead.v].min_w == 0);
  CHECK(s.path_clear.empty());
}

TEST_CASE("builder: a request past the domain is refused, never clamped") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  // A name long enough that its measured width leaves the quarter-domain.
  build_state(c, root, std::string(40000, 'W'), StateKind::Normal, {});
  Spaces s;
  CHECK(!measure_chart(c, bundled(), readable(), s));

  // And a profile whose line-height ratio is out of range fails before any
  // measuring happens at all.
  scav_profile bad{ readable() };
  bad.line_height_k_den = 0;
  Chart tiny;
  build_chart(tiny, "t", {});
  CHECK(!measure_chart(tiny, bundled(), bad, s));
}

TEST_CASE("builder: the whole pipeline draws a box, a name, a route and a label") {
  Built const b{ pipeline(small_chart(), readable()) };
  // Three normal states, each an rrect.
  CHECK(kind_count(b.list, SCAV_PRIM_RRECT) == 3);
  CHECK(kind_count(b.list, SCAV_PRIM_POLYLINE) == 2);  // one per transition
  CHECK(kind_count(b.list, SCAV_PRIM_PATH) == 2);      // and one arrowhead each
  CHECK(has_text(b.list, "Running"));
  CHECK(has_text(b.list, "Idle"));
  CHECK(has_text(b.list, "work arrived"));

  // Every primitive names the entity it came from, so a backend can synthesize
  // a class for it.
  for (scav_prim const &p : b.list.prims) {
    CHECK(p.origin_kind != static_cast<uint32_t>(ElemKind::None));
  }
}

TEST_CASE("builder: a name lands inside the rect its own h_before reserved") {
  Built const b{ pipeline(small_chart(), readable()) };
  ColumnId const id{ column_find(b.chart, "scav.geom.state_before") };
  REQUIRE(id.v != INVALID);
  std::vector<scav_rect> befores(column_count(b.chart, id));
  std::memcpy(befores.data(),
              column_data(b.chart, id),
              befores.size() * sizeof(scav_rect));

  for (scav_prim const &p : b.list.prims) {
    if (p.kind != SCAV_PRIM_TEXT) { continue; }
    if (p.origin_kind != static_cast<uint32_t>(ElemKind::State)) { continue; }
    scav_rect const r{ befores[p.origin_ordinal] };
    scav_point const at{ b.list.points[p.points.off] };
    CAPTURE(p.origin_ordinal);
    CHECK(at.x >= r.x);
    CHECK(at.x <= (r.x + r.w));
    // The baseline sits one em below the top, so it is inside the band it was
    // given as long as that band is at least a line tall.
    CHECK(at.y > r.y);
  }
}

TEST_CASE("builder: a label lands on the route its own path box was placed on") {
  Chart c{ small_chart() };
  scav_profile const p{ readable() };
  Metrics const m{ bundled() };
  Spaces s;
  REQUIRE(measure_chart(c, m, p, s));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  REQUIRE(layout_run(c, as_spaces(s), opts(p), placed, diags));
  REQUIRE(placed.size() == 1);

  // The rect layout placed, read back rather than recomputed, which is what
  // keeps the drawn label inside the box that was reserved for it.
  scav_rect box{};
  REQUIRE(label_box(c,
                    as_spaces(s),
                    placed.data(),
                    static_cast<uint32_t>(placed.size()),
                    s.path_box[0].subject,
                    box));
  CHECK(box.x == placed[0].x);
  CHECK(box.w == placed[0].w);

  DrawList d;
  emit_label(d, c, m, palette_standard(), s.path_box[0].subject, box, 0);
  REQUIRE(d.prims.size() == 1);
  scav_point const at{ d.points[d.prims[0].points.off] };
  CHECK(at.x >= box.x);
  CHECK(at.x <= (box.x + box.w));
}

TEST_CASE("builder: each pseudostate kind draws as its own shape") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  constexpr std::array<StateKind, 8> KINDS{ StateKind::Initial, StateKind::Final,
                                            StateKind::Choice,  StateKind::Junction,
                                            StateKind::Fork,    StateKind::Join,
                                            StateKind::History, StateKind::DeepHistory };
  for (StateKind const k : KINDS) { build_state(c, root, {}, k, {}); }
  Built const b{ pipeline(std::move(c), readable()) };

  CHECK(kind_count(b.list, SCAV_PRIM_RRECT) == 0);  // no normal state here
  CHECK(kind_count(b.list, SCAV_PRIM_RECT) == 2);   // fork and join are bars
  CHECK(kind_count(b.list, SCAV_PRIM_PATH) == 1);   // choice is a diamond
  // Initial, junction, history and deep history are one circle each; final and
  // the two history states carry a second circle or a glyph.
  CHECK(kind_count(b.list, SCAV_PRIM_CIRCLE) == 6);
  CHECK(has_text(b.list, "H"));
  CHECK(has_text(b.list, "H*"));
}

TEST_CASE("builder: only a sibling submachine draws a divider") {
  // Every submachine gets a child: an empty one sizes to nothing, and a rect of
  // no height has no border to draw a divider on.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const lone{ build_state(c, root, "Lone", StateKind::Normal, {}) };
  build_state(c, build_submachine(c, lone, "only", {}), "A", StateKind::Normal, {});
  StateId const both{ build_state(c, root, "Both", StateKind::Normal, {}) };
  build_state(c, build_submachine(c, both, "main", {}), "B", StateKind::Normal, {});
  build_state(c, build_submachine(c, both, "aux", {}), "C", StateKind::Normal, {});

  Built const b{ pipeline(std::move(c), readable()) };
  // Three submachines, one of which is a second sibling: one divider.
  CHECK(kind_count(b.list, SCAV_PRIM_LINE) == 1);
  for (scav_prim const &p : b.list.prims) {
    if (p.kind != SCAV_PRIM_LINE) { continue; }
    CHECK(p.origin_kind == static_cast<uint32_t>(ElemKind::Submachine));
  }

  // Where it is, not just that it exists: the rule used to be drawn along one
  // region's top edge however the packer had placed the two.
  ColumnId const id{ column_find(b.chart, "scav.geom.sub") };
  REQUIRE(id.v != INVALID);
  Span const kids{ b.chart.states[both.v].submachines };
  REQUIRE(kids.len == 2);
  auto const sub_rect = [&](uint32_t k) {
    scav_rect out{};
    std::memcpy(&out,
                column_data(b.chart, id) +
                    (size_t{ b.chart.submachine_ids[kids.off + k].v } * sizeof(scav_rect)),
                sizeof(scav_rect));
    return out;
  };
  scav_rect const first{ sub_rect(0) };
  scav_rect const second{ sub_rect(1) };

  scav_prim const *line{ nullptr };
  for (scav_prim const &p : b.list.prims) {
    if (p.kind == SCAV_PRIM_LINE) { line = &p; }
  }
  REQUIRE(line != nullptr);
  REQUIRE(line->points.len == 2);
  scav_point const a{ b.list.points[line->points.off] };
  scav_point const z{ b.list.points[line->points.off + 1] };

  bool const side_by_side{ ((first.x + first.w) <= second.x) ||
                           ((second.x + second.w) <= first.x) };
  if (side_by_side) {
    // A vertical rule, strictly between the two, spanning both.
    CHECK(a.x == z.x);
    CHECK(a.x > std::min(first.x + first.w, second.x + second.w));
    CHECK(a.x < std::max(first.x, second.x));
    CHECK(std::min(a.y, z.y) <= std::min(first.y, second.y));
    CHECK(std::max(a.y, z.y) >= std::max(first.y + first.h, second.y + second.h));
  } else {
    CHECK(a.y == z.y);
    CHECK(a.y > std::min(first.y + first.h, second.y + second.h));
    CHECK(a.y < std::max(first.y, second.y));
    CHECK(std::min(a.x, z.x) <= std::min(first.x, second.x));
    CHECK(std::max(a.x, z.x) >= std::max(first.x + first.w, second.x + second.w));
  }
}

TEST_CASE("builder: a bare pseudostate's glyph fills its box exactly") {
  // A route attaches to the box border and the glyph is what is seen, so the two
  // must be the same rectangle. Layout gives a bare state no ring (11.4).
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const dot{ build_state(c, root, "", StateKind::Initial, {}) };
  StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
  build_trans(c, dot, to, TransKind::External, {});

  Built const b{ pipeline(std::move(c), readable()) };
  ColumnId const boxes{ column_find(b.chart, "scav.geom.state") };
  REQUIRE(boxes.v != INVALID);
  scav_rect box{};
  std::memcpy(&box,
              column_data(b.chart, boxes) + (size_t{ dot.v } * sizeof(scav_rect)),
              sizeof(scav_rect));

  scav_prim const *circle{ nullptr };
  for (scav_prim const &prim : b.list.prims) {
    if ((prim.kind == SCAV_PRIM_CIRCLE) && (prim.origin_ordinal == dot.v)) {
      circle = &prim;
    }
  }
  REQUIRE(circle != nullptr);
  scav_point const centre{ b.list.points[circle->points.off] };
  CHECK(circle->a == (std::min(box.w, box.h) / 2));
  CHECK(centre.x == (box.x + (box.w / 2)));
  CHECK(centre.y == (box.y + (box.h / 2)));
  // The mark spans the whole box on its narrow axis, so the border it is
  // reached at is the mark's own edge.
  CHECK((centre.x - circle->a) == box.x);
  CHECK((centre.x + circle->a) == (box.x + box.w));
}

TEST_CASE("builder: a mark-drawn glyph is its profile minimum whatever it is named") {
  // Only a rounded rect and a diamond show the name they reserve for. Reserving
  // for the rest stretches a bar, a dot or an `H` to the width of a word and then
  // paints over it -- `kind_min_h` is a floor and cannot pull it back.
  scav_profile const p{ readable() };
  for (StateKind const kind : { StateKind::Junction,
                                StateKind::Fork,
                                StateKind::Join,
                                StateKind::History,
                                StateKind::DeepHistory }) {
    CAPTURE(static_cast<uint32_t>(kind));
    std::array<scav_rect, 2> box{};
    for (uint32_t which = 0; which < 2; ++which) {
      Chart c;
      SubmachineId const root{ build_chart(c, "t", {}) };
      StateId const mark{
        build_state(c, root, (which == 0) ? "V" : "AVeryLongPseudostateName", kind, {})
      };
      StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
      build_trans(c, mark, to, TransKind::External, {});

      Spaces s;
      REQUIRE(measure_chart(c, bundled(), p, s));
      CHECK(s.box_state[mark.v].min_w == 0);
      CHECK(s.box_state[mark.v].h_before == 0);

      Built const b{ pipeline(std::move(c), p) };
      ColumnId const boxes{ column_find(b.chart, "scav.geom.state") };
      REQUIRE(boxes.v != INVALID);
      std::memcpy(&box[which],
                  column_data(b.chart, boxes) + (size_t{ mark.v } * sizeof(scav_rect)),
                  sizeof(scav_rect));
      // Nothing draws the name, so nothing may be drawn for it either.
      for (scav_prim const &prim : b.list.prims) {
        if ((prim.kind == SCAV_PRIM_TEXT) &&
            (prim.origin_kind == static_cast<uint32_t>(ElemKind::State)) &&
            (prim.origin_ordinal == mark.v)) {
          std::string_view const drawn{ reinterpret_cast<char const *>(
                                            b.list.text.bytes.data() + prim.payload.off),
                                        prim.payload.len };
          CHECK(drawn != "AVeryLongPseudostateName");
        }
      }
    }
    // The name is an identifier, not a caption: a longer one may not grow the mark.
    CHECK(box[0].w == box[1].w);
    CHECK(box[0].h == box[1].h);
    CHECK(box[0].w == p.kind_min_w[static_cast<uint32_t>(kind)]);
    CHECK(box[0].h == p.kind_min_h[static_cast<uint32_t>(kind)]);
  }
}

TEST_CASE("builder: a history mark stays inside the circle it is drawn in") {
  // The box comes from `kind_min_*`, which knows nothing about `H*`, so the mark
  // is sized from the circle instead. Checked at the em box's worst corner.
  for (StateKind const kind : { StateKind::History, StateKind::DeepHistory }) {
    CAPTURE(static_cast<uint32_t>(kind));
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const h{ build_state(c, root, "Memory", kind, {}) };
    StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
    build_trans(c, h, to, TransKind::External, {});

    Built const b{ pipeline(std::move(c), readable()) };
    scav_prim const *circle{ nullptr };
    scav_prim const *text{ nullptr };
    for (scav_prim const &prim : b.list.prims) {
      if ((prim.origin_kind != static_cast<uint32_t>(ElemKind::State)) ||
          (prim.origin_ordinal != h.v)) {
        continue;
      }
      if (prim.kind == SCAV_PRIM_CIRCLE) { circle = &prim; }
      if (prim.kind == SCAV_PRIM_TEXT) { text = &prim; }
    }
    REQUIRE(circle != nullptr);
    REQUIRE(text != nullptr);
    scav_point const centre{ b.list.points[circle->points.off] };
    scav_point const at{ b.list.points[text->points.off] };
    int32_t const fs{ b.list.styles[text->style].font_size_grid };
    scav_extent ext{};
    std::string_view const mark{ reinterpret_cast<char const *>(b.list.text.bytes.data() +
                                                                text->payload.off),
                                 text->payload.len };
    REQUIRE(measure_text(bundled(),
                         reinterpret_cast<scav_byte const *>(mark.data()),
                         static_cast<uint32_t>(mark.size()),
                         fs,
                         ext) == MeasureStatus::Ok);
    // `at` is the baseline's left end, so the em box runs one font size above it.
    int64_t worst{ 0 };
    for (int32_t const x : { at.x, at.x + ext.w }) {
      for (int32_t const y : { at.y - fs, at.y }) {
        int64_t const dx{ int64_t{ x } - centre.x };
        int64_t const dy{ int64_t{ y } - centre.y };
        worst = std::max(worst, (dx * dx) + (dy * dy));
      }
    }
    CHECK(worst <= (int64_t{ circle->a } * circle->a));
  }
}

TEST_CASE("builder: a route into a pseudostate reaches the drawn mark") {
  // The property the case above exists to protect, stated end to end.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const dot{ build_state(c, root, "", StateKind::Initial, {}) };
  StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
  build_trans(c, dot, to, TransKind::External, {});

  Built const b{ pipeline(std::move(c), readable()) };
  scav_prim const *circle{ nullptr };
  scav_prim const *route{ nullptr };
  for (scav_prim const &prim : b.list.prims) {
    if ((prim.kind == SCAV_PRIM_CIRCLE) && (prim.origin_ordinal == dot.v)) {
      circle = &prim;
    }
    if (prim.kind == SCAV_PRIM_POLYLINE) { route = &prim; }
  }
  REQUIRE(circle != nullptr);
  REQUIRE(route != nullptr);
  REQUIRE(route->points.len >= 2);
  scav_point const centre{ b.list.points[circle->points.off] };
  scav_point const start{ b.list.points[route->points.off] };
  // On the circle: one coordinate matches the centre and the other is exactly
  // a radius away, which is where an axis-aligned route meets a disc.
  bool const touches{ ((start.y == centre.y) &&
                       (std::max(start.x - centre.x, centre.x - start.x) == circle->a)) ||
                      ((start.x == centre.x) &&
                       (std::max(start.y - centre.y, centre.y - start.y) == circle->a)) };
  CHECK(touches);
}

TEST_CASE("builder: a choice's name fits inside the diamond, not across it") {
  // A diamond holds a centred label only where `w/2a + h/2b <= 1`; sizing the box
  // to the text alone puts the name through the diamond's point.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const pick{ build_state(c, root, "SelfCheck", StateKind::Choice, {}) };
  StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
  build_trans(c, pick, to, TransKind::External, {});

  Built const b{ pipeline(std::move(c), readable()) };
  scav_prim const *diamond{ nullptr };
  scav_prim const *label{ nullptr };
  for (scav_prim const &prim : b.list.prims) {
    // An arrowhead is a path too, and the first transition shares ordinal 0
    // with the first state, so the kind has to be checked as well.
    bool const is_state{ prim.origin_kind == static_cast<uint32_t>(ElemKind::State) };
    if (is_state && (prim.kind == SCAV_PRIM_PATH) && (prim.origin_ordinal == pick.v)) {
      diamond = &prim;
    }
    if (is_state && (prim.kind == SCAV_PRIM_TEXT) && (prim.origin_ordinal == pick.v)) {
      label = &prim;
    }
  }
  REQUIRE(diamond != nullptr);
  REQUIRE(label != nullptr);
  REQUIRE(diamond->points.len == 4);

  // The diamond's own centre and half-extents, read off its four vertices.
  int32_t left{ b.list.points[diamond->points.off].x };
  int32_t right{ left };
  int32_t top{ b.list.points[diamond->points.off].y };
  int32_t bottom{ top };
  for (uint32_t k = 0; k < 4; ++k) {
    scav_point const at{ b.list.points[diamond->points.off + k] };
    left = std::min(left, at.x);
    right = std::max(right, at.x);
    top = std::min(top, at.y);
    bottom = std::max(bottom, at.y);
  }
  int32_t const cx{ (left + right) / 2 };
  int32_t const cy{ (top + bottom) / 2 };
  int32_t const a{ (right - left) / 2 };
  int32_t const bb{ (bottom - top) / 2 };
  REQUIRE(a > 0);
  REQUIRE(bb > 0);

  scav_extent ext{};
  REQUIRE(measure_text(bundled(),
                       reinterpret_cast<scav_byte const *>("SelfCheck"),
                       9,
                       palette_standard()[SCAV_STYLE_TITLE].font_size_grid,
                       ext) == MeasureStatus::Ok);
  scav_point const origin{ b.list.points[label->points.off] };
  // The baseline sits one em below the block's top, so the drawn box runs from
  // there back up by the measured height.
  int32_t const x0{ origin.x };
  int32_t const x1{ origin.x + ext.w };
  int32_t const y1{ origin.y };
  int32_t const y0{ origin.y - ext.h };
  for (int32_t const x : { x0, x1 }) {
    for (int32_t const y : { y0, y1 }) {
      CAPTURE(x);
      CAPTURE(y);
      int64_t const dx{ (x > cx) ? (x - cx) : (cx - x) };
      int64_t const dy{ (y > cy) ? (y - cy) : (cy - y) };
      // `dx/a + dy/b <= 1`, cross-multiplied so it stays integer.
      CHECK(((dx * bb) + (dy * a)) <= (int64_t{ a } * bb));
    }
  }
}

TEST_CASE("builder: an arrowhead points at the border, not at the trimmed end") {
  // `PathClear` shortens the polyline so the head has room; the tip still belongs
  // on the box.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const z{ build_state(c, root, "Z", StateKind::Normal, {}) };
  build_trans(c, a, z, TransKind::External, {});
  // A second, shorter hop so the clear gets capped at half its leg: extending by
  // the requested clear rather than the one taken overshoots the border.
  StateId const y{ build_state(c, root, "Y", StateKind::Normal, {}) };
  build_trans(c, z, y, TransKind::External, {});

  Built const b{ pipeline(std::move(c), readable()) };
  ColumnId const boxes{ column_find(b.chart, "scav.geom.state") };
  REQUIRE(boxes.v != INVALID);
  scav_rect target{};
  std::memcpy(&target,
              column_data(b.chart, boxes) + (size_t{ z.v } * sizeof(scav_rect)),
              sizeof(scav_rect));

  scav_rect other{};
  std::memcpy(&other,
              column_data(b.chart, boxes) + (size_t{ y.v } * sizeof(scav_rect)),
              sizeof(scav_rect));

  auto const on_border = [](scav_point tip, scav_rect const &r) {
    return (((tip.x == r.x) || (tip.x == (r.x + r.w))) && (tip.y >= r.y) &&
            (tip.y <= (r.y + r.h))) ||
           (((tip.y == r.y) || (tip.y == (r.y + r.h))) && (tip.x >= r.x) &&
            (tip.x <= (r.x + r.w)));
  };
  uint32_t heads{ 0 };
  for (scav_prim const &prim : b.list.prims) {
    if (prim.kind != SCAV_PRIM_PATH) { continue; }
    REQUIRE(prim.points.len >= 1);
    scav_point const tip{ b.list.points[prim.points.off] };
    CAPTURE(tip.x);
    CAPTURE(tip.y);
    CHECK((on_border(tip, target) || on_border(tip, other)));
    ++heads;
  }
  CHECK(heads == 2);
}

TEST_CASE("builder: an emitter given a row it cannot draw emits nothing") {
  Built b{ pipeline(small_chart(), readable()) };
  Metrics const m{ bundled() };
  Palette const p{ palette_standard() };
  DrawList d;

  emit_state(d, b.chart, m, p, 9999, 0);  // past the array
  emit_submachine(d, b.chart, p, 9999, 0);
  emit_route(d, {}, b.chart, p, 9999, 0);
  emit_label(d, b.chart, m, p, 9999, { .x = 0, .y = 0, .w = 10, .h = 10 }, 0);
  CHECK(d.prims.empty());

  // A short palette is refused rather than read past.
  Palette const stub(1);
  emit_state(d, b.chart, m, stub, 0, 0);
  CHECK(d.prims.empty());

  // And a tombstone draws nothing, whatever geometry it still holds.
  b.chart.states[0].live = 0;
  emit_state(d, b.chart, m, p, 0, 0);
  CHECK(d.prims.empty());
}

TEST_CASE("builder: a chart layout never ran on is refused") {
  Chart c{ small_chart() };
  DrawList d;
  CHECK(!emit_chart(d, c, bundled(), palette_standard(), {}, nullptr, 0, 0));
  CHECK(d.prims.empty());
}

TEST_CASE("builder: depth is the caller's, and every primitive gets the one given") {
  Chart c{ small_chart() };
  scav_profile const p{ readable() };
  Metrics const m{ bundled() };
  Spaces s;
  REQUIRE(measure_chart(c, m, p, s));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  REQUIRE(layout_run(c, as_spaces(s), opts(p), placed, diags));

  DrawList d;
  REQUIRE(emit_chart(d,
                     c,
                     m,
                     palette_standard(),
                     as_spaces(s),
                     placed.data(),
                     static_cast<uint32_t>(placed.size()),
                     42));
  for (scav_prim const &prim : d.prims) { CHECK(prim.depth == 42); }

  // Which is what lets an app interleave: the emitters take their own numbers,
  // so a second pass lands above or below the first with no splicing.
  DrawList interleaved;
  emit_state(interleaved, c, m, palette_standard(), 0, 10);
  emit_route(interleaved, {}, c, palette_standard(), 0, -5);
  REQUIRE(interleaved.prims.size() >= 2);
  CHECK(interleaved.prims.front().depth == 10);
  CHECK(interleaved.prims.back().depth == -5);
}

TEST_CASE("builder: the same chart built twice is the same drawlist") {
  Built const one{ pipeline(small_chart(), readable()) };
  Built const two{ pipeline(small_chart(), readable()) };
  Metrics const m{ bundled() };
  DrawList a{ one.list };
  DrawList b{ two.list };
  drawlist_canonicalize(a);
  drawlist_canonicalize(b);
  CHECK(drawlist_digest(a, m) == drawlist_digest(b, m));
}

TEST_CASE("builder: the profile's font size reaches the drawn text") {
  scav_profile small{ readable() };
  scav_profile large{ readable() };
  small.font_size_grid = 8 * 16;
  large.font_size_grid = 20 * 16;
  Metrics const m{ bundled() };

  Spaces narrow;
  Spaces wide;
  Chart a{ small_chart() };
  Chart b{ small_chart() };
  REQUIRE(measure_chart(a, m, small, narrow));
  REQUIRE(measure_chart(b, m, large, wide));
  // Bigger type asks for more room, which is the only channel by which a font
  // reaches layout at all.
  CHECK(wide.box_state[0].min_w > narrow.box_state[0].min_w);
  CHECK(wide.box_state[0].h_before > narrow.box_state[0].h_before);
}

TEST_CASE("builder: the C surface builds through the handles") {
  Chart c{ small_chart() };
  scav_profile const p{ readable() };
  Metrics const m{ bundled() };
  Spaces s;
  REQUIRE(measure_chart(c, m, p, s));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  REQUIRE(layout_run(c, as_spaces(s), opts(p), placed, diags));

  scav_chart chart{ .chart = std::move(c), .diags = {} };
  scav_metrics *metrics{ nullptr };
  scav_drawlist *list{ nullptr };
  REQUIRE(scav_metrics_create(nullptr, 0, &metrics) == SCAV_OK);
  REQUIRE(scav_drawlist_create(&list) == SCAV_OK);

  std::vector<scav_style> palette(SCAV_STYLE_COUNT);
  REQUIRE(scav_palette_standard(palette.data(), SCAV_STYLE_COUNT) == SCAV_OK);
  CHECK(scav_palette_standard(palette.data(), 1) == SCAV_E_CAPACITY);

  REQUIRE(scav_emit_chart(list,
                          &chart,
                          metrics,
                          palette.data(),
                          SCAV_STYLE_COUNT,
                          nullptr,
                          nullptr,
                          0,
                          0) == SCAV_OK);
  uint32_t prims{ 0 };
  REQUIRE(scav_drawlist_counts(list, &prims, nullptr, nullptr, nullptr, nullptr) ==
          SCAV_OK);
  CHECK(prims > 0);

  // A null palette takes the shipped one; a short one is refused.
  scav_drawlist *defaulted{ nullptr };
  REQUIRE(scav_drawlist_create(&defaulted) == SCAV_OK);
  REQUIRE(
      scav_emit_chart(defaulted, &chart, metrics, nullptr, 0, nullptr, nullptr, 0, 0) ==
      SCAV_OK);
  CHECK(
      scav_emit_chart(list, &chart, metrics, palette.data(), 1, nullptr, nullptr, 0, 0) ==
      SCAV_E_INVALID_ARG);
  CHECK(scav_emit_chart(nullptr, &chart, metrics, nullptr, 0, nullptr, nullptr, 0, 0) ==
        SCAV_E_INVALID_ARG);

  // A chart with no geometry is a state error, not a bad argument: the caller
  // did nothing wrong except skip layout.
  scav_chart unlaid{ .chart = small_chart(), .diags = {} };
  CHECK(scav_emit_chart(list, &unlaid, metrics, nullptr, 0, nullptr, nullptr, 0, 0) ==
        SCAV_E_STATE);

  scav_drawlist_destroy(defaulted);
  scav_drawlist_destroy(list);
  scav_metrics_destroy(metrics);
}
