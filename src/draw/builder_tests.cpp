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
}

TEST_CASE("builder: an emitter given a row it cannot draw emits nothing") {
  Built b{ pipeline(small_chart(), readable()) };
  Metrics const m{ bundled() };
  Palette const p{ palette_standard() };
  DrawList d;

  emit_state(d, b.chart, m, p, 9999, 0);  // past the array
  emit_submachine(d, b.chart, p, 9999, 0);
  emit_route(d, b.chart, p, 9999, 0);
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
  emit_route(interleaved, c, palette_standard(), 0, -5);
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
