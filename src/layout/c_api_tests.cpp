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

#include <cstdint>
#include <vector>

namespace {

using namespace scav;

// What a refused call must leave an out-param holding.
constexpr uint32_t SENTINEL{ 0xD1CE'D1CEU };
constexpr int32_t SENTINEL_I{ 0x5CA5 };

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
      .got = scav_profile_named(nullptr, &profile),
      .want = SCAV_E_INVALID_ARG },
    { .what = "profile_named: nowhere to put it",
      .got = scav_profile_named("readable", nullptr),
      .want = SCAV_E_INVALID_ARG },
    { .what = "profile_named: no such profile",
      .got = scav_profile_named("no-such-profile", &profile),
      .want = SCAV_E_INVALID_ARG },
    { .what = "profile_validate: no profile",
      .got = scav_profile_validate(nullptr),
      .want = SCAV_E_INVALID_ARG },
    { .what = "layout_run: no chart",
      .got = scav_layout_run(nullptr, nullptr, &opts, &placed, 1, &count),
      .want = SCAV_E_INVALID_ARG },
    { .what = "layout_run: no options",
      .got = scav_layout_run(&chart, nullptr, nullptr, &placed, 1, &count),
      .want = SCAV_E_INVALID_ARG },
    { .what = "layout_run: nowhere to put the count",
      .got = scav_layout_run(&chart, nullptr, &opts, &placed, 1, nullptr),
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

TEST_CASE("layout abi: the placed boxes honour the query-then-fill protocol") {
  scav_chart chart{ .chart = two_transitions(), .diags = {} };
  scav_layout_opts const opts{ readable() };
  std::vector<scav_path_box> const boxes{
    { .subject = 0, .w = 10, .h = 4, .order = 0 },
    { .subject = 1, .w = 12, .h = 4, .order = 0 },
  };
  scav_spaces const spaces{ .path_box = boxes.data(), .n_path_box = 2 };

  uint32_t count{ SENTINEL };
  REQUIRE(scav_layout_run(&chart, &spaces, &opts, nullptr, 0, &count) == SCAV_OK);
  CHECK(count == 2);

  // A cap short of the count never truncates, and says how many were wanted.
  std::vector<scav_placed> placed(
      2,
      scav_placed{ .x = SENTINEL_I, .y = SENTINEL_I, .w = SENTINEL_I, .h = SENTINEL_I });
  count = SENTINEL;
  CHECK(scav_layout_run(&chart, &spaces, &opts, placed.data(), 1, &count) ==
        SCAV_E_CAPACITY);
  CHECK(count == 2);
  CHECK(placed[0].x == SENTINEL_I);

  // A cap that fits with no buffer under it is the argument error.
  count = SENTINEL;
  CHECK(scav_layout_run(&chart, &spaces, &opts, nullptr, 2, &count) == SCAV_E_INVALID_ARG);
  CHECK(count == 2);

  // Neither reached layout, so neither left a finding behind.
  CHECK(chart.diags.empty());

  REQUIRE(scav_layout_run(&chart, &spaces, &opts, placed.data(), 2, &count) == SCAV_OK);
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
  REQUIRE(scav_layout_run(&chart, nullptr, &opts, nullptr, 0, &count) == SCAV_OK);
  CHECK(count == 0);

  // And a buffer offered where there is nothing to put in it stays as it was.
  scav_placed placed{ .x = SENTINEL_I, .y = SENTINEL_I, .w = SENTINEL_I, .h = SENTINEL_I };
  count = SENTINEL;
  REQUIRE(scav_layout_run(&chart, nullptr, &opts, &placed, 1, &count) == SCAV_OK);
  CHECK(count == 0);
  CHECK(placed.x == SENTINEL_I);

  scav_column_id id{ SENTINEL };
  REQUIRE(scav_column_find(&chart, "scav.geom.state", &id) == SCAV_OK);
  uint32_t rows{ SENTINEL };
  REQUIRE(scav_column_count(&chart, id, &rows) == SCAV_OK);
  CHECK(rows == chart.chart.states.size());
}
