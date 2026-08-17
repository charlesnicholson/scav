// Rows land where their ids say, spans stay contiguous under out-of-order
// appends, and a bad call refuses.

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

// Every id and span in the chart lands inside the array it names.
void check_refs_resolve(Chart const &c) {
  for (State const &s : c.states) {
    CHECK(s.parent.v < c.submachines.size());
    CHECK(static_cast<size_t>(s.submachines.off) + s.submachines.len <=
          c.submachine_ids.size());
    CHECK(static_cast<size_t>(s.attrs.off) + s.attrs.len <= c.attrs.size());
  }
  for (Submachine const &m : c.submachines) {
    CHECK(((m.owner.v == INVALID) || (m.owner.v < c.states.size())));
    CHECK(static_cast<size_t>(m.children.off) + m.children.len <= c.state_ids.size());
    CHECK(static_cast<size_t>(m.attrs.off) + m.attrs.len <= c.attrs.size());
  }
  for (Transition const &t : c.transitions) {
    CHECK(t.src.v < c.states.size());
    CHECK(t.dst.v < c.states.size());
    CHECK(static_cast<size_t>(t.attrs.off) + t.attrs.len <= c.attrs.size());
  }
  for (Include const &inc : c.includes) { CHECK(inc.host.v < c.states.size()); }
  for (StateId const id : c.state_ids) { CHECK(id.v < c.states.size()); }
  for (SubmachineId const id : c.submachine_ids) { CHECK(id.v < c.submachines.size()); }
  for (Attr const &a : c.attrs) { CHECK(a.key.v < c.attr_key_names.size()); }
  CHECK(static_cast<size_t>(c.chart_attrs.off) + c.chart_attrs.len <= c.attrs.size());
}

// The states a submachine's children span yields, liveness-checked the way
// every consumer must walk it.
std::vector<StateId> live_children(Chart const &c, SubmachineId id) {
  std::vector<StateId> out;
  Span const kids{ c.submachines[id.v].children };
  for (uint32_t i = 0; i < kids.len; ++i) {
    StateId const s{ c.state_ids[kids.off + i] };
    if (c.states[s.v].live != 0) { out.push_back(s); }
  }
  return out;
}

}  // namespace

TEST_CASE("build: build_chart names the chart and returns the root submachine") {
  Chart c;
  SubmachineId const root{ build_chart(c, "vac", "robot vacuum") };
  REQUIRE(root.v == 0);
  CHECK(chart_string(c, c.name) == "vac");
  CHECK(chart_string(c, c.label) == "robot vacuum");
  CHECK(c.root_submachine == root);
  CHECK(c.submachines[0].owner.v == INVALID);
  CHECK(c.submachines[0].ordinal == 0);
  CHECK(c.submachines[0].live == 1);
  // The root is the chart's implicit submachine, so it is unnamed.
  CHECK(c.submachines[0].name.len == 0);
}

TEST_CASE("build: a second build_chart is refused") {
  Chart c;
  REQUIRE(build_chart(c, "one", {}).v == 0);
  CHECK(build_chart(c, "two", {}).v == INVALID);
  CHECK(chart_string(c, c.name) == "one");
  CHECK(c.submachines.size() == 1);
}

TEST_CASE("build: states land under their parent in append order") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const off{ build_state(c, root, "Off", StateKind::Normal, "powered down") };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  REQUIRE(off.v == 0);
  REQUIRE(on.v == 1);
  CHECK(c.states[off.v].parent == root);
  CHECK(chart_string(c, c.states[off.v].name) == "Off");
  CHECK(chart_string(c, c.states[off.v].label) == "powered down");
  CHECK(c.states[on.v].label.len == 0);
  CHECK(live_children(c, root) == std::vector<StateId>{ off, on });
  CHECK(c.states[off.v].stmt.v == INVALID);  // code-built: no statement
  CHECK(c.states[off.v].inst.v == INVALID);
  check_refs_resolve(c);
}

TEST_CASE("build: a state under a bad or dead parent is refused") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  CHECK(build_state(c, SubmachineId{ 99 }, "X", StateKind::Normal, {}).v == INVALID);
  CHECK(build_state(c, SubmachineId{ INVALID }, "X", StateKind::Normal, {}).v == INVALID);
  c.submachines[root.v].live = 0;
  CHECK(build_state(c, root, "X", StateKind::Normal, {}).v == INVALID);
  CHECK(c.states.empty());
}

TEST_CASE("build: submachines attach to their owner in order") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const on{ build_state(c, root, "On", StateKind::Normal, {}) };
  SubmachineId const main_sm{ build_submachine(c, on, "main", {}) };
  SubmachineId const aux{ build_submachine(c, on, "aux", "sweeps while main drives") };
  REQUIRE(main_sm.v == 1);  // 0 is the root
  REQUIRE(aux.v == 2);
  CHECK(c.submachines[main_sm.v].owner == on);
  CHECK(c.submachines[main_sm.v].ordinal == 0);
  CHECK(c.submachines[aux.v].ordinal == 1);
  Span const subs{ c.states[on.v].submachines };
  REQUIRE(subs.len == 2);
  CHECK(c.submachine_ids[subs.off] == main_sm);
  CHECK(c.submachine_ids[subs.off + 1] == aux);
  CHECK(build_submachine(c, StateId{ 42 }, "bad", {}).v == INVALID);
  check_refs_resolve(c);
}

TEST_CASE("build: appending inside a shared array rebuilds it and every span") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(c, a, "m", {}) };
  StateId const b{ build_state(c, m, "B", StateKind::Normal, {}) };
  // Root's children span is no longer at the tail of state_ids, so this append
  // shifts B's slot right and bumps m's span offset.
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  CHECK(live_children(c, root) == std::vector<StateId>{ a, d });
  CHECK(live_children(c, m) == std::vector<StateId>{ b });
  check_refs_resolve(c);

  // And the other direction: grow the nested one after the outer one moved it.
  StateId const e{ build_state(c, m, "E", StateKind::Normal, {}) };
  CHECK(live_children(c, m) == std::vector<StateId>{ b, e });
  CHECK(live_children(c, root) == std::vector<StateId>{ a, d });
  check_refs_resolve(c);
}

TEST_CASE("build: transitions take live endpoints only") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  TransId const t{ build_trans(c, a, b, TransKind::External, "go") };
  REQUIRE(t.v == 0);
  CHECK(c.transitions[t.v].src == a);
  CHECK(c.transitions[t.v].dst == b);
  CHECK(chart_string(c, c.transitions[t.v].label) == "go");

  CHECK(build_trans(c, a, StateId{ 77 }, TransKind::External, {}).v == INVALID);
  c.states[b.v].live = 0;
  CHECK(build_trans(c, a, b, TransKind::External, {}).v == INVALID);
  CHECK(c.transitions.size() == 1);
  check_refs_resolve(c);
}

TEST_CASE("build: a self-transition is ordinary") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "Ready", StateKind::Normal, {}) };
  TransId const t{ build_trans(c, a, a, TransKind::Internal, "RETRY") };
  CHECK(c.transitions[t.v].src == c.transitions[t.v].dst);
  CHECK(c.transitions[t.v].kind == TransKind::Internal);
}

TEST_CASE("build: attrs intern their keys and attach to any live subject") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  TransId const t{ build_trans(c, a, a, TransKind::External, {}) };
  SubmachineId const m{ build_submachine(c, a, "m", {}) };

  CHECK(build_attr(c, ref(a), "doc", "state A") != INVALID);
  CHECK(build_attr(c, chart_ref(), "doc", "the chart") != INVALID);
  CHECK(build_attr(c, ref(t), "libhsm:handler", "false") != INVALID);
  CHECK(build_attr(c, ref(m), "doc", "the submachine") != INVALID);

  // One spelling, one id: three subjects used "doc" and interned one key.
  CHECK(c.attr_key_names.size() == 2);
  AttrKeyId const doc{ chart_attr_key_find(c, "doc") };
  REQUIRE(doc.v != INVALID);
  CHECK(chart_attr_key(c, doc) == "doc");

  uint32_t const found{ chart_attr_find(c, ref(a), "doc") };
  REQUIRE(found != INVALID);
  CHECK(chart_string(c, c.attrs[found].value) == "state A");
  CHECK(chart_attr_find(c, chart_ref(), "doc") != INVALID);
  CHECK(chart_attr_find(c, ref(a), "missing") == INVALID);
  check_refs_resolve(c);
}

TEST_CASE("build: a repeated key appends rows -- that is how a list is stored") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  CHECK(build_attr(c, ref(a), "tags", "x") != INVALID);
  CHECK(build_attr(c, ref(a), "tags", "y") != INVALID);
  Span const span{ chart_attrs_of(c, ref(a)) };
  REQUIRE(span.len == 2);
  CHECK(c.attrs[span.off].key == c.attrs[span.off + 1].key);
  CHECK(chart_string(c, c.attrs[span.off].value) == "x");
  CHECK(chart_string(c, c.attrs[span.off + 1].value) == "y");
}

TEST_CASE("build: interleaved attrs keep every subject's span contiguous") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  CHECK(build_attr(c, ref(a), "k1", "a1") != INVALID);
  CHECK(build_attr(c, ref(b), "k2", "b1") != INVALID);
  // Lands inside the array, not at its tail: A's span grows in place and B's
  // shifts right.
  CHECK(build_attr(c, ref(a), "k3", "a2") != INVALID);
  CHECK(build_attr(c, chart_ref(), "k4", "c1") != INVALID);

  uint32_t const a2{ chart_attr_find(c, ref(a), "k3") };
  REQUIRE(a2 != INVALID);
  CHECK(chart_string(c, c.attrs[a2].value) == "a2");
  uint32_t const b1{ chart_attr_find(c, ref(b), "k2") };
  REQUIRE(b1 != INVALID);
  CHECK(chart_string(c, c.attrs[b1].value) == "b1");
  uint32_t const c1{ chart_attr_find(c, chart_ref(), "k4") };
  REQUIRE(c1 != INVALID);
  CHECK(chart_string(c, c.attrs[c1].value) == "c1");
  CHECK(chart_attrs_of(c, ref(a)).len == 2);
  CHECK(chart_attrs_of(c, ref(b)).len == 1);
  check_refs_resolve(c);
}

TEST_CASE("build: attrs refuse an empty key, a dead subject, and a non-subject") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  CHECK(build_attr(c, ref(a), "", "v") == INVALID);
  c.states[a.v].live = 0;
  CHECK(build_attr(c, ref(a), "k", "v") == INVALID);
  CHECK(build_attr(c, ElemRef{ ElemKind::Point, 0 }, "k", "v") == INVALID);
  CHECK(build_attr(c, ElemRef{ ElemKind::State, 99 }, "k", "v") == INVALID);
  CHECK(c.attrs.empty());
}

TEST_CASE("build: an include synthesizes its alias host state") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  InstId const inc{ build_include(c, root, "wifi", "net/wifi.scav") };
  REQUIRE(inc.v == 0);
  Include const &row{ c.includes[inc.v] };
  CHECK(chart_string(c, row.alias) == "wifi");
  CHECK(chart_string(c, row.path) == "net/wifi.scav");
  CHECK(row.target.v == INVALID);  // the loader's to fill
  CHECK(row.stmt.v == INVALID);
  REQUIRE(row.host.v < c.states.size());
  State const &host{ c.states[row.host.v] };
  CHECK(chart_string(c, host.name) == "wifi");
  CHECK(host.parent == root);
  CHECK(host.kind == StateKind::Normal);
  // The host is an ordinary state, so it is an ordinary path: `wifi` is a
  // state.
  CHECK(path(c, row.host) == "wifi");
  check_refs_resolve(c);
}

TEST_CASE("build: an include refuses an empty alias, an empty path, a bad parent") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  CHECK(build_include(c, root, "", "w.scav").v == INVALID);
  CHECK(build_include(c, root, "w", "").v == INVALID);
  CHECK(build_include(c, SubmachineId{ 9 }, "w", "w.scav").v == INVALID);
  CHECK(c.includes.empty());
  CHECK(c.states.empty());
}

TEST_CASE("build: an include's alias interns once, not once per row") {
  // The pool never deduplicates, so a second add of the same bytes is a second
  // copy. The host state and the Include row are the same name and share one.
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  InstId const inc{ build_include(c, root, "wifi", "wifi.scav") };
  REQUIRE(inc.v != INVALID);
  CHECK(c.includes[inc.v].alias == c.states[c.includes[inc.v].host.v].name);
}

TEST_CASE("build: a walk skips tombstoned rows and never renumbers live ones") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  c.states[b.v].live = 0;  // no delete API; a test pokes the flag
  CHECK(live_children(c, root) == std::vector<StateId>{ a, d });
  // The dead row keeps its slot in the span -- compaction would invalidate
  // every other span into the array.
  CHECK(c.submachines[root.v].children.len == 3);
  CHECK(chart_live(c, ref(b)) == false);
  CHECK(chart_live(c, ref(a)) == true);
  CHECK(a.v == 0);
  CHECK(d.v == 2);
}

TEST_CASE("build: a depth-16 chain builds and walks back") {
  Chart c;
  SubmachineId sm{ build_chart(c, "deep", {}) };
  StateId leaf{ INVALID };
  for (uint32_t level = 0; level < 16; ++level) {
    leaf = build_state(c, sm, "S", StateKind::Normal, {});
    REQUIRE(leaf.v != INVALID);
    sm = build_submachine(c, leaf, {}, {});
    REQUIRE(sm.v != INVALID);
  }
  // Walk back up to the root by ordinals alone.
  uint32_t depth{ 0 };
  StateId cur{ leaf };
  while (true) {
    SubmachineId const parent{ c.states[cur.v].parent };
    ++depth;
    StateId const owner{ c.submachines[parent.v].owner };
    if (owner.v == INVALID) { break; }
    cur = owner;
  }
  CHECK(depth == 16);
  CHECK(path(c, leaf) == "S/S/S/S/S/S/S/S/S/S/S/S/S/S/S/S");
  check_refs_resolve(c);
}
