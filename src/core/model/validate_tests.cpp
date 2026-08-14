// Structural validation: every §10 check fires on a crafted violation and
// stays silent on a well-built chart. Violations are direct row pokes -- the
// builder refuses to construct them, which is the point of having both.

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
    InstId const inc{ build_include(r.c, r.root, "A") };
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
  CHECK(diags[0].subject.ordinal == col.v);
}

TEST_CASE("validate: findings arrive in (code, subject kind, ordinal) order") {
  Built r{ built() };
  // Three violations whose discovery order differs from the artifact order:
  // a metachar name (late code, low ordinal walks first), a dangling parent
  // (early code), and a tombstoned target (middle code).
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
  // The NSDMI is what lets every P0 producer stay untouched: a designated
  // initializer that skips .subject gets None + INVALID, not zeroes.
  Diagnostic const d{ .code = DiagCode::ExpectedChart, .doc = { 0 }, .src = {} };
  CHECK(d.subject.kind == ElemKind::None);
  CHECK(d.subject.ordinal == INVALID);
}
