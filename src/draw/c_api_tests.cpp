// The C projection of scav_draw.h: what each entry point does with an argument
// it cannot use. The behaviour behind them is metrics', drawlist's and
// builder's own tests; here it is the codes and the out-params.

#include "scav/scav_draw_c.h"

#include "scav/scav_core.h"
#include "scav/scav_draw.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include "draw/handles.h"
#include "scav_c_handles.h"

#include "doctest.h"

#include <cstdint>
#include <vector>

namespace {

using namespace scav;

// What a refused call must leave an out-param holding.
constexpr uint32_t SENTINEL{ 0xD1CE'D1CEU };
constexpr int32_t SENTINEL_I{ 0x5CA5 };

// A named state, a submachine with two children and a labelled transition: the
// smallest chart that fills all four space tables with something.
Chart small_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const on{ build_state(c, root, "Running", StateKind::Normal, {}) };
  SubmachineId const main{ build_submachine(c, on, "main", {}) };
  StateId const idle{ build_state(c, main, "Idle", StateKind::Normal, {}) };
  StateId const busy{ build_state(c, main, "Busy", StateKind::Normal, {}) };
  build_trans(c, idle, busy, TransKind::External, "work arrived");
  return c;
}

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

struct Case {
  char const *what;
  scav_result got;
  scav_result want;
};

void run(std::vector<Case> const &cases) {
  for (Case const &c : cases) {
    CAPTURE(c.what);
    CHECK(c.got == c.want);
  }
}

}  // namespace

TEST_CASE("draw abi: a null argument is refused whichever one it is") {
  scav_metrics *metrics{ nullptr };
  scav_drawlist *list{ nullptr };
  scav_images *images{ nullptr };
  REQUIRE(scav_metrics_create(nullptr, 0, &metrics) == SCAV_OK);
  REQUIRE(scav_drawlist_create(&list) == SCAV_OK);
  REQUIRE(scav_images_create(&images) == SCAV_OK);
  scav_chart chart{ .chart = small_chart(), .diags = {} };
  scav_profile const profile{ readable() };

  scav_byte const guard{ 0x5C };
  scav_byte const *bytes{ &guard };
  uint32_t count{ SENTINEL };
  scav_extent extent{ .w = SENTINEL_I, .h = SENTINEL_I };
  scav_span const empty{ .off = 0, .len = 0 };
  std::vector<uint32_t> counts(4, SENTINEL);

  run({
      { .what = "metrics_create: nowhere to put the handle",
        .got = scav_metrics_create(&guard, 1, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "metrics_identity: no handle",
        .got = scav_metrics_identity(nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "metrics_identity: nowhere to put it",
        .got = scav_metrics_identity(metrics, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "metrics_units_per_em: no handle",
        .got = scav_metrics_units_per_em(nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "metrics_units_per_em: nowhere to put it",
        .got = scav_metrics_units_per_em(metrics, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "metrics_glyph_count: no handle",
        .got = scav_metrics_glyph_count(nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "metrics_glyph_count: nowhere to put it",
        .got = scav_metrics_glyph_count(metrics, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_text: no handle",
        .got = scav_measure_text(nullptr, &guard, 1, 16, &extent),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_text: nowhere to put the extent",
        .got = scav_measure_text(metrics, &guard, 1, 16, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "line_height: nowhere to put it",
        .got = scav_line_height(16, 7, 5, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_block: no handle",
        .got = scav_measure_block(nullptr, &guard, 1, 16, 7, 5, &extent),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_block: nowhere to put the extent",
        .got = scav_measure_block(metrics, &guard, 1, 16, 7, 5, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_counts: no list",
        .got = scav_drawlist_counts(nullptr, &count, nullptr, nullptr, nullptr, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_str: no list",
        .got = scav_drawlist_str(nullptr, empty, &bytes, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_str: nowhere to put the bytes",
        .got = scav_drawlist_str(list, empty, nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_str: nowhere to put the length",
        .got = scav_drawlist_str(list, empty, &bytes, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_validate: no list",
        .got = scav_drawlist_validate(nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_canonicalize: no list",
        .got = scav_drawlist_canonicalize(nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_digest: no list",
        .got = scav_drawlist_digest(nullptr, metrics, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_digest: nowhere to put it",
        .got = scav_drawlist_digest(list, metrics, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "drawlist_append: no destination",
        .got = scav_drawlist_append(nullptr, list),
        .want = SCAV_E_INVALID_ARG },
      { .what = "images_create: nowhere to put the handle",
        .got = scav_images_create(nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_register: no registry",
        .got = scav_image_register(nullptr, "a", &guard, 1, 8, 8, "image/png"),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_register: no id",
        .got = scav_image_register(images, nullptr, &guard, 1, 8, 8, "image/png"),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_register: no mime type",
        .got = scav_image_register(images, "a", &guard, 1, 8, 8, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_count: no registry",
        .got = scav_image_count(nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_count: nowhere to put it",
        .got = scav_image_count(images, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_find: no registry",
        .got = scav_image_find(nullptr, &guard, 1, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_find: no id to look for",
        .got = scav_image_find(images, nullptr, 1, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_find: nowhere to put the index",
        .got = scav_image_find(images, &guard, 1, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_extent: no registry",
        .got = scav_image_extent(nullptr, 0, &extent),
        .want = SCAV_E_INVALID_ARG },
      { .what = "image_extent: nowhere to put the extent",
        .got = scav_image_extent(images, 0, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_chart: no chart",
        .got = scav_measure_chart(nullptr,
                                  metrics,
                                  &profile,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  counts.data()),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_chart: no metrics",
        .got = scav_measure_chart(&chart,
                                  nullptr,
                                  &profile,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  counts.data()),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_chart: no profile",
        .got = scav_measure_chart(&chart,
                                  metrics,
                                  nullptr,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  counts.data()),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_chart: nowhere to put the counts",
        .got = scav_measure_chart(&chart,
                                  metrics,
                                  &profile,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "palette_standard: nowhere to put the rows",
        .got = scav_palette_standard(nullptr, SCAV_STYLE_COUNT),
        .want = SCAV_E_INVALID_ARG },
      { .what = "emit_chart: no chart",
        .got = scav_emit_chart(list, nullptr, metrics, nullptr, 0, nullptr, nullptr, 0, 0),
        .want = SCAV_E_INVALID_ARG },
      { .what = "emit_chart: no metrics",
        .got = scav_emit_chart(list, &chart, nullptr, nullptr, 0, nullptr, nullptr, 0, 0),
        .want = SCAV_E_INVALID_ARG },
  });

  CHECK(bytes == &guard);
  CHECK(count == SENTINEL);
  CHECK(extent.w == SENTINEL_I);
  CHECK(extent.h == SENTINEL_I);
  for (uint32_t const row : counts) { CHECK(row == SENTINEL); }

  scav_images_destroy(images);
  scav_drawlist_destroy(list);
  scav_metrics_destroy(metrics);
}

TEST_CASE("draw abi: each way a measurement can fail keeps its own code") {
  scav_metrics *metrics{ nullptr };
  REQUIRE(scav_metrics_create(nullptr, 0, &metrics) == SCAV_OK);
  auto const *text{ reinterpret_cast<scav_byte const *>("one\ntwo") };
  scav_extent extent{ .w = SENTINEL_I, .h = SENTINEL_I };

  // A newline and malformed bytes are both the caller's fault, so both are the
  // argument error; a codepoint the font lacks is the font's, and is its own.
  run({
      { .what = "measure_text: a newline, which the caller splits on itself",
        .got = scav_measure_text(metrics, text, 7, 160, &extent),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_text: bytes that are not UTF-8",
        .got = scav_measure_text(metrics,
                                 reinterpret_cast<scav_byte const *>("\xC0\x80"),
                                 2,
                                 160,
                                 &extent),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_text: no bytes where a length says there are some",
        .got = scav_measure_text(metrics, nullptr, 5, 160, &extent),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_block: no bytes where a length says there are some",
        .got = scav_measure_block(metrics, nullptr, 5, 160, 7, 5, &extent),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_block: bytes that are not UTF-8",
        .got = scav_measure_block(metrics,
                                  reinterpret_cast<scav_byte const *>("\xC0\x80"),
                                  2,
                                  160,
                                  7,
                                  5,
                                  &extent),
        .want = SCAV_E_INVALID_ARG },
      { .what = "measure_block: a codepoint the font has no glyph for",
        .got = scav_measure_block(metrics,
                                  reinterpret_cast<scav_byte const *>("\xF3\xB0\x80\x81"),
                                  4,
                                  160,
                                  7,
                                  5,
                                  &extent),
        .want = SCAV_E_NO_GLYPH },
      { .what = "measure_block: a line height outside the domain",
        .got = scav_measure_block(metrics, text, 7, 160, 7, 0, &extent),
        .want = SCAV_E_INVALID_ARG },
  });

  // The extent is zeroed before anything is measured, so a refused call reads
  // back nothing rather than a partial width.
  CHECK(extent.w == 0);
  CHECK(extent.h == 0);

  scav_metrics_destroy(metrics);
}

TEST_CASE("draw abi: an empty drawlist reads back null and zero, not a bad pointer") {
  scav_drawlist *list{ nullptr };
  REQUIRE(scav_drawlist_create(&list) == SCAV_OK);

  scav_prim const *prims{ nullptr };
  scav_style const *styles{ nullptr };
  scav_point const *points{ nullptr };
  scav_rect const *clips{ nullptr };
  uint32_t count{ SENTINEL };

  REQUIRE(scav_drawlist_prims(list, &prims, &count) == SCAV_OK);
  CHECK(prims == nullptr);
  CHECK(count == 0);
  count = SENTINEL;
  REQUIRE(scav_drawlist_styles(list, &styles, &count) == SCAV_OK);
  CHECK(styles == nullptr);
  CHECK(count == 0);
  count = SENTINEL;
  REQUIRE(scav_drawlist_points(list, &points, &count) == SCAV_OK);
  CHECK(points == nullptr);
  CHECK(count == 0);
  count = SENTINEL;
  REQUIRE(scav_drawlist_clips(list, &clips, &count) == SCAV_OK);
  CHECK(clips == nullptr);
  CHECK(count == 0);

  // The pool is empty, so every span but the empty one is past its end.
  scav_byte const guard{ 0x5C };
  scav_byte const *bytes{ &guard };
  count = SENTINEL;
  CHECK(scav_drawlist_str(list, { .off = 1, .len = 0 }, &bytes, &count) ==
        SCAV_E_INVALID_ARG);
  CHECK(bytes == &guard);
  CHECK(count == SENTINEL);
  REQUIRE(scav_drawlist_str(list, { .off = 0, .len = 0 }, &bytes, &count) == SCAV_OK);
  CHECK(bytes == nullptr);
  CHECK(count == 0);

  // Each count is optional on its own, so the one omitted here is the first --
  // which is the one every other caller passes.
  uint32_t styles_n{ SENTINEL };
  uint32_t text_n{ SENTINEL };
  REQUIRE(scav_drawlist_counts(list, nullptr, &styles_n, nullptr, nullptr, &text_n) ==
          SCAV_OK);
  CHECK(styles_n == 0);
  CHECK(text_n == 0);

  // The offending primitive is optional too: an empty list has none, and a
  // caller that does not ask is not written to.
  REQUIRE(scav_drawlist_validate(list, nullptr) == SCAV_OK);

  // Each array accessor refuses each of its own out-params.
  run({
      { .what = "prims: no list",
        .got = scav_drawlist_prims(nullptr, &prims, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "prims: nowhere to put the count",
        .got = scav_drawlist_prims(list, &prims, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "styles: no list",
        .got = scav_drawlist_styles(nullptr, &styles, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "styles: nowhere to put the rows",
        .got = scav_drawlist_styles(list, nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "styles: nowhere to put the count",
        .got = scav_drawlist_styles(list, &styles, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "points: no list",
        .got = scav_drawlist_points(nullptr, &points, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "points: nowhere to put the rows",
        .got = scav_drawlist_points(list, nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "points: nowhere to put the count",
        .got = scav_drawlist_points(list, &points, nullptr),
        .want = SCAV_E_INVALID_ARG },
      { .what = "clips: no list",
        .got = scav_drawlist_clips(nullptr, &clips, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "clips: nowhere to put the rows",
        .got = scav_drawlist_clips(list, nullptr, &count),
        .want = SCAV_E_INVALID_ARG },
      { .what = "clips: nowhere to put the count",
        .got = scav_drawlist_clips(list, &clips, nullptr),
        .want = SCAV_E_INVALID_ARG },
  });
  CHECK(count == 0);  // the last successful read's, untouched by any of those

  scav_drawlist_destroy(list);
}

TEST_CASE("draw abi: the measurement pass honours the query-then-fill protocol") {
  scav_metrics *metrics{ nullptr };
  REQUIRE(scav_metrics_create(nullptr, 0, &metrics) == SCAV_OK);
  scav_chart chart{ .chart = small_chart(), .diags = {} };
  scav_profile const profile{ readable() };

  std::vector<uint32_t> counts(4, SENTINEL);
  REQUIRE(scav_measure_chart(&chart,
                             metrics,
                             &profile,
                             nullptr,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             0,
                             counts.data()) == SCAV_OK);
  for (uint32_t const row : counts) { CHECK(row != 0); }

  std::vector<scav_box_space> box_state(counts[0], scav_box_space{});
  std::vector<scav_box_space> box_sub(counts[1], scav_box_space{});
  std::vector<scav_path_clear> path_clear(counts[2], scav_path_clear{});
  std::vector<scav_path_box> path_box(counts[3], scav_path_box{});

  // One cap short at a time, so each table's own check is the one that fires.
  struct Short {
    char const *what;
    uint32_t at;
  };
  for (Short const &s : { Short{ .what = "state boxes", .at = 0 },
                          Short{ .what = "submachine boxes", .at = 1 },
                          Short{ .what = "path clearances", .at = 2 },
                          Short{ .what = "path boxes", .at = 3 } }) {
    CAPTURE(s.what);
    std::vector<uint32_t> caps{ counts };
    caps[s.at] -= 1;
    std::vector<uint32_t> got(4, SENTINEL);
    CHECK(scav_measure_chart(&chart,
                             metrics,
                             &profile,
                             box_state.data(),
                             caps[0],
                             box_sub.data(),
                             caps[1],
                             path_clear.data(),
                             caps[2],
                             path_box.data(),
                             caps[3],
                             got.data()) == SCAV_E_CAPACITY);
    // Never truncates, and still says how much was wanted.
    for (uint32_t i = 0; i < 4; ++i) { CHECK(got[i] == counts[i]); }
    CHECK(box_state[0].min_w == 0);
  }

  // A cap that fits with no buffer under it is the argument error, not a write.
  for (uint32_t which = 0; which < 4; ++which) {
    CAPTURE(which);
    std::vector<uint32_t> got(4, SENTINEL);
    CHECK(scav_measure_chart(&chart,
                             metrics,
                             &profile,
                             (which == 0) ? nullptr : box_state.data(),
                             counts[0],
                             (which == 1) ? nullptr : box_sub.data(),
                             counts[1],
                             (which == 2) ? nullptr : path_clear.data(),
                             counts[2],
                             (which == 3) ? nullptr : path_box.data(),
                             counts[3],
                             got.data()) == SCAV_E_INVALID_ARG);
    for (uint32_t i = 0; i < 4; ++i) { CHECK(got[i] == counts[i]); }
  }

  REQUIRE(scav_measure_chart(&chart,
                             metrics,
                             &profile,
                             box_state.data(),
                             counts[0],
                             box_sub.data(),
                             counts[1],
                             path_clear.data(),
                             counts[2],
                             path_box.data(),
                             counts[3],
                             counts.data()) == SCAV_OK);
  CHECK(box_state[0].min_w != 0);

  // All four caps zero is the query; any one of them non-zero is a fill, so a
  // caller wanting one table still has to size the other three.
  for (uint32_t which = 1; which < 4; ++which) {
    CAPTURE(which);
    std::vector<uint32_t> caps(4, 0);
    caps[which] = counts[which];
    std::vector<uint32_t> got(4, SENTINEL);
    CHECK(scav_measure_chart(&chart,
                             metrics,
                             &profile,
                             box_state.data(),
                             caps[0],
                             box_sub.data(),
                             caps[1],
                             path_clear.data(),
                             caps[2],
                             path_box.data(),
                             caps[3],
                             got.data()) == SCAV_E_CAPACITY);
    for (uint32_t i = 0; i < 4; ++i) { CHECK(got[i] == counts[i]); }
  }

  // A chart with no entities fills nothing and says so, rather than reading a
  // buffer it was handed the room for.
  scav_chart bare{ .chart = {}, .diags = {} };
  std::vector<uint32_t> none(4, SENTINEL);
  std::vector<scav_box_space> untouched(
      1,
      scav_box_space{ .min_w = SENTINEL_I, .h_before = 0, .h_after = 0 });
  REQUIRE(scav_measure_chart(&bare,
                             metrics,
                             &profile,
                             untouched.data(),
                             1,
                             nullptr,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             0,
                             none.data()) == SCAV_OK);
  for (uint32_t const row : none) { CHECK(row == 0); }
  CHECK(untouched[0].min_w == SENTINEL_I);

  // A profile with no line height in it leaves the measurement's domain, which
  // is the handle's state rather than one bad argument.
  scav_profile const unusable{};
  std::vector<uint32_t> unreached(4, SENTINEL);
  CHECK(scav_measure_chart(&chart,
                           metrics,
                           &unusable,
                           nullptr,
                           0,
                           nullptr,
                           0,
                           nullptr,
                           0,
                           nullptr,
                           0,
                           unreached.data()) == SCAV_E_STATE);
  for (uint32_t const row : unreached) { CHECK(row == SENTINEL); }

  scav_metrics_destroy(metrics);
}

TEST_CASE("draw abi: the shipped palette is written only when it fits") {
  std::vector<scav_style> rows(SCAV_STYLE_COUNT, scav_style{});
  CHECK(scav_palette_standard(rows.data(), 0) == SCAV_E_CAPACITY);
  CHECK(rows[0].stroke_rgba == 0);  // refused, so nothing was written
  REQUIRE(scav_palette_standard(rows.data(), SCAV_STYLE_COUNT) == SCAV_OK);
  CHECK(rows[SCAV_STYLE_COUNT - 1].stroke_rgba != 0);
}
