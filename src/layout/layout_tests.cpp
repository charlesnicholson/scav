// Exact rects from the box formula, stacking, routes, placed boxes, the
// columns, the hash split, and a geometry invariant sweep.

#include "layout/decompose.h"
#include "scav/scav_core.h"
#include "scav/scav_core_c.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_c_handles.h"

#include "doctest.h"

#include "scav_int.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace scav;

// The C structs carry no operators; the tests compare them field-wise.
constexpr bool operator==(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

// The default router and no thread request, which is every test that does not
// say otherwise.
scav_layout_opts opts(scav_profile const &p) {
  return { .profile = p, .router = 0, .threads = 0 };
}

// A run expected to succeed, returning its placed boxes.
std::vector<scav_placed> run(Chart &c, scav_spaces const &s, scav_profile const &p) {
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  bool const ok{ layout_run(c, s, opts(p), placed, diags) };
  std::string why;
  for (Diagnostic const &d : diags) {
    why += diag_message(d.code);
    why += "; ";
  }
  CAPTURE(why);
  REQUIRE(ok);
  return placed;
}

// Typed column reads over the public accessors -- what any consumer writes.
template <typename T>
T row_of(Chart const &c, char const *name, uint32_t row) {
  ColumnId const id{ column_find(c, name) };
  REQUIRE(id.v != INVALID);
  REQUIRE(row < column_count(c, id));
  T out{};
  std::memcpy(&out,
              column_data(c, id) + (static_cast<size_t>(row) * sizeof(T)),
              sizeof(T));
  return out;
}

scav_rect state_rect(Chart const &c, StateId s) {
  return row_of<scav_rect>(c, "scav.geom.state", s.v);
}
scav_rect sub_rect(Chart const &c, SubmachineId m) {
  return row_of<scav_rect>(c, "scav.geom.sub", m.v);
}

bool inside(scav_rect const &inner, scav_rect const &outer) {
  return (inner.x >= outer.x) && (inner.y >= outer.y) &&
         ((inner.x + inner.w) <= (outer.x + outer.w)) &&
         ((inner.y + inner.h) <= (outer.y + outer.h));
}

bool on_border(scav_point pt, scav_rect const &r) {
  bool const x_edge{ (pt.x == r.x) || (pt.x == (r.x + r.w)) };
  bool const y_edge{ (pt.y == r.y) || (pt.y == (r.y + r.h)) };
  bool const x_in{ (pt.x >= r.x) && (pt.x <= (r.x + r.w)) };
  bool const y_in{ (pt.y >= r.y) && (pt.y <= (r.y + r.h)) };
  return (x_edge && y_in) || (y_edge && x_in);
}

}  // namespace

TEST_CASE("layout: an empty chart lays out to nothing, hashed and stable") {
  Chart c;
  std::vector<scav_placed> const placed{ run(c, {}, readable()) };
  CHECK(placed.empty());
  CHECK((row_of<scav_rect>(c, "scav.geom.chart", 0) == scav_rect{}));
  CHECK(row_of<uint32_t>(c, "scav.geom.gen", 0) == 1);
  CHECK(layout_structural_hash(c) == layout_structural_hash(c));
  CHECK(layout_coordinate_hash(c) == layout_coordinate_hash(c));
}

TEST_CASE("layout: a lone leaf sizes to its kind minimum plus padding") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  scav_profile const p{ readable() };
  run(c, {}, p);

  int32_t const w{ p.kind_min_w[0] + (2 * p.pad) };
  int32_t const h{ p.kind_min_h[0] + (2 * p.pad) };
  CHECK((state_rect(c, a) == scav_rect{ .x = 0, .y = 0, .w = w, .h = h }));
  CHECK((row_of<scav_rect>(c, "scav.geom.chart", 0) ==
         scav_rect{ .x = 0, .y = 0, .w = w, .h = h }));
  // Zero-height bands still land at their offsets inside the padding.
  CHECK((row_of<scav_rect>(c, "scav.geom.state_before", a.v) ==
         scav_rect{ .x = p.pad, .y = p.pad, .w = p.kind_min_w[0], .h = 0 }));
}

TEST_CASE("layout: each arm of the box formula can dominate") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const by_min_w{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const by_kind{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const by_subs{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, by_subs, {}, {}) };
  build_state(c, inner, "K", StateKind::Normal, {});

  scav_profile const p{ readable() };
  std::vector<scav_box_space> boxes(c.states.size());
  boxes[by_min_w.v] = { .min_w = 9000, .h_before = 100, .h_after = 50 };
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };
  run(c, s, p);

  // min_w dominates A's width; its height stacks the bands over kind_min_h.
  int32_t const bands{ 100 + 50 };
  int32_t const a_h{ ((bands > p.kind_min_h[0]) ? bands : p.kind_min_h[0]) + 2 * p.pad };
  CHECK(state_rect(c, by_min_w).w == 9000 + (2 * p.pad));
  CHECK(state_rect(c, by_min_w).h == a_h);

  // B has nothing: kind minimum alone.
  CHECK(state_rect(c, by_kind).w == p.kind_min_w[0] + (2 * p.pad));

  // C wraps its child submachine: child leaf width plus two pads.
  int32_t const leaf_w{ p.kind_min_w[0] + (2 * p.pad) };
  CHECK(sub_rect(c, inner).w == leaf_w);
  CHECK(state_rect(c, by_subs).w == leaf_w + (2 * p.pad));
}

TEST_CASE("layout: interior bands and submachines stack from the top") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const m1{ build_submachine(c, comp, {}, {}) };
  SubmachineId const m2{ build_submachine(c, comp, {}, {}) };
  build_state(c, m1, "A", StateKind::Normal, {});
  build_state(c, m2, "B", StateKind::Normal, {});

  scav_profile const p{ readable() };
  std::vector<scav_box_space> boxes(c.states.size());
  boxes[comp.v] = { .min_w = 0, .h_before = 40, .h_after = 24 };
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };
  run(c, s, p);

  scav_rect const outer{ state_rect(c, comp) };
  scav_rect const before{ row_of<scav_rect>(c, "scav.geom.state_before", comp.v) };
  scav_rect const r1{ sub_rect(c, m1) };
  scav_rect const r2{ sub_rect(c, m2) };
  scav_rect const after{ row_of<scav_rect>(c, "scav.geom.state_after", comp.v) };

  CHECK(before.y == outer.y + p.pad);
  CHECK(before.h == 40);
  CHECK(r1.y == before.y + before.h);
  CHECK(r2.y == r1.y + r1.h + p.pad);  // concurrent regions stack with a gap
  CHECK(after.y == r2.y + r2.h);
  CHECK(after.h == 24);
  CHECK(inside(before, outer));
  CHECK(inside(r1, outer));
  CHECK(inside(r2, outer));
  CHECK(inside(after, outer));
}

TEST_CASE("layout: siblings stack in document order with one pad between") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  scav_profile const p{ readable() };
  run(c, {}, p);

  scav_rect const ra{ state_rect(c, a) };
  scav_rect const rb{ state_rect(c, b) };
  scav_rect const rd{ state_rect(c, d) };
  CHECK(ra.y == 0);
  CHECK(rb.y == ra.y + ra.h + p.pad);
  CHECK(rd.y == rb.y + rb.h + p.pad);
  CHECK(row_of<scav_rect>(c, "scav.geom.chart", 0).h == rd.y + rd.h);
}

TEST_CASE("layout: children wrap into rows at the aspect-ratio target") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  std::vector<StateId> kids;
  kids.reserve(9);
  for (uint32_t i = 0; i < 9; ++i) {
    kids.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  scav_profile const p{ readable() };
  run(c, {}, p);

  // Nine identical leaves: the same isqrt the packer runs says two per row.
  int32_t const w{ p.kind_min_w[0] + (2 * p.pad) };
  int32_t const h{ p.kind_min_h[0] + (2 * p.pad) };
  Wide const target{ static_cast<Wide>(
      isqrt(static_cast<uint64_t>(9LL * w * h * p.dar_num / p.dar_den))) };
  REQUIRE(target >= (2 * w) + p.pad);       // two fit
  REQUIRE(target < (3 * w) + (2 * p.pad));  // three do not

  CHECK((state_rect(c, kids[0]) == scav_rect{ .x = 0, .y = 0, .w = w, .h = h }));
  CHECK((state_rect(c, kids[1]) == scav_rect{ .x = w + p.pad, .y = 0, .w = w, .h = h }));
  CHECK((state_rect(c, kids[2]) == scav_rect{ .x = 0, .y = h + p.pad, .w = w, .h = h }));
  CHECK((state_rect(c, kids[8]) ==
         scav_rect{ .x = 0, .y = 4 * (h + p.pad), .w = w, .h = h }));
  CHECK((row_of<scav_rect>(c, "scav.geom.chart", 0) ==
         scav_rect{ .x = 0, .y = 0, .w = (2 * w) + p.pad, .h = (5 * h) + (4 * p.pad) }));
}

TEST_CASE("layout: routes are straight, ports on borders, self-loops outside") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s1{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const d1{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, s1, d1, TransKind::External, {});  // t0: one port on comp
  build_trans(c, d1, d1, TransKind::External, {});  // t1: loop outside
  build_trans(c, d1, d1, TransKind::Internal, {});  // t2: no route
  scav_profile const p{ readable() };
  run(c, {}, p);

  scav_span const r0{ row_of<scav_span>(c, "scav.geom.route", 0) };
  REQUIRE(r0.len == 3);
  scav_point const src{ row_of<scav_point>(c, "scav.geom.point", r0.off) };
  scav_point const mid{ row_of<scav_point>(c, "scav.geom.point", r0.off + 1) };
  scav_rect const rs{ state_rect(c, s1) };
  CHECK(src.x == rs.x + (rs.w / 2));
  CHECK(src.y == rs.y + (rs.h / 2));
  CHECK(on_border(mid, state_rect(c, comp)));

  scav_span const ports{ row_of<scav_span>(c, "scav.geom.port", 0) };
  REQUIRE(ports.len == 1);
  scav_port_slot const slot{ row_of<scav_port_slot>(c, "scav.geom.portslot", ports.off) };
  CHECK(slot.x == mid.x);
  CHECK(slot.y == mid.y);
  CHECK(slot.boundary_depth == 0);
  CHECK(slot.side <= 3);

  scav_span const r1{ row_of<scav_span>(c, "scav.geom.route", 1) };
  REQUIRE(r1.len == 2);
  scav_rect const rd{ state_rect(c, d1) };
  scav_point const lip{ row_of<scav_point>(c, "scav.geom.point", r1.off) };
  CHECK(lip.x == rd.x + rd.w);  // leaves through the right border
  CHECK(row_of<scav_point>(c, "scav.geom.point", r1.off + 1).x == lip.x + (2 * p.pad));

  CHECK(row_of<scav_span>(c, "scav.geom.route", 2).len == 0);
  CHECK(row_of<scav_span>(c, "scav.geom.port", 2).len == 0);
}

TEST_CASE("layout: path clears trim the route ends by exact integers") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  scav_profile const p{ readable() };

  std::vector<scav_path_clear> const clears{ { .src = 10, .dst = 6 } };
  scav_spaces const s{ .path_clear = clears.data(), .n_path_clear = 1 };
  run(c, s, p);

  // A stacks above B, so the route is vertical: trims apply to y alone.
  scav_span const r{ row_of<scav_span>(c, "scav.geom.route", 0) };
  scav_point const p0{ row_of<scav_point>(c, "scav.geom.point", r.off) };
  scav_point const p1{ row_of<scav_point>(c, "scav.geom.point", r.off + 1) };
  scav_rect const ra{ state_rect(c, a) };
  scav_rect const rb{ state_rect(c, b) };
  CHECK(p0.x == ra.x + (ra.w / 2));
  CHECK(p0.y == (ra.y + (ra.h / 2)) + 10);
  CHECK(p1.y == (rb.y + (rb.h / 2)) - 6);
}

TEST_CASE("layout: placed boxes center on the route midpoint") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 20, .h = 8, .order = 0 } };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 1 };
  std::vector<scav_placed> const placed{ run(c, s, readable()) };

  REQUIRE(placed.size() == 1);
  scav_span const r{ row_of<scav_span>(c, "scav.geom.route", 0) };
  scav_point const mid{ row_of<scav_point>(c, "scav.geom.point", r.off + (r.len / 2)) };
  CHECK((placed[0] == scav_placed{ .x = mid.x - 10, .y = mid.y - 4, .w = 20, .h = 8 }));
}

TEST_CASE("layout: reruns rewrite in place, bumping only the generation") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  scav_profile const p{ readable() };

  run(c, {}, p);
  uint32_t const columns_after_first{ static_cast<uint32_t>(c.columns.size()) };
  uint32_t const coord{ layout_coordinate_hash(c) };
  uint32_t const structure{ layout_structural_hash(c) };

  run(c, {}, p);
  CHECK(static_cast<uint32_t>(c.columns.size()) == columns_after_first);
  CHECK(row_of<uint32_t>(c, "scav.geom.gen", 0) == 2);
  CHECK(layout_coordinate_hash(c) == coord);
  CHECK(layout_structural_hash(c) == structure);
}

TEST_CASE("layout: the hash split separates size changes from shape changes") {
  auto build = [](int32_t min_w) {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
    build_trans(c, a, b, TransKind::External, {});
    std::vector<scav_box_space> boxes(c.states.size());
    boxes[0].min_w = min_w;
    boxes[1].min_w = min_w;
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()) };
    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    REQUIRE(layout_run(c, s, opts(readable()), placed, diags));
    return c;
  };

  Chart const narrow{ build(0) };
  Chart const wide{ build(4000) };
  // Both boxes widen together: every coordinate moves, the route still points
  // straight down, so only the coordinate hash may change.
  CHECK(layout_coordinate_hash(narrow) != layout_coordinate_hash(wide));
  CHECK(layout_structural_hash(narrow) == layout_structural_hash(wide));

  Chart more{ build(0) };
  build_trans(more, { 1 }, { 0 }, TransKind::External, {});
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  REQUIRE(layout_run(more, {}, opts(p), placed, diags));
  CHECK(layout_structural_hash(more) != layout_structural_hash(narrow));
}

TEST_CASE("layout: the inputs digest hears every input that is not the model") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  CHECK(layout_inputs_digest(c) == 0);  // never laid out

  run(c, {}, readable());
  uint32_t const base{ layout_inputs_digest(c) };
  CHECK(base != 0);
  run(c, {}, readable());
  CHECK(layout_inputs_digest(c) == base);  // same inputs, same digest

  // A profile knob layout does read, and one it does not: both are inputs a
  // golden was produced under, so both move the digest.
  scav_profile moved{ readable() };
  moved.pad += 1;
  run(c, {}, moved);
  CHECK(layout_inputs_digest(c) != base);

  scav_profile renamed{ readable() };
  renamed.profile_version += 1;
  run(c, {}, renamed);
  CHECK(layout_inputs_digest(c) != base);

  // The space tables ride in, which is how the font reaches a digest it can
  // never be an argument to.
  std::vector<scav_box_space> boxes(c.states.size());
  boxes[0].min_w = 64;
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };
  run(c, s, readable());
  CHECK(layout_inputs_digest(c) != base);

  // And it is a third value, not a seed: a space change that leaves the shape
  // alone must not disturb the structural hash.
  run(c, {}, readable());
  uint32_t const structure{ layout_structural_hash(c) };
  run(c, s, readable());
  CHECK(layout_structural_hash(c) == structure);
  CHECK(layout_inputs_digest(c) != base);
}

TEST_CASE("layout: the router carries a version, and both stop at the end") {
  scav_byte const *name{ nullptr };
  uint32_t len{ 0 };
  uint32_t version{ 0 };
  REQUIRE(router_name(0, name, len));
  REQUIRE(router_version(0, version));
  CHECK(version >= 1);
  CHECK(!router_name(router_count(), name, len));
  CHECK(!router_version(router_count(), version));
}

TEST_CASE("layout: composed geometry past the domain is rejected, columns kept") {
  // Maximal bands at every nesting level compound the enclosing heights past
  // COORD_MAX in three levels: legal inputs, illegal composition.
  Chart c;
  SubmachineId parent{ build_chart(c, "t", {}) };
  for (uint32_t i = 0; i < 3; ++i) {
    StateId const comp{ build_state(c, parent, {}, StateKind::Normal, {}) };
    parent = build_submachine(c, comp, {}, {});
  }
  std::vector<scav_box_space> boxes(
      c.states.size(),
      { .min_w = 0, .h_before = SPACE_MAX, .h_after = SPACE_MAX });
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  CHECK(!layout_run(c, s, opts(readable()), placed, diags));
  REQUIRE(!diags.empty());
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::State);
  CHECK(column_find(c, "scav.geom.state").v == INVALID);  // nothing written
}

TEST_CASE("layout: a packed row wider than the domain is rejected") {
  // dar at its bound stretches the aspect target past COORD_MAX, so five
  // maximal-width children land in one row that no wrapping state bounds.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  for (uint32_t i = 0; i < 5; ++i) { build_state(c, root, {}, StateKind::Normal, {}); }
  std::vector<scav_box_space> const boxes(
      c.states.size(),
      { .min_w = SPACE_MAX, .h_before = 0, .h_after = 0 });
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };
  scav_profile wide{ readable() };
  wide.dar_num = 1024;
  wide.dar_den = 1;

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  CHECK(!layout_run(c, s, opts(wide), placed, diags));
  REQUIRE(!diags.empty());
  CHECK(diags[0].code == DiagCode::CoordinateOverflow);
  CHECK(diags[0].subject.kind == ElemKind::Submachine);
  CHECK(diags[0].subject.ordinal == root.v);
  CHECK(column_find(c, "scav.geom.state").v == INVALID);
}

TEST_CASE("layout: invalid profiles and spaces fail before any geometry") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});

  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_profile bad{ readable() };
  bad.dar_den = 0;
  CHECK(!layout_run(c, {}, opts(bad), placed, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::ProfileOutOfRange);

  diags.clear();
  std::vector<scav_box_space> boxes{ { .min_w = -1, .h_before = 0, .h_after = 0 } };
  scav_spaces const s{ .box_state = boxes.data(), .n_box_state = 1 };
  CHECK(!layout_run(c, s, opts(readable()), placed, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::SpaceOutOfRange);
  CHECK(column_find(c, "scav.geom.state").v == INVALID);
}

TEST_CASE("layout: tombstones leave zero rects and no routes") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  c.states[b.v].live = 0;
  c.transitions[0].live = 0;
  run(c, {}, readable());

  CHECK((state_rect(c, b) == scav_rect{}));
  CHECK(row_of<scav_span>(c, "scav.geom.route", 0).len == 0);
  // The dead sibling neither occupies a slot nor leaves a gap.
  CHECK(state_rect(c, d).y == state_rect(c, a).h + readable().pad);
}

TEST_CASE("layout: the C surface runs, queries, and reports end to end") {
  scav_load *loader{ nullptr };
  REQUIRE(scav_load_begin(&loader) == SCAV_OK);
  std::string_view const text{ R"(chart t { state A, state B, trans A -> B, })" };
  REQUIRE(scav_load_add(loader,
                        reinterpret_cast<scav_byte const *>(text.data()),
                        static_cast<uint32_t>(text.size()),
                        "t.scav") == SCAV_OK);
  scav_chart *chart{ nullptr };
  REQUIRE(scav_load_finish(loader, &chart) == SCAV_OK);

  scav_layout_opts opts{};
  REQUIRE(scav_profile_named("compact", &opts.profile) == SCAV_OK);

  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 10, .h = 4, .order = 0 } };
  scav_spaces const s{ .path_box = boxes.data(), .n_path_box = 1 };

  // The out-param protocol: query, too small, filled.
  uint32_t count{ 0 };
  REQUIRE(scav_layout_run(chart, &s, &opts, nullptr, 0, &count) == SCAV_OK);
  CHECK(count == 1);
  scav_placed one{};
  CHECK(scav_layout_run(chart, &s, &opts, &one, 1, &count) == SCAV_OK);
  CHECK(one.w == 10);

  // Geometry reads back through the three-call accessor.
  scav_column_id id{ 0 };
  REQUIRE(scav_column_find(chart, "scav.geom.state", &id) == SCAV_OK);
  uint32_t rows{ 0 };
  uint32_t stride{ 0 };
  scav_byte const *data{ nullptr };
  REQUIRE(scav_column_count(chart, id, &rows) == SCAV_OK);
  REQUIRE(scav_column_data(chart, id, &data, &stride) == SCAV_OK);
  CHECK(stride == sizeof(scav_rect));
  CHECK(rows == chart->chart.states.size());

  // A bad router id is an argument error; a bad profile is a diagnosed one.
  opts.router = 99;
  CHECK(scav_layout_run(chart, &s, &opts, &one, 1, &count) == SCAV_E_INVALID_ARG);
  opts.router = 0;
  opts.profile.trybox = 7;
  CHECK(scav_layout_run(chart, &s, &opts, &one, 1, &count) == SCAV_E_LAYOUT);
  uint32_t n_diags{ 0 };
  REQUIRE(scav_chart_diag_count(chart, &n_diags) == SCAV_OK);
  REQUIRE(n_diags == 1);
  scav_diag d{};
  REQUIRE(scav_chart_diag(chart, 0, &d) == SCAV_OK);
  CHECK(d.code == static_cast<uint32_t>(DiagCode::ProfileOutOfRange));

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}

namespace {

// The invariants any laid-out chart owes, checked from the columns alone.
void check_geometry(Chart const &c) {
  SplitGraph const g{ decompose(c) };
  scav_rect const chart_box{ row_of<scav_rect>(c, "scav.geom.chart", 0) };

  for (uint32_t i = 0; i < c.states.size(); ++i) {
    scav_rect const r{ state_rect(c, { i }) };
    if (c.states[i].live == 0) {
      CHECK((r == scav_rect{}));
      continue;
    }
    CHECK(r.w > 0);
    CHECK(inside(r, sub_rect(c, c.states[i].parent)));
    CHECK(inside(row_of<scav_rect>(c, "scav.geom.state_before", i), r));
    CHECK(inside(row_of<scav_rect>(c, "scav.geom.state_after", i), r));
    CHECK(static_cast<Wide>(r.x) + r.w <= COORD_MAX);
    CHECK(static_cast<Wide>(r.y) + r.h <= COORD_MAX);
  }
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    StateId const owner{ c.submachines[m].owner };
    if (owner.v != INVALID) {
      CHECK(inside(sub_rect(c, { m }), state_rect(c, owner)));
    } else {
      CHECK(inside(sub_rect(c, { m }), chart_box));
    }
  }
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    scav_span const route{ row_of<scav_span>(c, "scav.geom.route", t) };
    scav_span const ports{ row_of<scav_span>(c, "scav.geom.port", t) };
    Span const segs{ g.trans_segments[t] };
    if (segs.len == 0) {
      CHECK(route.len == 0);
      CHECK(ports.len == 0);
      continue;
    }
    Transition const &tr{ c.transitions[t] };
    CHECK(route.len == ((tr.src == tr.dst) ? 2 : segs.len + 1));
    CHECK(ports.len == segs.len - 1);
    for (uint32_t k = 0; k < ports.len; ++k) {
      scav_port_slot const slot{
        row_of<scav_port_slot>(c, "scav.geom.portslot", ports.off + k)
      };
      SplitPort const &port{ g.ports[g.segments[segs.off + k].dst_port] };
      scav_rect const boundary{ (port.state.v != INVALID) ? state_rect(c, port.state)
                                                          : sub_rect(c, port.sub) };
      CHECK(on_border({ .x = slot.x, .y = slot.y }, boundary));
      CHECK(slot.side <= 3);
    }
  }
}

}  // namespace

TEST_CASE("layout: geometry invariants hold across topologies and spaces") {
  // The split sweep's fixture: mixed depth, concurrency, nesting in a region.
  auto fixture = [] {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    build_state(c, root, "A", StateKind::Normal, {});
    StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
    SubmachineId const b_m{ build_submachine(c, b, {}, {}) };
    build_state(c, b_m, "B1", StateKind::Normal, {});
    StateId const b2{ build_state(c, b_m, "B2", StateKind::Normal, {}) };
    SubmachineId const b2_m{ build_submachine(c, b2, {}, {}) };
    build_state(c, b2_m, "B21", StateKind::Normal, {});
    StateId const o{ build_state(c, root, "O", StateKind::Normal, {}) };
    SubmachineId const m1{ build_submachine(c, o, "m1", {}) };
    SubmachineId const m2{ build_submachine(c, o, "m2", {}) };
    StateId const p{ build_state(c, m1, "P", StateKind::Normal, {}) };
    SubmachineId const p_m{ build_submachine(c, p, {}, {}) };
    build_state(c, p_m, "P1", StateKind::Normal, {});
    build_state(c, m2, "Q", StateKind::Normal, {});
    return c;
  };

  Chart c{ fixture() };
  uint32_t const n{ static_cast<uint32_t>(c.states.size()) };
  for (uint32_t src = 0; src < n; ++src) {
    for (uint32_t dst = 0; dst < n; ++dst) {
      for (TransKind const kind :
           { TransKind::External, TransKind::Internal, TransKind::Local }) {
        build_trans(c, { src }, { dst }, kind, {});
      }
    }
  }

  SUBCASE("with no space requests") { run(c, {}, readable()); }
  SUBCASE("with fabricated measurement") {
    // A pure integer function of the model, the way a real app measures.
    std::vector<scav_box_space> boxes(c.states.size());
    for (uint32_t i = 0; i < c.states.size(); ++i) {
      boxes[i] = { .min_w = static_cast<int32_t>(200 + (i * 40)),
                   .h_before = static_cast<int32_t>(30 + (i % 3) * 10),
                   .h_after = static_cast<int32_t>((i % 2) * 20) };
    }
    std::vector<scav_path_clear> clears(c.transitions.size(), { .src = 4, .dst = 8 });
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()),
                         .path_clear = clears.data(),
                         .n_path_clear = static_cast<uint32_t>(clears.size()) };
    run(c, s, readable());
  }
  check_geometry(c);
}

namespace {

// The scale target: depth 16, ~2k states, ~3.7k transitions including one
// long hierarchical edge per level.
Chart two_k_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  for (uint32_t r = 0; r < 8; ++r) {
    SubmachineId parent{ root };
    StateId last{ INVALID };
    for (uint32_t d = 0; d < 16; ++d) {
      std::vector<StateId> level;
      level.reserve(15);
      for (uint32_t k = 0; k < 15; ++k) {
        level.push_back(build_state(c, parent, {}, StateKind::Normal, {}));
      }
      StateId const comp{ build_state(c, parent, {}, StateKind::Normal, {}) };
      for (uint32_t k = 1; k < level.size(); ++k) {
        build_trans(c, level[k - 1], level[k], TransKind::External, {});
        build_trans(c, level[k], comp, TransKind::External, {});
      }
      if (last.v != INVALID) { build_trans(c, comp, last, TransKind::External, {}); }
      last = comp;
      parent = build_submachine(c, comp, {}, {});
    }
  }
  return c;
}

}  // namespace

TEST_CASE("layout: two thousand states lay out, and quickly") {
  Chart c{ two_k_chart() };
  REQUIRE(c.states.size() >= 2000);
  REQUIRE(c.transitions.size() >= 3500);

  auto const t0{ std::chrono::steady_clock::now() };
  run(c, {}, readable());
  auto const t1{ std::chrono::steady_clock::now() };
  auto const us{ std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() };
  MESSAGE("layout_run over ",
          c.states.size(),
          " states / ",
          c.transitions.size(),
          " transitions: ",
          us,
          " us");
#if SCAV_PERF_ASSERT_FLOOR == 1
  // A floor, not a time: named machines only, never under instrumentation.
  CHECK(us < 20000);
#endif
  check_geometry(c);
}

TEST_CASE("layout: the coordinate extent estimate holds under fat text") {
  // Deliberately generous stand-ins for measured text -- twenty wide glyphs
  // of width, two title lines, a compartment -- so the grid decision errs
  // conservative: if this fits, real fonts fit smaller.
  Chart c{ two_k_chart() };
  std::vector<scav_box_space> const boxes(
      c.states.size(),
      { .min_w = 3200, .h_before = 448, .h_after = 96 });
  scav_spaces const s{ .box_state = boxes.data(),
                       .n_box_state = static_cast<uint32_t>(boxes.size()) };
  run(c, s, readable());

  // Measured: 181120 x 277888 -- inside the domain with 1.9x headroom on the
  // tall axis, so the 1/16 pt grid stands. The bar below trips when a change
  // eats the margin.
  scav_rect const extent{ row_of<scav_rect>(c, "scav.geom.chart", 0) };
  MESSAGE("2k-state fat-text extent: ",
          extent.w,
          " x ",
          extent.h,
          " of ",
          COORD_MAX,
          " grid units");
  CHECK(extent.w <= (COORD_MAX / 4) * 3);
  CHECK(extent.h <= (COORD_MAX / 4) * 3);
}

TEST_CASE("layout: corpus charts hash to the committed golden") {
  // Three columns per chart: the inputs digest naming the measurement policy,
  // then the structural and coordinate hashes it produced. The policy here is
  // no space requests and the readable profile -- what dump --layout does.
  std::string actual;
  for (char const *name : { "axis.scav",
                            "brew.scav",
                            "dock.scav",
                            "estop.scav",
                            "led.scav",
                            "mill.scav",
                            "ota.scav",
                            "tcp.scav",
                            "toolchanger.scav",
                            "vac.scav" }) {
    CAPTURE(name);
    std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
    path += name;
    Loader loader;
    Chart c;
    std::vector<Diagnostic> diags;
    std::string failed;
    REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
    run(c, {}, readable());
    actual += name;
    actual += ' ';
    string_append_hex32(actual, layout_inputs_digest(c));
    actual += ' ';
    string_append_hex32(actual, layout_structural_hash(c));
    actual += ' ';
    string_append_hex32(actual, layout_coordinate_hash(c));
    actual += '\n';
  }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/layout/corpus_hashes.txt", golden));
  std::string const want{ reinterpret_cast<char const *>(golden.data()), golden.size() };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_hashes.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_hashes.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("layout: fuzzed charts and spaces either lay out or diagnose") {
  uint64_t rng{ 0x5CA7'F00D'0123'4567ULL };
  auto const next = [&rng](uint32_t bound) {
    rng = (rng * 6364136223846793005ULL) + 1442695040888963407ULL;
    return static_cast<uint32_t>((rng >> 33U) % bound);
  };

  for (uint32_t iter = 0; iter < 150; ++iter) {
    CAPTURE(iter);
    Chart c;
    SubmachineId const root{ build_chart(c, "f", {}) };
    std::vector<SubmachineId> frames{ root };
    uint32_t const n_states{ 2 + next(10) };
    for (uint32_t i = 0; i < n_states; ++i) {
      SubmachineId const parent{ frames[next(static_cast<uint32_t>(frames.size()))] };
      StateId const st{ build_state(c, parent, {}, StateKind::Normal, {}) };
      if (next(3) == 0) { frames.push_back(build_submachine(c, st, {}, {})); }
      if (next(4) == 0) { frames.push_back(build_submachine(c, st, {}, {})); }
    }
    uint32_t const total{ static_cast<uint32_t>(c.states.size()) };
    for (uint32_t i = next(12); i-- > 0;) {
      build_trans(c,
                  { next(total) },
                  { next(total) },
                  static_cast<TransKind>(next(3)),
                  {});
    }
    if (!c.transitions.empty() && (next(4) == 0)) {
      c.transitions[next(static_cast<uint32_t>(c.transitions.size()))].live = 0;
    }

    // Hostile by construction: fields wander outside the domain, subjects
    // outside the transition array, orders colliding.
    std::vector<scav_box_space> boxes(c.states.size());
    for (scav_box_space &b : boxes) {
      b = { .min_w = static_cast<int32_t>(next(200000)) - 20000,
            .h_before = static_cast<int32_t>(next(150000)) - 10000,
            .h_after = static_cast<int32_t>(next(150000)) - 10000 };
    }
    std::vector<scav_path_box> path_boxes(next(4));
    for (scav_path_box &b : path_boxes) {
      b = { .subject = next(static_cast<uint32_t>(c.transitions.size()) + 2),
            .w = static_cast<int32_t>(next(150000)) - 10000,
            .h = static_cast<int32_t>(next(150000)) - 10000,
            .order = next(3) };
    }
    scav_spaces const s{ .box_state = boxes.data(),
                         .n_box_state = static_cast<uint32_t>(boxes.size()),
                         .path_box = path_boxes.data(),
                         .n_path_box = static_cast<uint32_t>(path_boxes.size()) };

    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    if (layout_run(c, s, opts(readable()), placed, diags)) {
      CHECK(diags.empty());
      CHECK(placed.size() == path_boxes.size());
      check_geometry(c);
    } else {
      CHECK(!diags.empty());
    }
  }
}
