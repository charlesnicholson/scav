// The C projection of scav_layout.h: what each entry point does with an
// argument it cannot use, and the out-param protocol around the placed boxes.
// What layout does with a chart it can use is layout_tests.cpp's.

#include "scav/scav_layout_c.h"

#include "scav/scav_core.h"
#include "scav/scav_core_c.h"
#include "scav/scav_layout.h"
#include "scav/scav_types.h"

#include "scav_c_handles.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {

using namespace scav;

// What a refused call must leave an out-param holding.
constexpr uint32_t SENTINEL{ 0xD1CE'D1CEU };
constexpr int32_t SENTINEL_I{ 0x5CA5 };

// The sizes the C surface checks against, as this build measures them.
constexpr uint32_t PROFILE_SIZE{ static_cast<uint32_t>(sizeof(scav_profile)) };
constexpr uint32_t OPTS_SIZE{ static_cast<uint32_t>(sizeof(scav_layout_opts)) };
constexpr uint32_t SPACES_SIZE{ static_cast<uint32_t>(sizeof(scav_spaces)) };
constexpr uint32_t PLACED_SIZE{ static_cast<uint32_t>(sizeof(scav_placed)) };

// A space table with every stride declared, which is what layout_run checks
// before it reads a row: the tests below vary the pointers and counts.
scav_spaces strided(scav_spaces s) {
  s.box_state_stride = static_cast<uint32_t>(sizeof(scav_box_space));
  s.box_sub_stride = static_cast<uint32_t>(sizeof(scav_box_space));
  s.path_clear_stride = static_cast<uint32_t>(sizeof(scav_path_clear));
  s.path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box));
  return s;
}

// Two states in one submachine and two transitions between them, so two path
// boxes can be requested and a cap can be one short of them.
Chart two_transitions() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const idle{ build_state(c, root, "Idle", StateKind::Normal, {}) };
  StateId const busy{ build_state(c, root, "Busy", StateKind::Normal, {}) };
  build_trans(c, idle, busy, TransKind::External, "go");
  build_trans(c, busy, idle, TransKind::External, "stop");
  return c;
}

scav_layout_opts readable() {
  scav_layout_opts o{ .profile = {}, .router = 0, .threads = 0 };
  REQUIRE(profile_named("readable", o.profile));
  return o;
}

// The run every case below is a variation on: every size correct, so a case
// that spoils one spoils only that one.
scav_result run(scav_chart *chart,
                scav_spaces const *spaces,
                scav_layout_opts const *opts,
                scav_placed *placed,
                uint32_t cap,
                uint32_t *out_count) {
  return scav_layout_run(chart,
                         spaces,
                         SPACES_SIZE,
                         opts,
                         OPTS_SIZE,
                         placed,
                         cap,
                         PLACED_SIZE,
                         out_count);
}

}  // namespace

TEST_CASE("layout abi: a null argument is refused whichever one it is") {
  scav_chart chart{ .chart = two_transitions(), .diags = {} };
  scav_layout_opts const opts{ readable() };
  scav_profile profile{ .profile_id = SENTINEL_I };

  scav_byte const guard{ 0x5C };
  scav_byte const *bytes{ &guard };
  uint32_t count{ SENTINEL };
  scav_router_id router{ SENTINEL };
  scav_placed placed{ .x = SENTINEL_I, .y = SENTINEL_I, .w = SENTINEL_I, .h = SENTINEL_I };

  struct Case {
    char const *what;
    scav_result got;
    scav_result want;
  };
  std::vector<Case> const cases{
    { .what = "profile_named: no name",
      .got = scav_profile_named(nullptr, &profile, PROFILE_SIZE),
      .want = SCAV_E_INVALID_ARG },
    { .what = "profile_named: nowhere to put it",
      .got = scav_profile_named("readable", nullptr, PROFILE_SIZE),
      .want = SCAV_E_INVALID_ARG },
    { .what = "profile_named: no such profile",
      .got = scav_profile_named("no-such-profile", &profile, PROFILE_SIZE),
      .want = SCAV_E_INVALID_ARG },
    { .what = "profile_validate: no profile",
      .got = scav_profile_validate(nullptr, PROFILE_SIZE),
      .want = SCAV_E_INVALID_ARG },
    { .what = "layout_run: no chart",
      .got = run(nullptr, nullptr, &opts, &placed, 1, &count),
      .want = SCAV_E_INVALID_ARG },
    { .what = "layout_run: no options",
      .got = run(&chart, nullptr, nullptr, &placed, 1, &count),
      .want = SCAV_E_INVALID_ARG },
    { .what = "layout_run: nowhere to put the count",
      .got = run(&chart, nullptr, &opts, &placed, 1, nullptr),
      .want = SCAV_E_INVALID_ARG },
    { .what = "router_list: nowhere to put the count",
      .got = scav_router_list(nullptr),
      .want = SCAV_E_INVALID_ARG },
    { .what = "router_name: nowhere to put the bytes",
      .got = scav_router_name(0, nullptr, &count),
      .want = SCAV_E_INVALID_ARG },
    { .what = "router_name: nowhere to put the length",
      .got = scav_router_name(0, &bytes, nullptr),
      .want = SCAV_E_INVALID_ARG },
    { .what = "router_name: no such router",
      .got = scav_router_name(9999, &bytes, &count),
      .want = SCAV_E_INVALID_ARG },
    { .what = "router_by_name: no name",
      .got = scav_router_by_name(nullptr, 1, &router),
      .want = SCAV_E_INVALID_ARG },
    { .what = "router_by_name: nowhere to put the id",
      .got = scav_router_by_name(&guard, 1, nullptr),
      .want = SCAV_E_INVALID_ARG },
    { .what = "router_by_name: no such router",
      .got = scav_router_by_name(&guard, 1, &router),
      .want = SCAV_E_INVALID_ARG },
  };
  for (Case const &c : cases) {
    CAPTURE(c.what);
    CHECK(c.got == c.want);
  }

  CHECK(bytes == &guard);
  CHECK(count == SENTINEL);
  CHECK(router == SENTINEL);
  CHECK(profile.profile_id == SENTINEL_I);  // an unknown name writes nothing
  CHECK(placed.x == SENTINEL_I);
  CHECK(chart.diags.empty());  // nothing above got as far as running anything
}

TEST_CASE("layout abi: a size that disagrees with the header is refused first") {
  scav_chart chart{ .chart = two_transitions(), .diags = {} };
  scav_layout_opts const opts{ readable() };
  scav_profile profile{ .profile_id = SENTINEL_I };

  // One int32 short is the shape of the incident this exists to refuse: a
  // caller built against a 47-knob header handing a 48-knob library its struct.
  for (uint32_t size : { PROFILE_SIZE - 4, PROFILE_SIZE + 4, 0U }) {
    CAPTURE(size);
    CHECK(scav_profile_named("readable", &profile, size) == SCAV_E_ABI);
    CHECK(profile.profile_id == SENTINEL_I);  // refused before it was written
    CHECK(scav_profile_validate(&profile, size) == SCAV_E_ABI);
    // SCAV_E_ABI outranks the null-argument refusal: a caller whose header
    // differs has said nothing about the rest of its arguments worth reading.
    CHECK(scav_profile_named("readable", nullptr, size) == SCAV_E_ABI);
    CHECK(scav_profile_named(nullptr, &profile, size) == SCAV_E_ABI);
    CHECK(scav_profile_validate(nullptr, size) == SCAV_E_ABI);
  }
  REQUIRE(scav_profile_named("readable", &profile, PROFILE_SIZE) == SCAV_OK);
  CHECK(profile.profile_id != SENTINEL_I);
  CHECK(scav_profile_validate(&profile, PROFILE_SIZE) == SCAV_OK);

  std::vector<scav_path_box> const boxes{ { .subject = 0, .w = 10, .h = 4, .order = 0 } };
  scav_spaces const spaces{ strided({ .path_box = boxes.data(), .n_path_box = 1 }) };
  scav_placed placed{ .x = SENTINEL_I, .y = SENTINEL_I, .w = SENTINEL_I, .h = SENTINEL_I };
  uint32_t count{ SENTINEL };

  // Each of layout_run's three sizes wrong on its own, so each check is the one
  // that fires; and each with the pointer beside it null, since the size is
  // tested whether or not there is anything to read.
  for (int32_t delta : { -4, 4 }) {
    CAPTURE(delta);
    uint32_t const bad_spaces{ SPACES_SIZE + static_cast<uint32_t>(delta) };
    uint32_t const bad_opts{ OPTS_SIZE + static_cast<uint32_t>(delta) };
    uint32_t const bad_placed{ PLACED_SIZE + static_cast<uint32_t>(delta) };
    CHECK(scav_layout_run(&chart,
                          &spaces,
                          bad_spaces,
                          &opts,
                          OPTS_SIZE,
                          &placed,
                          1,
                          PLACED_SIZE,
                          &count) == SCAV_E_ABI);
    CHECK(scav_layout_run(&chart,
                          nullptr,
                          bad_spaces,
                          &opts,
                          OPTS_SIZE,
                          &placed,
                          1,
                          PLACED_SIZE,
                          &count) == SCAV_E_ABI);
    CHECK(scav_layout_run(&chart,
                          &spaces,
                          SPACES_SIZE,
                          &opts,
                          bad_opts,
                          &placed,
                          1,
                          PLACED_SIZE,
                          &count) == SCAV_E_ABI);
    CHECK(scav_layout_run(&chart,
                          &spaces,
                          SPACES_SIZE,
                          &opts,
                          OPTS_SIZE,
                          &placed,
                          1,
                          bad_placed,
                          &count) == SCAV_E_ABI);
    CHECK(scav_layout_run(&chart,
                          &spaces,
                          SPACES_SIZE,
                          &opts,
                          OPTS_SIZE,
                          nullptr,
                          0,
                          bad_placed,
                          &count) == SCAV_E_ABI);
    CHECK(scav_layout_run(nullptr,
                          nullptr,
                          bad_spaces,
                          nullptr,
                          bad_opts,
                          nullptr,
                          0,
                          bad_placed,
                          nullptr) == SCAV_E_ABI);
  }

  // And each stride inside the space table, which a caller's header declares
  // one member at a time: an absent one reads as zero.
  for (uint32_t which = 0; which < 4; ++which) {
    CAPTURE(which);
    scav_spaces one_wrong{ spaces };
    std::array<uint32_t *, 4> const strides{ &one_wrong.box_state_stride,
                                             &one_wrong.box_sub_stride,
                                             &one_wrong.path_clear_stride,
                                             &one_wrong.path_box_stride };
    *strides[which] = 0;
    CHECK(run(&chart, &one_wrong, &opts, &placed, 1, &count) == SCAV_E_ABI);
  }

  // Nothing above wrote a row, moved the count, or reached layout.
  CHECK(placed.x == SENTINEL_I);
  CHECK(count == SENTINEL);
  CHECK(chart.diags.empty());

  REQUIRE(run(&chart, &spaces, &opts, &placed, 1, &count) == SCAV_OK);
  CHECK(count == 1);
  CHECK(placed.w >= 10);
}

TEST_CASE("layout abi: the placed boxes honour the query-then-fill protocol") {
  scav_chart chart{ .chart = two_transitions(), .diags = {} };
  scav_layout_opts const opts{ readable() };
  std::vector<scav_path_box> const boxes{
    { .subject = 0, .w = 10, .h = 4, .order = 0 },
    { .subject = 1, .w = 12, .h = 4, .order = 0 },
  };
  scav_spaces const spaces{ strided({ .path_box = boxes.data(), .n_path_box = 2 }) };

  uint32_t count{ SENTINEL };
  REQUIRE(run(&chart, &spaces, &opts, nullptr, 0, &count) == SCAV_OK);
  CHECK(count == 2);

  // A cap short of the count never truncates, and says how many were wanted.
  std::vector<scav_placed> placed(
      2,
      scav_placed{ .x = SENTINEL_I, .y = SENTINEL_I, .w = SENTINEL_I, .h = SENTINEL_I });
  count = SENTINEL;
  CHECK(run(&chart, &spaces, &opts, placed.data(), 1, &count) == SCAV_E_CAPACITY);
  CHECK(count == 2);
  CHECK(placed[0].x == SENTINEL_I);

  // A cap that fits with no buffer under it is the argument error.
  count = SENTINEL;
  CHECK(run(&chart, &spaces, &opts, nullptr, 2, &count) == SCAV_E_INVALID_ARG);
  CHECK(count == 2);

  // Neither reached layout, so neither left a finding behind.
  CHECK(chart.diags.empty());

  REQUIRE(run(&chart, &spaces, &opts, placed.data(), 2, &count) == SCAV_OK);
  CHECK(count == 2);
  CHECK(placed[0].w >= 10);
  CHECK(placed[1].w >= 12);
}

TEST_CASE("layout abi: a run asking for no boxes at all fills nothing") {
  // The count query is only a query when boxes are pending: with none, a zero
  // cap is a run that had nothing to write.
  scav_chart chart{ .chart = two_transitions(), .diags = {} };
  scav_layout_opts const opts{ readable() };

  uint32_t count{ SENTINEL };
  REQUIRE(run(&chart, nullptr, &opts, nullptr, 0, &count) == SCAV_OK);
  CHECK(count == 0);

  // And a buffer offered where there is nothing to put in it stays as it was.
  scav_placed placed{ .x = SENTINEL_I, .y = SENTINEL_I, .w = SENTINEL_I, .h = SENTINEL_I };
  count = SENTINEL;
  REQUIRE(run(&chart, nullptr, &opts, &placed, 1, &count) == SCAV_OK);
  CHECK(count == 0);
  CHECK(placed.x == SENTINEL_I);

  scav_column_id id{ SENTINEL };
  REQUIRE(scav_column_find(&chart, "scav.geom.state", &id) == SCAV_OK);
  uint32_t rows{ SENTINEL };
  REQUIRE(scav_column_count(&chart, id, &rows) == SCAV_OK);
  CHECK(rows == chart.chart.states.size());
}
