// The read side of the model: refs, liveness, attr lookup, and addressing.
// chart_path_of produces the addressing spellings, so most of this is paths.

#include "core/core_internal.h"
#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

}  // namespace

TEST_CASE("chart: entity counts follow the arrays; the chart itself is one") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  CHECK(chart_entity_count(c, ElemKind::State) == 1);
  CHECK(chart_entity_count(c, ElemKind::Submachine) == 1);
  CHECK(chart_entity_count(c, ElemKind::Transition) == 0);
  CHECK(chart_entity_count(c, ElemKind::Chart) == 1);
  // No point or path-box arrays exist; their counts are their columns'.
  CHECK(chart_entity_count(c, ElemKind::Point) == 0);
  CHECK(chart_entity_count(c, ElemKind::PathBox) == 0);
  CHECK(chart_entity_count(c, ElemKind::None) == 0);
}

TEST_CASE("chart: ref validity is bounds plus kind") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  CHECK(chart_ref_valid(c, ref(a)));
  CHECK(chart_ref_valid(c, chart_ref()));
  CHECK_FALSE(chart_ref_valid(c, ElemRef{ ElemKind::Chart, 1 }));
  CHECK_FALSE(chart_ref_valid(c, ElemRef{ ElemKind::State, 1 }));
  CHECK_FALSE(chart_ref_valid(c, ElemRef{ ElemKind::State, INVALID }));
  CHECK_FALSE(chart_ref_valid(c, ElemRef{ ElemKind::Point, 0 }));
  CHECK_FALSE(chart_ref_valid(c, ElemRef{ ElemKind::None, 0 }));
}

TEST_CASE("chart: liveness is the row's flag; the chart entity has none") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  TransId const t{ build_trans(c, a, a, TransKind::External, {}) };
  CHECK(chart_live(c, ref(a)));
  CHECK(chart_live(c, ref(t)));
  CHECK(chart_live(c, ref(root)));
  CHECK(chart_live(c, chart_ref()));
  c.states[a.v].live = 0;
  CHECK_FALSE(chart_live(c, ref(a)));
  CHECK(chart_live(c, ref(t)));  // liveness belongs to each entity row
}

TEST_CASE("chart: attr lookup walks the subject's span and dead subjects miss") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_attr(c, ref(a), "doc", "first");
  build_attr(c, ref(a), "doc", "second");
  uint32_t const found{ chart_attr_find(c, ref(a), "doc") };
  REQUIRE(found != INVALID);
  // First row wins; a list is the rest of the span under the same key.
  CHECK(chart_string(c, c.attrs[found].value) == "first");
  c.states[a.v].live = 0;
  CHECK(chart_attr_find(c, ref(a), "doc") == INVALID);
}

TEST_CASE("chart: attr keys intern to one id per spelling") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_attr(c, ref(a), "libhsm:handler", "false");
  AttrKeyId const k{ chart_attr_key_find(c, "libhsm:handler") };
  REQUIRE(k.v != INVALID);
  CHECK(chart_attr_key(c, k) == "libhsm:handler");
  CHECK(chart_attr_key_find(c, "libhsm:missing").v == INVALID);
  CHECK(chart_attr_key(c, AttrKeyId{ 99 }).empty());
  CHECK(chart_attr_key(c, AttrKeyId{ INVALID }).empty());
}

TEST_CASE("path: a top-level state is its bare name") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const off{ build_state(c, root, "Off", StateKind::Normal, {}) };
  CHECK(path(c, off) == "Off");
}

TEST_CASE("path: a sole submachine earns no qualifier") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(c, on, "main", {}) };
  StateId const idle{ build_state(c, m, "Idle", StateKind::Normal, {}) };
  CHECK(path(c, idle) == "On/Idle");
}

TEST_CASE("path: a second submachine forces qualifiers, by name when named") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const main_sm{ build_submachine(c, on, "main", {}) };
  SubmachineId const aux{ build_submachine(c, on, "aux", {}) };
  StateId const idle{ build_state(c, main_sm, "Idle", StateKind::Normal, {}) };
  StateId const s_idle{ build_state(c, aux, "Idle", StateKind::Normal, {}) };
  CHECK(path(c, idle) == "On:main/Idle");
  CHECK(path(c, s_idle) == "On:aux/Idle");
}

TEST_CASE("path: an unnamed submachine qualifies by ordinal") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const m0{ build_submachine(c, on, {}, {}) };
  SubmachineId const m1{ build_submachine(c, on, {}, {}) };
  StateId const a{ build_state(c, m0, "Idle", StateKind::Normal, {}) };
  StateId const b{ build_state(c, m1, "Idle", StateKind::Normal, {}) };
  CHECK(path(c, a) == "On:0/Idle");
  CHECK(path(c, b) == "On:1/Idle");
}

TEST_CASE("path: qualifiers apply per level, not per chart") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const main_sm{ build_submachine(c, on, "main", {}) };
  SubmachineId const aux{ build_submachine(c, on, "aux", {}) };
  (void)aux;
  StateId const ready{ build_state(c, main_sm, "Ready", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, ready, {}, {}) };
  StateId const leaf{ build_state(c, inner, "Leaf", StateKind::Normal, {}) };
  // On is ambiguous (two submachines); Ready is not (one).
  CHECK(path(c, leaf) == "On:main/Ready/Leaf");
}

TEST_CASE("path: unnamed pseudostates spell as $kind with a stable ordinal") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const i0{ build_state(c, root, {}, StateKind::Initial, {}) };
  StateId const c0{ build_state(c, root, {}, StateKind::Choice, {}) };
  StateId const c1{ build_state(c, root, {}, StateKind::Choice, {}) };
  StateId const f0{ build_state(c, root, {}, StateKind::Final, {}) };
  CHECK(path(c, i0) == "$initial");
  CHECK(path(c, c0) == "$choice");
  CHECK(path(c, c1) == "$choice1");
  CHECK(path(c, f0) == "$final");

  // Ordinals count rows, not live rows: tombstoning one $choice must not
  // rename the other, or an address would dangle on delete.
  c.states[c0.v].live = 0;
  CHECK(path(c, c1) == "$choice1");
}

TEST_CASE("path: a named pseudostate is addressed by its name") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const pre{ build_state(c, root, "PreConfig", StateKind::Choice, {}) };
  CHECK(path(c, pre) == "PreConfig");
}

TEST_CASE("path: an out-of-range id appends nothing") {
  Chart c;
  build_chart(c, "c", {});
  std::string out{ "prefix" };
  chart_path_of(c, StateId{ 5 }, out);
  chart_path_of(c, StateId{ INVALID }, out);
  CHECK(out == "prefix");
}

TEST_CASE("chart: footprint counts what the arrays hold") {
  Chart c;
  CHECK(chart_footprint(c) == 0);
  SubmachineId const root{ build_chart(c, "chart", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  uint64_t const small{ chart_footprint(c) };
  CHECK(small > 0);
  for (uint32_t i = 0; i < 100; ++i) {
    build_state(c, root, "S", StateKind::Normal, "a label of some length");
  }
  CHECK(chart_footprint(c) > small);
}
