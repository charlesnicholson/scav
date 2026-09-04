// Every check fires on a crafted violation and stays silent on a well-built
// chart. Violations are direct row pokes; the builder refuses to make them.

#include "core/model/model.h"
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

struct Built {
  Chart c;
  SubmachineId root{ INVALID };
  StateId a{ INVALID }, b{ INVALID };
  TransId t{ INVALID };
};

// A minimal correct chart every test corrupts its own copy of.
Built built() {
  Built r;
  r.root = build_chart(r.c, "c", {});
  r.a = build_state(r.c, r.root, "A", StateKind::Normal, {});
  r.b = build_state(r.c, r.root, "B", StateKind::Normal, {});
  r.t = build_trans(r.c, r.a, r.b, TransKind::External, "go");
  build_attr(r.c, ref(r.a), "doc", "state A");
  return r;
}

std::vector<Diagnostic> run(Chart const &c) {
  std::vector<Diagnostic> diags;
  validate_chart(c, diags);
  return diags;
}

}  // namespace

TEST_CASE("validate: a well-built chart is clean") {
  Built r{ built() };
  std::vector<Diagnostic> diags;
  CHECK(validate_chart(r.c, diags));
  CHECK(diags.empty());
}

TEST_CASE("validate: an empty Chart has no root and says so") {
  Chart const c;
  std::vector<Diagnostic> const diags{ run(c) };
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::MissingRequiredId);
  CHECK(diags[0].subject.kind == ElemKind::Chart);
}

TEST_CASE("validate: a dangling parent is caught with the state as subject") {
  Built r{ built() };
  r.c.states[r.a.v].parent = SubmachineId{ 99 };
  std::vector<Diagnostic> const diags{ run(r.c) };
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::DanglingRef);
  CHECK(diags[0].subject == ref(r.a));
}

TEST_CASE("validate: a live row under a tombstoned parent is caught") {
  Built r{ built() };
  StateId const on{ build_state(r.c, r.root, "On", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(r.c, on, "m", {}) };
  StateId const idle{ build_state(r.c, m, "Idle", StateKind::Normal, {}) };
  r.c.submachines[m.v].live = 0;
  std::vector<Diagnostic> const diags{ run(r.c) };
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::TombstonedTarget);
  CHECK(diags[0].subject == ref(idle));
}

TEST_CASE("validate: transition endpoints are required, in range, and live") {
  Built r{ built() };
  SUBCASE("missing") {
    r.c.transitions[r.t.v].src = StateId{ INVALID };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::MissingRequiredId);
    CHECK(diags[0].subject == ref(r.t));
  }
  SUBCASE("dangling") {
    r.c.transitions[r.t.v].dst = StateId{ 1000 };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DanglingRef);
  }
  SUBCASE("tombstoned") {
    r.c.states[r.b.v].live = 0;
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::TombstonedTarget);
    CHECK(diags[0].subject == ref(r.t));
  }
  SUBCASE("a tombstoned transition is not checked") {
    r.c.transitions[r.t.v].dst = StateId{ 1000 };
    r.c.transitions[r.t.v].live = 0;
    CHECK(run(r.c).empty());
  }
}

TEST_CASE("validate: duplicate names within one submachine, aliases included") {
  Built r{ built() };
  SUBCASE("two authored states") {
    StateId const dup{ build_state(r.c, r.root, "A", StateKind::Normal, {}) };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DuplicateName);
    // The later ordinal is the duplicate; the first keeps its name.
    CHECK(diags[0].subject == ref(dup));
  }
  SUBCASE("an alias host collides like any state") {
    InstId const inc{ build_include(r.c, r.root, "A", "a.scav") };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DuplicateName);
    CHECK(diags[0].subject == ref(r.c.includes[inc.v].host));
  }
  SUBCASE("same name in different submachines is fine") {
    StateId const on{ build_state(r.c, r.root, "On", StateKind::Normal, {}) };
    SubmachineId const m{ build_submachine(r.c, on, "m", {}) };
    build_state(r.c, m, "A", StateKind::Normal, {});
    CHECK(run(r.c).empty());
  }
  SUBCASE("a tombstoned holder frees its name") {
    r.c.states[r.a.v].live = 0;
    // Rebuild r.t to a live endpoint first, or the tombstone check fires too.
    r.c.transitions[r.t.v].src = r.b;
    build_state(r.c, r.root, "A", StateKind::Normal, {});
    CHECK(run(r.c).empty());
  }
}

TEST_CASE("validate: more than one initial per submachine") {
  Built r{ built() };
  build_state(r.c, r.root, {}, StateKind::Initial, {});
  StateId const extra{ build_state(r.c, r.root, {}, StateKind::Initial, {}) };
  std::vector<Diagnostic> const diags{ run(r.c) };
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::MultipleInitial);
  // The second and later initials are the findings; the first is legitimate.
  CHECK(diags[0].subject == ref(extra));
}

TEST_CASE("validate: path metacharacters in authored names") {
  for (char const ch : { '/', ':', '$', '@' }) {
    Built r{ built() };
    std::string name{ "Bad" };
    name.push_back(ch);
    StateId const s{ build_state(r.c, r.root, name, StateKind::Normal, {}) };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::NameHasMetacharacter);
    CHECK(diags[0].subject == ref(s));
  }
}

TEST_CASE("validate: a broken span is a dangling ref on its owner") {
  Built r{ built() };
  SUBCASE("children span past the array") {
    r.c.submachines[r.root.v].children.len += 10;
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(!diags.empty());
    CHECK(diags[0].code == DiagCode::DanglingRef);
    CHECK(diags[0].subject == ref(r.root));
  }
  SUBCASE("attrs span past the array") {
    r.c.states[r.a.v].attrs.len += 5;
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DanglingRef);
    CHECK(diags[0].subject == ref(r.a));
  }
  SUBCASE("an attr row with an unknown key") {
    Span const span{ chart_attrs_of(r.c, ref(r.a)) };
    r.c.attrs[span.off].key = AttrKeyId{ 42 };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DanglingRef);
  }
}

TEST_CASE("validate: provenance ordinals must land in their arrays") {
  Built r{ built() };
  SUBCASE("stmt") {
    r.c.states[r.a.v].stmt = StmtId{ 7 };  // no statements exist
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DanglingRef);
  }
  SUBCASE("inst") {
    r.c.transitions[r.t.v].inst = InstId{ 3 };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DanglingRef);
  }
}

TEST_CASE("validate: column counts must match their entity array") {
  Built r{ built() };
  ColumnId const col{
    column_register(r.c, "events", ElemKind::State, ValueKind::U32, 4, 4, 0)
  };
  REQUIRE(col.v != INVALID);
  CHECK(run(r.c).empty());
  r.c.columns[col.v].bytes.resize(4);  // two states, one row
  std::vector<Diagnostic> const diags{ run(r.c) };
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::ColumnCountMismatch);
  // A column has no ElemKind, so the subject is the array the column failed to
  // cover, spelled with the INVALID ordinal that names a kind and no row.
  CHECK(diags[0].subject.kind == ElemKind::State);
  CHECK(diags[0].subject.ordinal == INVALID);
  CHECK_FALSE(chart_ref_valid(r.c, diags[0].subject));

  // The renderer has no statement to walk to, so it falls back to the name the
  // caller supplied rather than indexing a row that is not there.
  std::string out;
  diag_append(out, r.c, diags[0], "cmdline.scav");
  CHECK(out == std::string{ "cmdline.scav: " } +
                   diag_message(DiagCode::ColumnCountMismatch) + "\n");
}

TEST_CASE("validate: an include names its host state, or names nothing") {
  Built r{ built() };
  InstId const inc{ build_include(r.c, r.root, "sub", "sub.scav") };
  REQUIRE(inc.v != INVALID);
  CHECK(run(r.c).empty());

  SUBCASE("a host that exists is the subject") {
    r.c.includes[inc.v].alias = {};
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::MissingRequiredId);
    CHECK(diags[0].subject == ref(r.c.includes[inc.v].host));
  }
  SUBCASE("an absent host leaves the finding subjectless") {
    r.c.includes[inc.v].host = StateId{ INVALID };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::MissingRequiredId);
    CHECK(diags[0].subject.kind == ElemKind::None);
    CHECK(diags[0].subject.ordinal == INVALID);
  }
  SUBCASE("an out-of-range host does too") {
    r.c.includes[inc.v].host = StateId{ 99 };
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::DanglingRef);
    CHECK(diags[0].subject.kind == ElemKind::None);
    CHECK(diags[0].subject.ordinal == INVALID);
    std::string out;
    diag_append(out, r.c, diags[0], "cmdline.scav");
    CHECK(out ==
          std::string{ "cmdline.scav: " } + diag_message(DiagCode::DanglingRef) + "\n");
  }
}

// Containment ===============================================================

TEST_CASE("validate: both producers of the containment spans are accepted") {
  // The builder writes the spans one insertion at a time; finalize rebuilds
  // them all at once. The check must accept what either leaves behind.
  Built r{ built() };
  StateId const on{ build_state(r.c, r.root, "On", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(r.c, on, "m", {}) };
  StateId const idle{ build_state(r.c, m, "Idle", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(r.c, idle, {}, {}) };
  build_state(r.c, inner, "Deep", StateKind::Normal, {});
  build_submachine(r.c, on, "aux", {});
  CHECK(run(r.c).empty());
  model_finalize_containment(r.c);
  CHECK(run(r.c).empty());
}

TEST_CASE("validate: a state sits in its parent's children exactly once") {
  Built r{ built() };
  SUBCASE("absent") {
    r.c.submachines[r.root.v].children.len -= 1;  // drops B
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
    CHECK(diags[0].subject == ref(r.b));
  }
  SUBCASE("twice") {
    // Nameless and not an initial, so the duplicate entry trips neither the
    // duplicate-name check nor the multiple-initial one.
    StateId const anon{ build_state(r.c, r.root, {}, StateKind::Choice, {}) };
    r.c.state_ids.push_back(anon);
    r.c.submachines[r.root.v].children.len += 1;
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
    CHECK(diags[0].subject == ref(anon));
  }
}

TEST_CASE("validate: a children entry points back at the submachine holding it") {
  Built r{ built() };
  StateId const on{ build_state(r.c, r.root, "On", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(r.c, on, "m", {}) };
  // A is listed under m as well as under its own parent, so A's side of the
  // relation still agrees and only m disagrees.
  r.c.submachines[m.v].children =
      make_span(static_cast<uint32_t>(r.c.state_ids.size()), 1);
  r.c.state_ids.push_back(r.a);
  std::vector<Diagnostic> const diags{ run(r.c) };
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
  CHECK(diags[0].subject == ref(m));
}

TEST_CASE("validate: a submachine sits in its owner's span exactly once") {
  Built r{ built() };
  StateId const on{ build_state(r.c, r.root, "On", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(r.c, on, "m", {}) };
  SUBCASE("absent") {
    r.c.states[on.v].submachines.len = 0;
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
    CHECK(diags[0].subject == ref(m));
  }
  SUBCASE("twice") {
    r.c.submachine_ids.push_back(m);
    r.c.states[on.v].submachines.len += 1;
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
    CHECK(diags[0].subject == ref(m));
  }
}

TEST_CASE("validate: a submachines entry points back at the state holding it") {
  Built r{ built() };
  StateId const on{ build_state(r.c, r.root, "On", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(r.c, on, "m", {}) };
  r.c.states[r.a.v].submachines =
      make_span(static_cast<uint32_t>(r.c.submachine_ids.size()), 1);
  SUBCASE("a submachine another state owns") {
    r.c.submachine_ids.push_back(m);
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
    CHECK(diags[0].subject == ref(r.a));
  }
  SUBCASE("a document root, which no state owns") {
    r.c.submachine_ids.push_back(r.root);
    std::vector<Diagnostic> const diags{ run(r.c) };
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
    CHECK(diags[0].subject == ref(r.a));
  }
}

TEST_CASE("validate: only the chart's own root submachine may be ownerless") {
  Built r{ built() };
  // Appended rather than built: the builder has no way to make a second root.
  r.c.submachines.push_back({ .owner = { INVALID },
                              .ordinal = 0,
                              .name = {},
                              .label = {},
                              .children = {},
                              .attrs = {},
                              .stmt = { INVALID },
                              .inst = { INVALID },
                              .live = 1 });
  std::vector<Diagnostic> const diags{ run(r.c) };
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
  CHECK(diags[0].subject == ref(SubmachineId{ 1 }));
}

TEST_CASE("validate: a containment cycle is reported and the walk terminates") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  SubmachineId const ma{ build_submachine(c, a, "ma", {}) };
  StateId const b{ build_state(c, ma, "B", StateKind::Normal, {}) };
  SubmachineId const mb{ build_submachine(c, b, "mb", {}) };

  // A moves under its own grandchild's submachine, spans and all, so the climb
  // A -> mb -> B -> ma -> A closes and the cycle is the only disagreement.
  c.states[a.v].parent = mb;
  c.submachines[root.v].children = make_span(0, 0);
  c.submachines[mb.v].children = make_span(0, 1);  // state_ids[0] is A

  std::vector<Diagnostic> const diags{ run(c) };
  REQUIRE(diags.size() == 2);
  // Both states are on the cycle, so neither of them reaches a document root.
  CHECK(diags[0].code == DiagCode::ContainmentInconsistent);
  CHECK(diags[0].subject == ref(a));
  CHECK(diags[1].code == DiagCode::ContainmentInconsistent);
  CHECK(diags[1].subject == ref(b));
}

TEST_CASE("validate: a broken containment ordinal is one finding, not two") {
  // The dangling parent is already a DanglingRef, so the containment check
  // stays silent about the same ordinal rather than piling on.
  Built r{ built() };
  r.c.states[r.a.v].parent = SubmachineId{ 99 };
  std::vector<Diagnostic> const diags{ run(r.c) };
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::DanglingRef);
}

TEST_CASE("validate: findings arrive in (code, subject kind, ordinal) order") {
  Built r{ built() };
  // Three violations whose discovery order differs from the sorted order: a
  // metachar name, a dangling parent, and a tombstoned target.
  r.c.states[r.a.v].parent = SubmachineId{ 50 };  // DanglingRef
  r.c.states[r.b.v].live = 0;                     // t -> TombstonedTarget
  StateId const bad{ build_state(r.c, r.root, "x$y", StateKind::Normal, {}) };
  std::vector<Diagnostic> const diags{ run(r.c) };
  REQUIRE(diags.size() == 3);
  CHECK(diags[0].code == DiagCode::DanglingRef);
  CHECK(diags[0].subject == ref(r.a));
  CHECK(diags[1].code == DiagCode::TombstonedTarget);
  CHECK(diags[1].subject == ref(r.t));
  CHECK(diags[2].code == DiagCode::NameHasMetacharacter);
  CHECK(diags[2].subject == ref(bad));
}

TEST_CASE("validate: appends to the caller's vector without clearing it") {
  Built r{ built() };
  r.c.states[r.a.v].parent = SubmachineId{ 50 };
  std::vector<Diagnostic> diags;
  diags.push_back({ .code = DiagCode::Ok, .doc = {}, .src = {} });
  CHECK_FALSE(validate_chart(r.c, diags));
  REQUIRE(diags.size() == 2);
  CHECK(diags[0].code == DiagCode::Ok);
  CHECK(diags[1].code == DiagCode::DanglingRef);
}

TEST_CASE("validate: pre-model diagnostics default to a None subject") {
  // The NSDMI is what lets a producer leave the subject out entirely: a designated
  // initializer that skips .subject gets None + INVALID, not zeroes.
  Diagnostic const d{ .code = DiagCode::ExpectedChart, .doc = { 0 }, .src = {} };
  CHECK(d.subject.kind == ElemKind::None);
  CHECK(d.subject.ordinal == INVALID);
}
