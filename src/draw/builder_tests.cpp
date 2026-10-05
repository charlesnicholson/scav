// Tests for the measurement pass and the emitters that draw layout's geometry.

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
// Declares `operator<<` for `string_view`, used when doctest prints CHECK operands.
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
  build_trans(c, idle, busy, TransKind::Default, "work arrived");
  build_trans(c, busy, idle, TransKind::Default, {});  // no label, so no box
  return c;
}

// A chart after measure, layout and emit, with each stage's output.
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

constexpr uint32_t RECT_SIZE{ static_cast<uint32_t>(sizeof(scav_rect)) };

// The sizes the C surface checks against, as this build measures them.
constexpr uint32_t STYLE_SIZE{ static_cast<uint32_t>(sizeof(scav_style)) };
constexpr uint32_t SPACES_SIZE{ static_cast<uint32_t>(sizeof(scav_spaces)) };
constexpr uint32_t PLACED_SIZE{ static_cast<uint32_t>(sizeof(scav_placed)) };

// A plane-15 private-use codepoint with no bundled-font glyph; measuring it fails.
constexpr char const *NO_GLYPH{ "\xF3\xB0\x80\x81" };

// A geometry column in layout's own shape, written by hand.
ColumnId geom_column(Chart &c,
                     char const *name,
                     ElemKind entity,
                     ValueKind kind,
                     uint32_t elem_size) {
  ColumnId const id{
    column_register(c, name, entity, kind, elem_size, 4, COLUMN_DERIVED)
  };
  REQUIRE(id.v != INVALID);
  return id;
}

template <typename T>
void put_row(Chart &c, ColumnId id, uint32_t row, T const &value) {
  REQUIRE(column_count(c, id) > row);
  std::memcpy(column_data(c, id) + (static_cast<size_t>(row) * sizeof(T)),
              &value,
              sizeof(T));
}

ColumnId state_boxes(Chart &c) {
  return geom_column(c, "scav.geom.state", ElemKind::State, ValueKind::Pod, RECT_SIZE);
}

// Measurable text past the quarter-domain on x (`wide_text`) or on y (`tall_text`).
std::string wide_text() {
  std::string s;
  s.append(2000, 'W');
  return s;
}

std::string tall_text() {
  std::string s;
  for (uint32_t i = 0; i < 600; ++i) { s += "W\n"; }
  return s;
}

scav_rect geom_rect(Chart const &c, char const *name, uint32_t row) {
  ColumnId const id{ column_find(c, name) };
  REQUIRE(id.v != INVALID);
  REQUIRE(column_count(c, id) > row);
  scav_rect r{};
  std::memcpy(&r,
              column_data(c, id) + (size_t{ row } * sizeof(scav_rect)),
              sizeof(scav_rect));
  return r;
}

std::vector<scav_prim> state_prims(DrawList const &d, uint32_t state, uint32_t kind) {
  std::vector<scav_prim> out;
  for (scav_prim const &p : d.prims) {
    if ((p.kind == kind) && (p.origin_kind == static_cast<uint32_t>(ElemKind::State)) &&
        (p.origin_ordinal == state)) {
      out.push_back(p);
    }
  }
  return out;
}

int32_t title_width(std::string_view text) {
  scav_extent e{};
  REQUIRE(measure_text(bundled(),
                       reinterpret_cast<scav_byte const *>(text.data()),
                       static_cast<uint32_t>(text.size()),
                       palette_standard()[SCAV_STYLE_TITLE].font_size_grid,
                       e) == MeasureStatus::Ok);
  return e.w;
}

scav_extent measured(std::string_view text, scav_profile const &p) {
  scav_extent e{};
  REQUIRE(measure_block(bundled(),
                        reinterpret_cast<scav_byte const *>(text.data()),
                        static_cast<uint32_t>(text.size()),
                        p.font_size_grid,
                        p.line_height_k_num,
                        p.line_height_k_den,
                        e) == MeasureStatus::Ok);
  return e;
}

}  // namespace

TEST_CASE("builder: the standard palette fills every slot the builder indexes") {
  Palette const p{ palette_standard() };
  REQUIRE(p.size() == SCAV_STYLE_COUNT);
  // Text styles have a nonzero font size; shape styles have zero.
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
  // The whole policy: the title plus a pad each side, the title's height plus half a pad
  // down to the rule that ends the band, and nothing after it.
  CHECK(s.box_state[0].min_w == (title.w + (2 * p.pad)));
  CHECK(s.box_state[0].h_before == (title.h + (p.pad / 2)));
  CHECK(s.box_state[0].ruled == 1U);
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

TEST_CASE("builder: only a rounded rect and a diamond reserve room for a name") {
  scav_profile const p{ readable() };
  Metrics const m{ bundled() };
  scav_extent title{};
  REQUIRE(measure_block(m,
                        reinterpret_cast<scav_byte const *>("Named"),
                        5,
                        p.font_size_grid,
                        p.line_height_k_num,
                        p.line_height_k_den,
                        title) == MeasureStatus::Ok);

  constexpr std::array<StateKind, 9> KINDS{ StateKind::Normal,     StateKind::Initial,
                                            StateKind::Final,      StateKind::Choice,
                                            StateKind::Junction,   StateKind::Fork,
                                            StateKind::Join,       StateKind::History,
                                            StateKind::DeepHistory };
  for (StateKind const kind : KINDS) {
    CAPTURE(static_cast<uint32_t>(kind));
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const s{ build_state(c, root, "Named", kind, {}) };
    Spaces sp;
    REQUIRE(measure_chart(c, m, p, sp));
    // A choice reserves twice a rectangle's width and height for its inscribed name;
    // other non-normal kinds reserve nothing.
    int32_t const grow{ (kind == StateKind::Choice) ? 2 : 0 };
    int32_t const rect{ (kind == StateKind::Normal) ? 1 : grow };
    CHECK(sp.box_state[s.v].min_w == (rect * (title.w + (2 * p.pad))));
    CHECK(sp.box_state[s.v].h_before == (rect * (title.h + p.pad)));
    CHECK(sp.box_state[s.v].h_after == 0);
  }
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
    // The baseline sits one em below the band's top.
    CHECK(at.y > r.y);
  }
}

TEST_CASE("builder: a description reserves its lines under the name, and its width") {
  scav_profile const p{ readable() };
  Metrics const m{ bundled() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const bare{ build_state(c, root, "Standby", StateKind::Normal, {}) };
  StateId const wide{ build_state(c, root, "Warm", StateKind::Normal, "warm and idle") };
  StateId const slim{ build_state(c, root, "Standby", StateKind::Normal, "hot") };
  StateId const pick{
    build_state(c, root, "Standby", StateKind::Choice, "warm and idle")
  };
  StateId const mark{ build_state(c, root, "Done", StateKind::Final, "warm and idle") };
  Spaces s;
  REQUIRE(measure_chart(c, m, p, s));

  scav_extent const standby{ measured("Standby", p) };
  scav_extent const warm{ measured("Warm", p) };
  scav_extent const about{ measured("warm and idle", p) };
  REQUIRE(about.w > standby.w);
  CHECK(s.box_state[bare.v].min_w == (standby.w + (2 * p.pad)));
  CHECK(s.box_state[bare.v].h_before == (standby.h + p.pad));

  // The name, the pad its rule splits, then the description's own lines.
  CHECK(s.box_state[wide.v].h_before == (warm.h + p.pad + about.h));
  CHECK(s.box_state[wide.v].min_w == (about.w + (2 * p.pad)));
  CHECK(s.box_state[wide.v].h_before > s.box_state[bare.v].h_before);
  CHECK(s.box_state[wide.v].min_w > s.box_state[bare.v].min_w);

  CHECK(s.box_state[slim.v].min_w == s.box_state[bare.v].min_w);
  CHECK(s.box_state[slim.v].h_before == (standby.h + p.pad + measured("hot", p).h));

  CHECK(s.box_state[pick.v].min_w == (2 * (standby.w + (2 * p.pad))));
  CHECK(s.box_state[pick.v].h_before == (2 * (standby.h + p.pad)));
  CHECK(s.box_state[mark.v].h_before == 0);
  CHECK(s.box_state[mark.v].min_w == 0);
}

TEST_CASE("builder: a description is drawn under a rule spanning the box") {
  scav_profile const p{ readable() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const st{ build_state(c, root, "Standby", StateKind::Normal, "warm and idle") };
  Built const b{ pipeline(std::move(c), p) };
  scav_rect const box{ geom_rect(b.chart, "scav.geom.state", st.v) };
  scav_rect const before{ geom_rect(b.chart, "scav.geom.state_before", st.v) };
  int32_t const fs{ palette_standard()[SCAV_STYLE_TITLE].font_size_grid };

  std::vector<scav_prim> const rules{ state_prims(b.list, st.v, SCAV_PRIM_LINE) };
  REQUIRE(rules.size() == 1);
  scav_point const a{ b.list.points[rules[0].points.off] };
  scav_point const z{ b.list.points[rules[0].points.off + 1] };
  CHECK(a.y == z.y);
  CHECK(a.x == box.x);  // border to border
  CHECK(z.x == (box.x + box.w));
  // Where measure_chart put the end of the name block and half its pad.
  CHECK(a.y == (before.y + measured("Standby", p).h + (p.pad / 2)));
  CHECK(a.y > before.y);
  CHECK(a.y < (before.y + before.h));

  std::vector<scav_prim> const texts{ state_prims(b.list, st.v, SCAV_PRIM_TEXT) };
  REQUIRE(texts.size() == 2);
  REQUIRE(payload(b.list, texts[0]) == "Standby");
  REQUIRE(payload(b.list, texts[1]) == "warm and idle");
  scav_point const name{ b.list.points[texts[0].points.off] };
  scav_point const note{ b.list.points[texts[1].points.off] };

  CHECK(name.x == (before.x + ((before.w - title_width("Standby")) / 2)));
  CHECK(name.y < a.y);

  CHECK(note.x == (before.x + p.pad));
  CHECK((note.x + title_width("warm and idle")) <= (before.x + before.w));
  CHECK((note.y - fs) > a.y);
  CHECK(note.y <= (before.y + before.h));
}

TEST_CASE("builder: a composite with no description still takes a header rule") {
  scav_profile const p{ readable() };
  Built const b{ pipeline(small_chart(), p) };
  scav_rect const box{ geom_rect(b.chart, "scav.geom.state", 0) };
  scav_rect const before{ geom_rect(b.chart, "scav.geom.state_before", 0) };
  scav_rect const region{ geom_rect(b.chart, "scav.geom.sub", 1) };

  std::vector<scav_prim> const rules{ state_prims(b.list, 0, SCAV_PRIM_LINE) };
  REQUIRE(rules.size() == 1);
  scav_point const a{ b.list.points[rules[0].points.off] };
  scav_point const z{ b.list.points[rules[0].points.off + 1] };
  CHECK(a.y == z.y);
  CHECK(a.x == box.x);
  CHECK(z.x == (box.x + box.w));
  CHECK(a.y == (before.y + measured("Running", p).h + (p.pad / 2)));
  CHECK(a.y == (before.y + before.h));  // the band's inner edge
  CHECK(a.y <= region.y);
  REQUIRE(state_prims(b.list, 0, SCAV_PRIM_TEXT).size() == 1);
}

TEST_CASE("builder: a plain leaf, or a box whose regions are all dead, takes no rule") {
  Built b{ pipeline(small_chart(), readable()) };
  for (uint32_t i = 1; i < b.chart.states.size(); ++i) {
    CAPTURE(i);
    CHECK(state_prims(b.list, i, SCAV_PRIM_LINE).empty());
  }

  b.chart.submachines[1].live = 0;
  DrawList d;
  emit_state(d, b.chart, bundled(), palette_standard(), 0, 0);
  CHECK(kind_count(d, SCAV_PRIM_RRECT) == 1);
  CHECK(kind_count(d, SCAV_PRIM_LINE) == 0);
  CHECK(has_text(d, "Running"));
}

TEST_CASE("builder: a description of several lines stacks them inside the band") {
  scav_profile const p{ readable() };
  std::string_view const about{ "warm\nand idle\nand ready" };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const st{ build_state(c, root, "Standby", StateKind::Normal, about) };
  Spaces s;
  REQUIRE(measure_chart(c, bundled(), p, s));
  scav_extent const name{ measured("Standby", p) };
  scav_extent const text{ measured(about, p) };
  CHECK(text.h == (3 * measured("warm", p).h));
  CHECK(s.box_state[st.v].h_before == (name.h + p.pad + text.h));
  CHECK(s.box_state[st.v].min_w == (std::max(name.w, text.w) + (2 * p.pad)));

  Built const b{ pipeline(std::move(c), p) };
  scav_rect const before{ geom_rect(b.chart, "scav.geom.state_before", st.v) };
  std::vector<scav_prim> const rules{ state_prims(b.list, st.v, SCAV_PRIM_LINE) };
  REQUIRE(rules.size() == 1);
  int32_t const rule{ b.list.points[rules[0].points.off].y };
  CHECK(rule == (before.y + name.h + (p.pad / 2)));

  std::vector<scav_prim> const texts{ state_prims(b.list, st.v, SCAV_PRIM_TEXT) };
  REQUIRE(texts.size() == 4);
  std::array<std::string_view, 3> const want{ { "warm", "and idle", "and ready" } };
  int32_t const fs{ palette_standard()[SCAV_STYLE_TITLE].font_size_grid };
  int32_t previous{ rule };
  for (uint32_t k = 0; k < 3; ++k) {
    CAPTURE(k);
    scav_prim const &line{ texts[k + 1] };
    CHECK(payload(b.list, line) == want[k]);
    scav_point const at{ b.list.points[line.points.off] };
    CHECK(at.x == (before.x + p.pad));
    CHECK((at.y - fs) >= previous);  // each em box below the last line's baseline
    CHECK(at.y <= (before.y + before.h));
    previous = at.y;
  }
}

TEST_CASE("builder: a band too short for its lines draws the name and no header") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "Named", StateKind::Normal, "a note");
  ColumnId const boxes{ state_boxes(c) };
  ColumnId const befores{
    geom_column(c, "scav.geom.state_before", ElemKind::State, ValueKind::Pod, RECT_SIZE)
  };
  put_row(c, boxes, 0, scav_rect{ .x = 0, .y = 0, .w = 400, .h = 200 });
  put_row(c, befores, 0, scav_rect{ .x = 8, .y = 8, .w = 384, .h = 9 });

  DrawList d;
  emit_state(d, c, bundled(), palette_standard(), 0, 0);
  CHECK(kind_count(d, SCAV_PRIM_LINE) == 0);
  CHECK(has_text(d, "Named"));
  CHECK(!has_text(d, "a note"));
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

  // `label_box` returns the rect layout placed for the label.
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
  // One child state per submachine gives each region a nonzero height.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const lone{ build_state(c, root, "Lone", StateKind::Normal, {}) };
  build_state(c, build_submachine(c, lone, "only", {}), "A", StateKind::Normal, {});
  StateId const both{ build_state(c, root, "Both", StateKind::Normal, {}) };
  build_state(c, build_submachine(c, both, "main", {}), "B", StateKind::Normal, {});
  build_state(c, build_submachine(c, both, "aux", {}), "C", StateKind::Normal, {});

  Built const b{ pipeline(std::move(c), readable()) };
  // Three submachines, one of which is a second sibling: one divider. The other
  // lines are the two owners' header rules.
  uint32_t dividers{ 0 };
  scav_prim const *line{ nullptr };
  for (scav_prim const &p : b.list.prims) {
    if (p.kind != SCAV_PRIM_LINE) { continue; }
    if (p.origin_kind != static_cast<uint32_t>(ElemKind::Submachine)) { continue; }
    ++dividers;
    line = &p;
  }
  CHECK(dividers == 1);
  CHECK(kind_count(b.list, SCAV_PRIM_LINE) == 3);

  // Checks the divider's position: between the two regions and spanning both.
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
  // The glyph fills the box a route attaches to; layout gives a bare state no ring.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const dot{ build_state(c, root, "", StateKind::Initial, {}) };
  StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
  build_trans(c, dot, to, TransKind::Default, {});

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
  // The circle spans the box's narrower axis, so its edge lies on the box border.
  CHECK((centre.x - circle->a) == box.x);
  CHECK((centre.x + circle->a) == (box.x + box.w));
}

TEST_CASE("builder: a mark-drawn glyph is its profile minimum whatever it is named") {
  // Junction, fork, join and history marks reserve no name space and take the
  // profile's `kind_min_w` and `kind_min_h`, whatever the name.
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
      build_trans(c, mark, to, TransKind::Default, {});

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
      // The mark never draws the state's name.
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
    // A longer name leaves the mark's size unchanged.
    CHECK(box[0].w == box[1].w);
    CHECK(box[0].h == box[1].h);
    CHECK(box[0].w == p.kind_min_w[static_cast<uint32_t>(kind)]);
    CHECK(box[0].h == p.kind_min_h[static_cast<uint32_t>(kind)]);
  }
}

TEST_CASE("builder: a history mark stays inside the circle it is drawn in") {
  // The `H`/`H*` mark's em is the circle's radius. Checks the em box's farthest
  // corner against the circle.
  for (StateKind const kind : { StateKind::History, StateKind::DeepHistory }) {
    CAPTURE(static_cast<uint32_t>(kind));
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const h{ build_state(c, root, "Memory", kind, {}) };
    StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
    build_trans(c, h, to, TransKind::Default, {});

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

TEST_CASE("builder: the history mark fits its circle at every radius the profile gives") {
  for (int32_t side : { 32, 64, 128, 256, 512, 1024 }) {
    CAPTURE(side);
    scav_profile p{ readable() };
    p.kind_min_w[static_cast<uint32_t>(StateKind::DeepHistory)] = side;
    p.kind_min_h[static_cast<uint32_t>(StateKind::DeepHistory)] = side;
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const h{ build_state(c, root, {}, StateKind::DeepHistory, {}) };

    Built const b{ pipeline(std::move(c), p) };
    scav_prim const *circle{ nullptr };
    scav_prim const *text{ nullptr };
    for (scav_prim const &prim : b.list.prims) {
      if (prim.origin_ordinal != h.v) { continue; }
      if (prim.kind == SCAV_PRIM_CIRCLE) { circle = &prim; }
      if (prim.kind == SCAV_PRIM_TEXT) { text = &prim; }
    }
    REQUIRE(circle != nullptr);
    REQUIRE(text != nullptr);
    scav_point const centre{ b.list.points[circle->points.off] };
    scav_point const at{ b.list.points[text->points.off] };
    int32_t const fs{ b.list.styles[text->style].font_size_grid };
    scav_extent ext{};
    REQUIRE(
        measure_text(bundled(), reinterpret_cast<scav_byte const *>("H*"), 2, fs, ext) ==
        MeasureStatus::Ok);
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

TEST_CASE("builder: an internal loop's label is a path box seated inside its state") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const s{ build_state(c, root, "Idle", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "Busy", StateKind::Normal, {}) };
  build_trans(c, s, s, TransKind::Internal, "tick");
  build_trans(c, s, other, TransKind::Default, "go");
  build_trans(c, s, s, TransKind::Local, "tock");

  scav_profile const p{ readable() };
  Spaces sp;
  REQUIRE(measure_chart(c, bundled(), p, sp));
  // Each labelled transition gets a path box; the state's after-band stays zero.
  CHECK(sp.box_state[s.v].h_after == 0);
  REQUIRE(sp.path_box.size() == 3);

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  REQUIRE(layout_run(c, as_spaces(sp), opts(p), placed, diags));
  ColumnId const states{ column_find(c, "scav.geom.state") };
  ColumnId const befores{ column_find(c, "scav.geom.state_before") };
  REQUIRE(states.v != INVALID);
  REQUIRE(befores.v != INVALID);
  scav_rect box{};
  scav_rect title{};
  std::memcpy(&box,
              column_data(c, states) + (static_cast<size_t>(s.v) * sizeof(scav_rect)),
              sizeof(scav_rect));
  std::memcpy(&title,
              column_data(c, befores) + (static_cast<size_t>(s.v) * sizeof(scav_rect)),
              sizeof(scav_rect));

  uint32_t const count{ static_cast<uint32_t>(placed.size()) };
  scav_rect first{};
  scav_rect second{};
  REQUIRE(label_box(c, as_spaces(sp), placed.data(), count, 0, first));
  REQUIRE(label_box(c, as_spaces(sp), placed.data(), count, 2, second));
  // Inside the state and below its title band, one row per loop in transition order.
  for (scav_rect const &r : { first, second }) {
    CHECK(r.x > box.x);
    CHECK((r.x + r.w) < (box.x + box.w));
    CHECK(r.y >= (title.y + title.h));
    CHECK((r.y + r.h) < (box.y + box.h));
  }
  CHECK((first.y + first.h) <= second.y);
}

TEST_CASE("builder: a tombstoned transition asks for no path box") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const s{ build_state(c, root, "S", StateKind::Normal, {}) };
  TransId const dead{ build_trans(c, s, s, TransKind::Internal, "gone") };
  TransId const live{ build_trans(c, s, s, TransKind::Internal, "tick") };
  c.transitions[dead.v].live = 0;

  Spaces sp;
  REQUIRE(measure_chart(c, bundled(), readable(), sp));
  REQUIRE(sp.path_box.size() == 1);
  CHECK(sp.path_box[0].subject == live.v);
}

TEST_CASE("builder: a label is drawn centred in the box that was placed for it") {
  Chart c{ small_chart() };
  scav_profile const p{ readable() };
  Metrics const m{ bundled() };
  Spaces s;
  REQUIRE(measure_chart(c, m, p, s));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  REQUIRE(layout_run(c, as_spaces(s), opts(p), placed, diags));
  REQUIRE(placed.size() == 1);

  DrawList d;
  Palette const pal{ palette_standard() };
  emit_label(d, c, m, pal, s.path_box[0].subject, placed[0], 0);
  REQUIRE(d.prims.size() == 1);
  int32_t const fs{ pal[SCAV_STYLE_LABEL].font_size_grid };
  scav_extent ext{};
  REQUIRE(
      measure_text(m, reinterpret_cast<scav_byte const *>("work arrived"), 12, fs, ext) ==
      MeasureStatus::Ok);
  scav_point const at{ d.points[d.prims[0].points.off] };
  // Centred on both axes of the rect layout placed; the baseline is one em below
  // the block's top.
  CHECK(at.x == (placed[0].x + ((placed[0].w - ext.w) / 2)));
  CHECK(at.y == (placed[0].y + ((placed[0].h - line_height(fs, 1, 1)) / 2) + fs));
}

TEST_CASE("builder: a route into a pseudostate reaches the drawn mark") {
  // Checks that the route's first point lies on the pseudostate's drawn circle.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const dot{ build_state(c, root, "", StateKind::Initial, {}) };
  StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
  build_trans(c, dot, to, TransKind::Default, {});

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
  // A centred label fits the diamond when `w/2a + h/2b <= 1`.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const pick{ build_state(c, root, "SelfCheck", StateKind::Choice, {}) };
  StateId const to{ build_state(c, root, "S", StateKind::Normal, {}) };
  build_trans(c, pick, to, TransKind::Default, {});

  Built const b{ pipeline(std::move(c), readable()) };
  scav_prim const *diamond{ nullptr };
  scav_prim const *label{ nullptr };
  for (scav_prim const &prim : b.list.prims) {
    // Selects state-origin paths and text only; arrowheads are transition-origin paths.
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
  build_trans(c, a, z, TransKind::Default, {});
  // A second, shorter hop whose clear is capped at half its last leg.
  StateId const y{ build_state(c, root, "Y", StateKind::Normal, {}) };
  build_trans(c, z, y, TransKind::Default, {});

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

  // A palette shorter than `SCAV_STYLE_COUNT` emits nothing.
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

  // Each emitter call stamps the `depth` it is given on every primitive.
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
  // A larger font size requests a larger box.
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
  REQUIRE(scav_palette_standard(palette.data(), SCAV_STYLE_COUNT, STYLE_SIZE) == SCAV_OK);
  CHECK(scav_palette_standard(palette.data(), 1, STYLE_SIZE) == SCAV_E_CAPACITY);

  REQUIRE(scav_emit_chart(list,
                          &chart,
                          metrics,
                          palette.data(),
                          SCAV_STYLE_COUNT,
                          STYLE_SIZE,
                          nullptr,
                          SPACES_SIZE,
                          nullptr,
                          0,
                          PLACED_SIZE,
                          0) == SCAV_OK);
  uint32_t prims{ 0 };
  REQUIRE(scav_drawlist_counts(list, &prims, nullptr, nullptr, nullptr, nullptr) ==
          SCAV_OK);
  CHECK(prims > 0);

  // A null palette takes the shipped one; a short one is refused.
  scav_drawlist *defaulted{ nullptr };
  REQUIRE(scav_drawlist_create(&defaulted) == SCAV_OK);
  REQUIRE(scav_emit_chart(defaulted,
                          &chart,
                          metrics,
                          nullptr,
                          0,
                          STYLE_SIZE,
                          nullptr,
                          SPACES_SIZE,
                          nullptr,
                          0,
                          PLACED_SIZE,
                          0) == SCAV_OK);
  CHECK(scav_emit_chart(list,
                        &chart,
                        metrics,
                        palette.data(),
                        1,
                        STYLE_SIZE,
                        nullptr,
                        SPACES_SIZE,
                        nullptr,
                        0,
                        PLACED_SIZE,
                        0) == SCAV_E_INVALID_ARG);
  CHECK(scav_emit_chart(nullptr,
                        &chart,
                        metrics,
                        nullptr,
                        0,
                        STYLE_SIZE,
                        nullptr,
                        SPACES_SIZE,
                        nullptr,
                        0,
                        PLACED_SIZE,
                        0) == SCAV_E_INVALID_ARG);

  // A chart without layout geometry returns `SCAV_E_STATE`.
  scav_chart unlaid{ .chart = small_chart(), .diags = {} };
  CHECK(scav_emit_chart(list,
                        &unlaid,
                        metrics,
                        nullptr,
                        0,
                        STYLE_SIZE,
                        nullptr,
                        SPACES_SIZE,
                        nullptr,
                        0,
                        PLACED_SIZE,
                        0) == SCAV_E_STATE);

  scav_drawlist_destroy(defaulted);
  scav_drawlist_destroy(list);
  scav_metrics_destroy(metrics);
}

TEST_CASE("builder: an empty chart reserves nothing and hands layout no pointers") {
  Chart const c;  // no root submachine either: there is nothing to measure
  Spaces s;
  REQUIRE(measure_chart(c, bundled(), readable(), s));
  CHECK(s.box_state.empty());
  CHECK(s.box_sub.empty());
  CHECK(s.path_clear.empty());
  CHECK(s.path_box.empty());

  // A null base with a zero count, never a pointer into an empty vector.
  scav_spaces const view{ as_spaces(s) };
  CHECK(view.box_state == nullptr);
  CHECK(view.n_box_state == 0);
  CHECK(view.box_sub == nullptr);
  CHECK(view.n_box_sub == 0);
  CHECK(view.path_clear == nullptr);
  CHECK(view.n_path_clear == 0);
  CHECK(view.path_box == nullptr);
  CHECK(view.n_path_box == 0);
}

TEST_CASE("builder: a state name past the quarter-domain is refused on either axis") {
  scav_profile const p{ readable() };
  auto const refused = [&p](std::string const &name) {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    build_state(c, root, name, StateKind::Normal, {});
    Spaces s;
    return !measure_chart(c, bundled(), p, s);
  };

  // Measurable text past `SPACE_MAX` on one axis is refused.
  scav_extent const wide{ measured(wide_text(), p) };
  CHECK(wide.w > SPACE_MAX);
  CHECK(wide.h < SPACE_MAX);
  CHECK(refused(wide_text()));

  scav_extent const tall{ measured(tall_text(), p) };
  CHECK(tall.w < SPACE_MAX);
  CHECK(tall.h > SPACE_MAX);
  CHECK(refused(tall_text()));
}

TEST_CASE("builder: a submachine name is measured under the rules a state name is") {
  scav_profile const p{ readable() };
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const owner{ build_state(c, root, "S", StateKind::Normal, {}) };

  SUBCASE("a tombstone reserves nothing") {
    SubmachineId const dead{ build_submachine(c, owner, "region", {}) };
    c.submachines[dead.v].live = 0;
    Spaces s;
    REQUIRE(measure_chart(c, bundled(), p, s));
    CHECK(s.box_sub[dead.v].min_w == 0);
    CHECK(s.box_sub[dead.v].h_before == 0);
  }
  SUBCASE("a name the font cannot measure refuses the pass") {
    build_submachine(c, owner, NO_GLYPH, {});
    Spaces s;
    CHECK(!measure_chart(c, bundled(), p, s));
  }
  SUBCASE("a name too wide for the domain is refused") {
    build_submachine(c, owner, wide_text(), {});
    Spaces s;
    CHECK(!measure_chart(c, bundled(), p, s));
  }
  SUBCASE("a name too tall for the domain is refused") {
    build_submachine(c, owner, tall_text(), {});
    Spaces s;
    CHECK(!measure_chart(c, bundled(), p, s));
  }
}

TEST_CASE("builder: a description is measured under the rules a state name is") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  Spaces s;
  SUBCASE("a description the font cannot measure refuses the pass") {
    build_state(c, root, "S", StateKind::Normal, NO_GLYPH);
    CHECK(!measure_chart(c, bundled(), readable(), s));
  }
  SUBCASE("a description too wide for the domain is refused") {
    build_state(c, root, "S", StateKind::Normal, wide_text());
    CHECK(!measure_chart(c, bundled(), readable(), s));
  }
  SUBCASE("a description too tall for the domain is refused") {
    build_state(c, root, "S", StateKind::Normal, tall_text());
    CHECK(!measure_chart(c, bundled(), readable(), s));
  }
  SUBCASE("a diamond never measures one") {
    build_state(c, root, "S", StateKind::Choice, NO_GLYPH);
    CHECK(measure_chart(c, bundled(), readable(), s));
  }
}

TEST_CASE("builder: a tombstoned transition reserves neither arrowhead room nor a box") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  TransId const dead{ build_trans(c, a, b, TransKind::Default, "gone") };
  TransId const kept{ build_trans(c, b, a, TransKind::Default, "kept") };
  c.transitions[dead.v].live = 0;

  Spaces s;
  REQUIRE(measure_chart(c, bundled(), readable(), s));
  CHECK(s.path_clear[dead.v].dst == 0);  // no head to leave room for
  CHECK(s.path_clear[kept.v].dst > 0);
  REQUIRE(s.path_box.size() == 1);  // and no box for a label nothing will draw
  CHECK(s.path_box[0].subject == kept.v);
  REQUIRE(s.label.size() == 1);
  CHECK(chart_string(c, s.label[0]) == "kept");
}

TEST_CASE("builder: a label the font cannot measure refuses the pass") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, NO_GLYPH);
  Spaces s;
  CHECK(!measure_chart(c, bundled(), readable(), s));
}

TEST_CASE("builder: a routeless label past the quarter-domain is refused either way") {
  // An internal self-transition's label is measured as a path box.
  auto const refused = [](std::string const &label) {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const s{ build_state(c, root, "S", StateKind::Normal, {}) };
    build_trans(c, s, s, TransKind::Internal, label);
    Spaces sp;
    return !measure_chart(c, bundled(), readable(), sp);
  };
  CHECK(refused(wide_text()));
  CHECK(refused(tall_text()));
}

TEST_CASE("builder: a path box past the quarter-domain is refused on either axis") {
  auto const refused = [](std::string const &label) {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
    build_trans(c, a, b, TransKind::Default, label);
    Spaces sp;
    return !measure_chart(c, bundled(), readable(), sp);
  };
  CHECK(refused(wide_text()));
  CHECK(refused(tall_text()));
}

TEST_CASE("builder: an emitter with no geometry column to read draws nothing") {
  Chart const c{ small_chart() };  // built, never laid out: no scav.geom.* at all
  Metrics const m{ bundled() };
  Palette const p{ palette_standard() };
  DrawList d;
  for (uint32_t i = 0; i < c.states.size(); ++i) { emit_state(d, c, m, p, i, 0); }
  for (uint32_t i = 0; i < c.submachines.size(); ++i) { emit_submachine(d, c, p, i, 0); }
  for (uint32_t i = 0; i < c.transitions.size(); ++i) { emit_route(d, {}, c, p, i, 0); }
  CHECK(d.prims.empty());
}

TEST_CASE("builder: a geometry column of a foreign shape is not read as layout's") {
  // A narrower stride reports one row per entity, over fewer bytes per row.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  geom_column(c, "scav.geom.state", ElemKind::State, ValueKind::Pod, RECT_SIZE / 2);
  geom_column(c, "scav.geom.route", ElemKind::Transition, ValueKind::Span, 4);
  geom_column(c, "scav.geom.point", ElemKind::Point, ValueKind::Pod, 4);

  DrawList d;
  emit_state(d, c, bundled(), palette_standard(), 0, 0);
  emit_state(d, c, bundled(), palette_standard(), 1, 0);
  emit_route(d, {}, c, palette_standard(), 0, 0);
  CHECK(d.prims.empty());
}

TEST_CASE("builder: a state box with no extent draws nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  build_state(c, root, "B", StateKind::Normal, {});
  ColumnId const boxes{ state_boxes(c) };
  put_row(c, boxes, 0, scav_rect{ .x = 0, .y = 0, .w = 0, .h = 40 });
  put_row(c, boxes, 1, scav_rect{ .x = 0, .y = 0, .w = 40, .h = 0 });

  DrawList d;
  emit_state(d, c, bundled(), palette_standard(), 0, 0);
  emit_state(d, c, bundled(), palette_standard(), 1, 0);
  CHECK(d.prims.empty());
}

TEST_CASE("builder: a name with no band reserved for it is not drawn") {
  // The `state_before` rect positions the name; without that column the box is drawn
  // with no name.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "Named", StateKind::Normal, {});
  ColumnId const boxes{ state_boxes(c) };
  put_row(c, boxes, 0, scav_rect{ .x = 10, .y = 20, .w = 100, .h = 60 });

  DrawList d;
  emit_state(d, c, bundled(), palette_standard(), 0, 0);
  CHECK(kind_count(d, SCAV_PRIM_RRECT) == 1);
  CHECK(kind_count(d, SCAV_PRIM_TEXT) == 0);
}

TEST_CASE("builder: a divider goes in the gap, and only where there is one to divide") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const owner{ build_state(c, root, "S", StateKind::Normal, {}) };
  SubmachineId const first{ build_submachine(c, owner, "a", {}) };
  SubmachineId const second{ build_submachine(c, owner, "b", {}) };
  ColumnId const rects{
    geom_column(c, "scav.geom.sub", ElemKind::Submachine, ValueKind::Pod, RECT_SIZE)
  };
  put_row(c, rects, first.v, scav_rect{ .x = 0, .y = 0, .w = 100, .h = 50 });
  put_row(c, rects, second.v, scav_rect{ .x = 0, .y = 60, .w = 100, .h = 50 });
  Palette const p{ palette_standard() };
  DrawList d;

  SUBCASE("regions stacked one above the other take a horizontal rule") {
    emit_submachine(d, c, p, second.v, 0);
    REQUIRE(d.prims.size() == 1);
    CHECK(d.prims[0].kind == SCAV_PRIM_LINE);
    CHECK(d.prims[0].origin_kind == static_cast<uint32_t>(ElemKind::Submachine));
    CHECK(d.prims[0].origin_ordinal == second.v);
    REQUIRE(d.prims[0].points.len == 2);
    scav_point const a{ d.points[d.prims[0].points.off] };
    scav_point const z{ d.points[d.prims[0].points.off + 1] };
    // Down the middle of the ten-unit gap, spanning both regions.
    CHECK(a.y == 55);
    CHECK(z.y == 55);
    CHECK(a.x == 0);
    CHECK(z.x == 100);
  }
  SUBCASE("a tombstoned region draws none") {
    c.submachines[second.v].live = 0;
    emit_submachine(d, c, p, second.v, 0);
    CHECK(d.prims.empty());
  }
  SUBCASE("a palette too short to index draws none") {
    emit_submachine(d, c, Palette(1), second.v, 0);
    CHECK(d.prims.empty());
  }
  SUBCASE("a region with no height draws none") {
    put_row(c, rects, second.v, scav_rect{ .x = 0, .y = 60, .w = 100, .h = 0 });
    emit_submachine(d, c, p, second.v, 0);
    CHECK(d.prims.empty());
  }
  SUBCASE("a region with no owner draws none") {
    c.submachines[second.v].owner = { INVALID };
    emit_submachine(d, c, p, second.v, 0);
    CHECK(d.prims.empty());
  }
  SUBCASE("a tombstoned sibling is not one to divide from") {
    c.submachines[first.v].live = 0;
    emit_submachine(d, c, p, second.v, 0);
    CHECK(d.prims.empty());
  }
  SUBCASE("a region its owner does not list draws none") {
    c.states[owner.v].submachines = {};
    emit_submachine(d, c, p, second.v, 0);
    CHECK(d.prims.empty());
  }
  SUBCASE("a sibling with no extent draws none") {
    put_row(c, rects, first.v, scav_rect{ .x = 0, .y = 0, .w = 0, .h = 50 });
    emit_submachine(d, c, p, second.v, 0);
    CHECK(d.prims.empty());
    put_row(c, rects, first.v, scav_rect{ .x = 0, .y = 0, .w = 100, .h = 0 });
    emit_submachine(d, c, p, second.v, 0);
    CHECK(d.prims.empty());
  }
}

TEST_CASE("builder: a route draws one polyline and one head, or nothing at all") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  ColumnId const routes{
    geom_column(c, "scav.geom.route", ElemKind::Transition, ValueKind::Span, 8)
  };
  ColumnId const points{
    geom_column(c, "scav.geom.point", ElemKind::Point, ValueKind::Pod, 8)
  };
  REQUIRE(column_resize(c, points, 2));
  put_row(c, points, 0, scav_point{ .x = 0, .y = 0 });
  put_row(c, points, 1, scav_point{ .x = 100, .y = 0 });
  put_row(c, routes, 0, scav_span{ .off = 0, .len = 2 });
  Palette const p{ palette_standard() };

  DrawList d;
  emit_route(d, {}, c, p, 0, 0);
  CHECK(kind_count(d, SCAV_PRIM_POLYLINE) == 1);
  CHECK(kind_count(d, SCAV_PRIM_PATH) == 1);
  // Nothing asked for clearance, so the head's tip is the route's last point.
  for (scav_prim const &prim : d.prims) {
    if (prim.kind != SCAV_PRIM_PATH) { continue; }
    CHECK(d.points[prim.points.off].x == 100);
    CHECK(d.points[prim.points.off].y == 0);
  }

  DrawList dead;
  c.transitions[0].live = 0;
  emit_route(dead, {}, c, p, 0, 0);
  CHECK(dead.prims.empty());
  c.transitions[0].live = 1;

  DrawList stub;
  emit_route(stub, {}, c, Palette(1), 0, 0);
  CHECK(stub.prims.empty());
}

TEST_CASE("builder: a transition that asked for no box gets none") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const s{ build_state(c, root, "S", StateKind::Normal, {}) };
  StateId const other{ build_state(c, root, "O", StateKind::Normal, {}) };
  TransId const silent{ build_trans(c, s, s, TransKind::Internal, {}) };
  TransId const dead{ build_trans(c, s, other, TransKind::Default, "gone") };
  TransId const spoken{ build_trans(c, s, s, TransKind::Internal, "tick") };
  c.transitions[dead.v].live = 0;

  Spaces sp;
  REQUIRE(measure_chart(c, bundled(), readable(), sp));
  scav_spaces const view{ as_spaces(sp) };
  scav_rect box{ .x = 1, .y = 2, .w = 3, .h = 4 };
  CHECK(!label_box(c, view, nullptr, 0, 9999, box));      // past the array
  CHECK(!label_box(c, view, nullptr, 0, dead.v, box));    // a tombstone
  CHECK(!label_box(c, view, nullptr, 0, silent.v, box));  // no label
  // Labelled, with a path box, and no placed array to read it from.
  CHECK(!label_box(c, view, nullptr, 0, spoken.v, box));
  // A zero-height `state_after` row leaves the result unchanged.
  ColumnId const after{
    geom_column(c, "scav.geom.state_after", ElemKind::State, ValueKind::Pod, RECT_SIZE)
  };
  put_row(c, after, s.v, scav_rect{ .x = 0, .y = 0, .w = 100, .h = 0 });
  CHECK(!label_box(c, view, nullptr, 0, spoken.v, box));
  CHECK(box.w == 3);  // and `out` is left alone throughout
}

TEST_CASE("builder: a routed label is found only where one was placed for it") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, "go");
  Spaces sp;
  REQUIRE(measure_chart(c, bundled(), readable(), sp));
  REQUIRE(sp.path_box.size() == 1);
  scav_spaces const view{ as_spaces(sp) };

  scav_rect box{};
  CHECK(!label_box(c, view, nullptr, 1, 0, box));  // a count with no array behind it
  std::array<scav_placed, 1> const placed{ { { .x = 5, .y = 6, .w = 7, .h = 8 } } };
  CHECK(!label_box(c, view, placed.data(), 0, 0, box));  // an array layout never filled
  REQUIRE(label_box(c, view, placed.data(), 1, 0, box));
  CHECK(box.x == 5);
  CHECK(box.h == 8);
}

TEST_CASE("builder: a label with nothing to say or no way to say it draws nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  TransId const bare{ build_trans(c, a, b, TransKind::Default, {}) };
  TransId const dead{ build_trans(c, a, b, TransKind::Default, "gone") };
  TransId const unglyphed{ build_trans(c, a, b, TransKind::Default, NO_GLYPH) };
  TransId const good{ build_trans(c, a, b, TransKind::Default, "go") };
  c.transitions[dead.v].live = 0;

  Metrics const m{ bundled() };
  Palette const p{ palette_standard() };
  scav_rect const box{ .x = 0, .y = 0, .w = 200, .h = 100 };
  DrawList d;
  emit_label(d, c, m, p, dead.v, box, 0);
  emit_label(d, c, m, Palette(1), good.v, box, 0);
  emit_label(d, c, m, p, bare.v, box, 0);
  // A label the font cannot measure emits nothing.
  emit_label(d, c, m, p, unglyphed.v, box, 0);
  CHECK(d.prims.empty());

  // The same box with a live labelled transition and a full palette draws one label.
  emit_label(d, c, m, p, good.v, box, 0);
  CHECK(d.prims.size() == 1);
}

TEST_CASE("builder: a profile whose pad is negative is refused, not drawn inside out") {
  scav_profile p{ readable() };
  p.pad = -1000;  // more than a short name is wide, so the box inverts
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "Named", StateKind::Normal, {});
  Spaces s;
  CHECK(!measure_chart(c, bundled(), p, s));
}

TEST_CASE("builder: a history circle too big for the metrics draws the ring and no mark") {
  // The mark's em is the circle's radius; when the metrics refuse that em, only the
  // ring is drawn.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, {}, StateKind::History, {});
  ColumnId const boxes{ state_boxes(c) };
  put_row(c, boxes, 0, scav_rect{ .x = 0, .y = 0, .w = 400000, .h = 400000 });

  DrawList d;
  emit_state(d, c, bundled(), palette_standard(), 0, 0);
  CHECK(kind_count(d, SCAV_PRIM_CIRCLE) == 1);
  CHECK(kind_count(d, SCAV_PRIM_TEXT) == 0);
}

TEST_CASE("builder: a name the metrics refuse is placed from its band, not its glyphs") {
  // A diamond centres its name on the measured width; an unmeasurable name is drawn
  // one eighth of the band's width in from its left.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, NO_GLYPH, StateKind::Choice, {});
  ColumnId const boxes{ state_boxes(c) };
  ColumnId const befores{
    geom_column(c, "scav.geom.state_before", ElemKind::State, ValueKind::Pod, RECT_SIZE)
  };
  put_row(c, boxes, 0, scav_rect{ .x = 0, .y = 0, .w = 400, .h = 200 });
  put_row(c, befores, 0, scav_rect{ .x = 8, .y = 4, .w = 80, .h = 40 });

  DrawList d;
  emit_state(d, c, bundled(), palette_standard(), 0, 0);
  REQUIRE(kind_count(d, SCAV_PRIM_TEXT) == 1);
  for (scav_prim const &prim : d.prims) {
    if (prim.kind != SCAV_PRIM_TEXT) { continue; }
    CHECK(d.points[prim.points.off].x == (8 + (80 / 8)));
  }
}

TEST_CASE("builder: the head's tip is the route's end and the line stops at its base") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  build_trans(c, b, a, TransKind::Default, {});
  build_trans(c, a, b, TransKind::Default, {});
  ColumnId const routes{
    geom_column(c, "scav.geom.route", ElemKind::Transition, ValueKind::Span, 8)
  };
  ColumnId const points{
    geom_column(c, "scav.geom.point", ElemKind::Point, ValueKind::Pod, 8)
  };
  REQUIRE(column_resize(c, points, 6));
  put_row(c, points, 0, scav_point{ .x = 0, .y = 0 });
  put_row(c, points, 1, scav_point{ .x = 100, .y = 0 });
  put_row(c, points, 2, scav_point{ .x = 0, .y = 0 });
  put_row(c, points, 3, scav_point{ .x = 100, .y = 50 });  // not axis-aligned
  put_row(c, points, 4, scav_point{ .x = 0, .y = 0 });
  put_row(c, points, 5, scav_point{ .x = 0, .y = 60 });  // shorter than the head
  put_row(c, routes, 0, scav_span{ .off = 0, .len = 2 });
  put_row(c, routes, 1, scav_span{ .off = 2, .len = 2 });
  put_row(c, routes, 2, scav_span{ .off = 4, .len = 2 });
  Palette const p{ palette_standard() };
  REQUIRE(p[SCAV_STYLE_LABEL].font_size_grid / 2 == 80);  // the head with no clear

  scav_path_clear const one{ .src = 0, .dst = 90 };
  scav_spaces const s{ .box_state = nullptr,
                       .n_box_state = 0,
                       .box_sub = nullptr,
                       .n_box_sub = 0,
                       .path_clear = &one,
                       .n_path_clear = 1,
                       .path_box = nullptr,
                       .n_path_box = 0 };

  struct Drawn {
    scav_point tip, line_end;
    uint32_t lines;
  };
  auto const drawn = [&](uint32_t trans, scav_spaces const &spaces) {
    DrawList d;
    emit_route(d, spaces, c, p, trans, 0);
    REQUIRE(kind_count(d, SCAV_PRIM_PATH) == 1);
    Drawn out{ .tip = {}, .line_end = {}, .lines = kind_count(d, SCAV_PRIM_POLYLINE) };
    for (scav_prim const &prim : d.prims) {
      if (prim.kind == SCAV_PRIM_PATH) { out.tip = d.points[prim.points.off]; }
      if (prim.kind == SCAV_PRIM_POLYLINE) {
        out.line_end = d.points[prim.points.off + prim.points.len - 1U];
      }
    }
    return out;
  };

  // The table's clear of 90 sizes transition 0's head.
  Drawn const asked{ drawn(0, s) };
  CHECK(asked.tip.x == 100);
  CHECK(asked.tip.y == 0);
  CHECK(asked.line_end.x == 10);
  CHECK(asked.line_end.y == 0);
  // With no clear table the head is half the label font.
  Drawn const flat{ drawn(0, {}) };
  CHECK(flat.tip.x == 100);
  CHECK(flat.line_end.x == 20);
  // A diagonal leg steps back along itself: 80 of its 111 units.
  Drawn const slant{ drawn(1, s) };
  CHECK(slant.tip.x == 100);
  CHECK(slant.tip.y == 50);
  CHECK(slant.line_end.x == 28);
  CHECK(slant.line_end.y == 14);
  // The head covers a leg shorter than itself; one leg leaves no line.
  Drawn const stub{ drawn(2, s) };
  CHECK(stub.tip.x == 0);
  CHECK(stub.tip.y == 60);
  CHECK(stub.lines == 0);
}

TEST_CASE("builder: a route whose point column was never filled draws nothing") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::Default, {});
  ColumnId const routes{
    geom_column(c, "scav.geom.route", ElemKind::Transition, ValueKind::Span, 8)
  };
  // Registered and never resized: a point column carries its own length.
  geom_column(c, "scav.geom.point", ElemKind::Point, ValueKind::Pod, 8);
  put_row(c, routes, 0, scav_span{ .off = 0, .len = 0 });

  DrawList d;
  emit_route(d, {}, c, palette_standard(), 0, 0);
  CHECK(d.prims.empty());
}

TEST_CASE("builder: emit_chart refuses a palette too short to index") {
  Built const b{ pipeline(small_chart(), readable()) };
  DrawList d;
  CHECK(!emit_chart(d,
                    b.chart,
                    bundled(),
                    Palette(1),
                    as_spaces(b.spaces),
                    b.placed.data(),
                    static_cast<uint32_t>(b.placed.size()),
                    0));
  CHECK(d.prims.empty());
}
