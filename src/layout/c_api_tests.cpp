// The C layout API: how each entry point refuses an unusable argument, and the
// out-param protocol for placed boxes.

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

// Out-param values a refused call leaves untouched.
constexpr uint32_t SENTINEL{ 0xD1CE'D1CEU };
constexpr int32_t SENTINEL_I{ 0x5CA5 };

// The struct sizes the C API checks, as this build measures them.
constexpr uint32_t PROFILE_SIZE{ static_cast<uint32_t>(sizeof(scav_profile)) };
constexpr uint32_t OPTS_SIZE{ static_cast<uint32_t>(sizeof(scav_layout_opts)) };
constexpr uint32_t SPACES_SIZE{ static_cast<uint32_t>(sizeof(scav_spaces)) };
constexpr uint32_t PLACED_SIZE{ static_cast<uint32_t>(sizeof(scav_placed)) };

// Returns `s` with every stride set to its row struct's size.
scav_spaces strided(scav_spaces s) {
  s.box_state_stride = static_cast<uint32_t>(sizeof(scav_box_space));
  s.box_sub_stride = static_cast<uint32_t>(sizeof(scav_box_space));
  s.path_clear_stride = static_cast<uint32_t>(sizeof(scav_path_clear));
  s.path_box_stride = static_cast<uint32_t>(sizeof(scav_path_box));
  return s;
}

// Two states in one submachine and one transition each way between them.
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

// Calls `scav_layout_run` with every struct size correct.
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
  CHECK(chart.diags.empty());  // no refused call adds a diagnostic
}

TEST_CASE("layout abi: a size that disagrees with the header is refused first") {
  scav_chart chart{ .chart = two_transitions(), .diags = {} };
  scav_layout_opts const opts{ readable() };
  scav_profile profile{ .profile_id = SENTINEL_I };

  for (uint32_t size : { PROFILE_SIZE - 4, PROFILE_SIZE + 4, 0U }) {
    CAPTURE(size);
    CHECK(scav_profile_named("readable", &profile, size) == SCAV_E_ABI);
    CHECK(profile.profile_id == SENTINEL_I);  // left unwritten
    CHECK(scav_profile_validate(&profile, size) == SCAV_E_ABI);
    // SCAV_E_ABI takes precedence over a null argument.
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

  // Each of layout_run's three sizes wrong alone, and with its pointer null.
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

  // Each space-table stride at zero, as from an absent member, is refused.
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

  // The refused calls write no row, leave the count, and add no diagnostic.
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

  // A cap short of the count returns SCAV_E_CAPACITY and the full count, writing no row.
  std::vector<scav_placed> placed(
      2,
      scav_placed{ .x = SENTINEL_I, .y = SENTINEL_I, .w = SENTINEL_I, .h = SENTINEL_I });
  count = SENTINEL;
  CHECK(run(&chart, &spaces, &opts, placed.data(), 1, &count) == SCAV_E_CAPACITY);
  CHECK(count == 2);
  CHECK(placed[0].x == SENTINEL_I);

  // A cap that fits with a null buffer returns SCAV_E_INVALID_ARG.
  count = SENTINEL;
  CHECK(run(&chart, &spaces, &opts, nullptr, 2, &count) == SCAV_E_INVALID_ARG);
  CHECK(count == 2);

  // Neither call adds a diagnostic.
  CHECK(chart.diags.empty());

  REQUIRE(run(&chart, &spaces, &opts, placed.data(), 2, &count) == SCAV_OK);
  CHECK(count == 2);
  CHECK(placed[0].w >= 10);
  CHECK(placed[1].w >= 12);
}

TEST_CASE("layout abi: a run asking for no boxes at all fills nothing") {
  // With no path boxes requested, a zero cap runs layout and reports a count of 0.
  scav_chart chart{ .chart = two_transitions(), .diags = {} };
  scav_layout_opts const opts{ readable() };

  uint32_t count{ SENTINEL };
  REQUIRE(run(&chart, nullptr, &opts, nullptr, 0, &count) == SCAV_OK);
  CHECK(count == 0);

  // A buffer passed with no boxes requested is left unchanged.
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
