// Domain validation attributing failures to their requests, and the digest's
// sensitivity to every field.

#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <vector>

namespace {

using namespace scav;

// One submachine, two states, one transition -- enough rows for every table.
Chart two_state_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});
  return c;
}

struct Tables {
  std::vector<scav_box_space> box_state;
  std::vector<scav_box_space> box_sub;
  std::vector<scav_path_clear> path_clear;
  std::vector<scav_path_box> path_box;
};

Tables full_tables(Chart const &c) {
  return { .box_state = std::vector<scav_box_space>(c.states.size()),
           .box_sub = std::vector<scav_box_space>(c.submachines.size()),
           .path_clear = std::vector<scav_path_clear>(c.transitions.size()),
           .path_box = {} };
}

scav_spaces as_spaces(Tables const &t) {
  return { .box_state = t.box_state.data(),
           .n_box_state = static_cast<uint32_t>(t.box_state.size()),
           .box_sub = t.box_sub.data(),
           .n_box_sub = static_cast<uint32_t>(t.box_sub.size()),
           .path_clear = t.path_clear.data(),
           .n_path_clear = static_cast<uint32_t>(t.path_clear.size()),
           .path_box = t.path_box.data(),
           .n_path_box = static_cast<uint32_t>(t.path_box.size()) };
}

}  // namespace

TEST_CASE("spaces: empty and all-zero tables both validate") {
  Chart const c{ two_state_chart() };
  std::vector<Diagnostic> diags;

  scav_spaces const none{};
  CHECK(spaces_validate(c, none, diags));

  Tables const t{ full_tables(c) };
  CHECK(spaces_validate(c, as_spaces(t), diags));
  CHECK(diags.empty());
}

TEST_CASE("spaces: the domain boundary itself is legal") {
  Chart const c{ two_state_chart() };
  Tables t{ full_tables(c) };
  t.box_state[0] = { .min_w = SPACE_MAX, .h_before = SPACE_MAX, .h_after = SPACE_MAX };
  t.path_clear[0] = { .src = SPACE_MAX, .dst = SPACE_MAX };
  t.path_box = { { .subject = 0, .w = SPACE_MAX, .h = SPACE_MAX, .order = 0 } };

  std::vector<Diagnostic> diags;
  CHECK(spaces_validate(c, as_spaces(t), diags));
  CHECK(diags.empty());
}

TEST_CASE("spaces: a count neither zero nor the entity's is a mismatch") {
  Chart const c{ two_state_chart() };
  Tables t{ full_tables(c) };
  t.box_state.pop_back();

  std::vector<Diagnostic> diags;
  CHECK(!spaces_validate(c, as_spaces(t), diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::SpaceCountMismatch);
  CHECK(diags[0].subject.kind == ElemKind::Chart);
}

TEST_CASE("spaces: a field outside the quarter-domain names its requester") {
  Chart const c{ two_state_chart() };

  struct Bad {
    char const *what;
    void (*poke)(Tables &);
    ElemKind kind;
    uint32_t ordinal;
  };
  std::array const bads{
    Bad{ .what = "negative h_before",
         .poke = [](Tables &t) { t.box_state[1].h_before = -1; },
         .kind = ElemKind::State,
         .ordinal = 1 },
    Bad{ .what = "min_w past the cap",
         .poke = [](Tables &t) { t.box_state[0].min_w = SPACE_MAX + 1; },
         .kind = ElemKind::State,
         .ordinal = 0 },
    Bad{ .what = "submachine h_after",
         .poke = [](Tables &t) { t.box_sub[0].h_after = -5; },
         .kind = ElemKind::Submachine,
         .ordinal = 0 },
    Bad{ .what = "path clear src",
         .poke = [](Tables &t) { t.path_clear[0].src = SPACE_MAX + 1; },
         .kind = ElemKind::Transition,
         .ordinal = 0 },
  };
  for (Bad const &bad : bads) {
    CAPTURE(bad.what);
    Tables t{ full_tables(c) };
    bad.poke(t);
    std::vector<Diagnostic> diags;
    CHECK(!spaces_validate(c, as_spaces(t), diags));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::SpaceOutOfRange);
    CHECK(diags[0].subject.kind == bad.kind);
    CHECK(diags[0].subject.ordinal == bad.ordinal);
  }
}

TEST_CASE("spaces: path boxes demand a live subject and unique orders") {
  Chart const c{ two_state_chart() };
  Tables t{ full_tables(c) };
  t.path_box = { { .subject = 0, .w = 10, .h = 4, .order = 0 },
                 { .subject = 0, .w = 12, .h = 4, .order = 1 } };

  std::vector<Diagnostic> diags;
  CHECK(spaces_validate(c, as_spaces(t), diags));

  SUBCASE("a duplicated order is reported against the transition") {
    t.path_box[1].order = 0;
    CHECK(!spaces_validate(c, as_spaces(t), diags));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::SpaceOrderDuplicate);
    CHECK(diags[0].subject.kind == ElemKind::Transition);
    CHECK(diags[0].subject.ordinal == 0);
  }

  SUBCASE("a subject out of range is invalid") {
    t.path_box[1].subject = 7;
    CHECK(!spaces_validate(c, as_spaces(t), diags));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::SpaceSubjectInvalid);
  }

  SUBCASE("a tombstoned subject is invalid") {
    Chart dead{ two_state_chart() };
    dead.transitions[0].live = 0;
    CHECK(!spaces_validate(dead, as_spaces(t), diags));
    CHECK(diags.size() == 2);  // both boxes name the dead transition
    CHECK(diags[0].code == DiagCode::SpaceSubjectInvalid);
  }

  SUBCASE("a box past the domain reports out of range") {
    t.path_box[0].w = SPACE_MAX + 1;
    CHECK(!spaces_validate(c, as_spaces(t), diags));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::SpaceOutOfRange);
  }
}

TEST_CASE("spaces: findings sort by code, kind, ordinal") {
  Chart const c{ two_state_chart() };
  Tables t{ full_tables(c) };
  // Poked in reverse of the expected report order.
  t.path_clear[0].dst = -1;
  t.box_state[1].min_w = -1;
  t.box_state[0].min_w = -1;

  std::vector<Diagnostic> diags;
  CHECK(!spaces_validate(c, as_spaces(t), diags));
  REQUIRE(diags.size() == 3);
  CHECK(diags[0].subject.kind == ElemKind::State);
  CHECK(diags[0].subject.ordinal == 0);
  CHECK(diags[1].subject.ordinal == 1);
  CHECK(diags[2].subject.kind == ElemKind::Transition);
}

TEST_CASE("spaces: the digest hears every field and both zero shapes differ") {
  Chart const c{ two_state_chart() };
  Tables t{ full_tables(c) };
  t.path_box = { { .subject = 0, .w = 10, .h = 4, .order = 0 } };

  uint32_t const base{ spaces_digest(as_spaces(t)) };
  CHECK(base == spaces_digest(as_spaces(t)));

  Tables poked{ t };
  poked.box_state[1].h_after = 1;
  CHECK(spaces_digest(as_spaces(poked)) != base);
  poked = t;
  poked.path_box[0].order = 1;
  CHECK(spaces_digest(as_spaces(poked)) != base);
  poked = t;
  poked.path_clear[0].dst = 8;
  CHECK(spaces_digest(as_spaces(poked)) != base);

  // No requests at all and zero-valued requests are different policies.
  scav_spaces const none{};
  CHECK(spaces_digest(none) != spaces_digest(as_spaces(full_tables(c))));

  // Count prefixes keep adjacent tables apart: the same 12 bytes hash
  // differently as one state row versus one submachine row.
  scav_box_space const row{ .min_w = 3, .h_before = 5, .h_after = 7 };
  scav_spaces const as_state{ .box_state = &row, .n_box_state = 1 };
  scav_spaces const as_sub{ .box_sub = &row, .n_box_sub = 1 };
  CHECK(spaces_digest(as_state) != spaces_digest(as_sub));
}
