// Interning, the per-kind point rules, the canonical form's order-independence,
// and append's rebasing.

#include "scav/scav_draw.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "draw/handles.h"

#include "doctest.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

constexpr ElemRef NONE{ .kind = ElemKind::None, .ordinal = INVALID };

ElemRef state(uint32_t i) { return { .kind = ElemKind::State, .ordinal = i }; }

scav_style ink(uint32_t rgba) {
  return { .stroke_rgba = rgba,
           .fill_rgba = 0,
           .stroke_w = 16,
           .dash = 0,
           .font_size_grid = 0 };
}

uint32_t valid(DrawList const &d) {
  uint32_t bad{ 0 };
  return drawlist_validate(d, bad) ? INVALID : bad;
}

}  // namespace

TEST_CASE("drawlist: an empty list is valid, canonical, and hashes stably") {
  DrawList d;
  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  CHECK(valid(d) == INVALID);
  uint32_t const before{ drawlist_digest(d, m) };
  drawlist_canonicalize(d);
  CHECK(d.prims.empty());
  CHECK(drawlist_digest(d, m) == before);
}

TEST_CASE("drawlist: styles and clips intern rather than accumulate") {
  DrawList d;
  CHECK(drawlist_style(d, ink(0xFF)) == 0);
  CHECK(drawlist_style(d, ink(0xFF)) == 0);  // the same style is the same row
  CHECK(drawlist_style(d, ink(0xEE)) == 1);
  CHECK(d.styles.size() == 2);

  scav_rect const r{ .x = 1, .y = 2, .w = 3, .h = 4 };
  CHECK(drawlist_clip(d, r) == 0);
  CHECK(drawlist_clip(d, r) == 0);
  CHECK(drawlist_clip(d, { .x = 1, .y = 2, .w = 3, .h = 5 }) == 1);
  CHECK(d.clips.size() == 2);

  // Every field is part of a style's identity, so no two differ silently.
  scav_style probe{ ink(0xFF) };
  probe.fill_rgba = 1;
  CHECK(drawlist_style(d, probe) == 2);
  probe.stroke_w = 99;
  CHECK(drawlist_style(d, probe) == 3);
  probe.dash = 7;
  CHECK(drawlist_style(d, probe) == 4);
  probe.font_size_grid = 160;
  CHECK(drawlist_style(d, probe) == 5);
}

TEST_CASE("drawlist: the text pool keeps duplicates and an empty span is free") {
  DrawList d;
  scav_span const first{ drawlist_text(d, "Idle") };
  scav_span const second{ drawlist_text(d, "Idle") };
  CHECK(first.off != second.off);  // the pool never deduplicates
  CHECK(first.len == 4);
  scav_span const empty{ drawlist_text(d, "") };
  CHECK(empty.len == 0);
  CHECK(d.text.bytes.size() == 8);
}

TEST_CASE("drawlist: every kind lands with the point count its kind demands") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, ink(0)) };
  scav_rect const r{ .x = 10, .y = 20, .w = 30, .h = 40 };
  scav_point const pts[3]{ { .x = 0, .y = 0 }, { .x = 1, .y = 1 }, { .x = 2, .y = 2 } };

  push_rect(d, 0, s, r, state(0));
  push_rrect(d, 0, s, r, 4, state(1));
  push_line(d, 0, s, pts[0], pts[1], NONE);
  push_polyline(d, 0, s, pts, 3, NONE);
  push_path(d, 0, s, pts, 3, NONE);
  push_text(d, 0, s, pts[0], "hi", state(2));
  push_circle(d, 0, s, pts[0], 8, NONE);
  push_arc(d, 0, s, r, 90 * 64, 180 * 64, NONE);
  push_image(d, 0, s, r, "logo", NONE);
  REQUIRE(valid(d) == INVALID);

  CHECK(d.prims[0].points.len == 2);
  CHECK(d.prims[1].a == 4);
  CHECK(d.prims[3].points.len == 3);
  CHECK(d.prims[5].payload.len == 2);
  CHECK(d.prims[6].a == 8);
  // An arc is a bounding box plus two angles, so its radius has somewhere to be.
  CHECK(d.prims[7].points.len == 2);
  CHECK(d.prims[7].a == (90 * 64));
  CHECK(d.prims[7].b == (180 * 64));
  CHECK(d.prims[8].payload.len == 4);

  // A rect's two points are its corners, so the second is x+w, y+h.
  CHECK(d.points[d.prims[0].points.off].x == 10);
  CHECK(d.points[d.prims[0].points.off + 1].x == 40);
  CHECK(d.points[d.prims[0].points.off + 1].y == 60);

  // The origin round-trips, and a primitive belonging to nothing says so.
  CHECK(d.prims[0].origin_kind == static_cast<uint32_t>(ElemKind::State));
  CHECK(d.prims[0].origin_ordinal == 0);
  CHECK(d.prims[2].origin_kind == static_cast<uint32_t>(ElemKind::None));
  // Nothing is clipped until an app says so.
  CHECK(d.prims[0].clip == SCAV_CLIP_NONE);
}

TEST_CASE("drawlist: the validator rejects what a backend could not draw") {
  DrawList base;
  uint32_t const s{ drawlist_style(base, ink(0)) };
  push_rect(base, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, NONE);
  REQUIRE(valid(base) == INVALID);

  SUBCASE("a kind no backend knows") {
    DrawList d{ base };
    d.prims[0].kind = SCAV_PRIM_KIND_COUNT;
    CHECK(valid(d) == 0);
  }
  SUBCASE("a point count its kind forbids") {
    DrawList d{ base };
    d.prims[0].points.len = 1;
    CHECK(valid(d) == 0);
    d.prims[0].points.len = 3;
    CHECK(valid(d) == 0);
  }
  SUBCASE("a polyline shorter than a line") {
    DrawList d{ base };
    d.prims[0].kind = SCAV_PRIM_POLYLINE;
    d.prims[0].points.len = 1;
    CHECK(valid(d) == 0);
  }
  SUBCASE("a span reaching past its array") {
    DrawList d{ base };
    d.prims[0].points.off = 99;
    CHECK(valid(d) == 0);
  }
  SUBCASE("a style or clip index out of range") {
    DrawList d{ base };
    d.prims[0].style = 9;
    CHECK(valid(d) == 0);
    d.prims[0].style = 0;
    d.prims[0].clip = 9;
    CHECK(valid(d) == 0);
  }
  SUBCASE("a payload on a kind that carries none") {
    DrawList d{ base };
    d.prims[0].payload = drawlist_text(d, "stray");
    CHECK(valid(d) == 0);
  }
  SUBCASE("an image with no id to look up") {
    DrawList d{ base };
    d.prims[0].kind = SCAV_PRIM_IMAGE;
    CHECK(valid(d) == 0);
  }
  SUBCASE("a negative radius") {
    DrawList d{ base };
    d.prims[0].kind = SCAV_PRIM_RRECT;
    d.prims[0].a = -1;
    CHECK(valid(d) == 0);
  }
  SUBCASE("and it names the offending row, not the first one") {
    DrawList d{ base };
    push_rect(d, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, NONE);
    push_rect(d, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, NONE);
    d.prims[2].style = 9;
    CHECK(valid(d) == 2);
  }
}

TEST_CASE("drawlist: canonical form is content, so emission order is erased") {
  // The same picture, drawn in two orders, through two style tables built up
  // differently. Only the content is shared.
  auto const forwards = [] {
    DrawList d;
    uint32_t const a{ drawlist_style(d, ink(0xAA)) };
    uint32_t const b{ drawlist_style(d, ink(0xBB)) };
    push_rect(d, 1, a, { .x = 0, .y = 0, .w = 10, .h = 10 }, state(0));
    push_text(d, 2, b, { .x = 1, .y = 9 }, "one", state(0));
    push_text(d, 2, b, { .x = 1, .y = 19 }, "two", state(1));
    return d;
  };
  auto const backwards = [] {
    DrawList d;
    // Registered in the other order, so the raw indices disagree.
    uint32_t const b{ drawlist_style(d, ink(0xBB)) };
    uint32_t const a{ drawlist_style(d, ink(0xAA)) };
    push_text(d, 2, b, { .x = 1, .y = 19 }, "two", state(1));
    push_text(d, 2, b, { .x = 1, .y = 9 }, "one", state(0));
    push_rect(d, 1, a, { .x = 0, .y = 0, .w = 10, .h = 10 }, state(0));
    return d;
  };

  DrawList one{ forwards() };
  DrawList two{ backwards() };
  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  REQUIRE(drawlist_digest(one, m) != drawlist_digest(two, m));

  drawlist_canonicalize(one);
  drawlist_canonicalize(two);
  CHECK(drawlist_digest(one, m) == drawlist_digest(two, m));
  REQUIRE(one.prims.size() == two.prims.size());
  for (uint32_t i = 0; i < one.prims.size(); ++i) {
    CAPTURE(i);
    CHECK(one.prims[i].kind == two.prims[i].kind);
    CHECK(one.prims[i].depth == two.prims[i].depth);
    CHECK(one.prims[i].style == two.prims[i].style);
    CHECK(one.prims[i].points.off == two.prims[i].points.off);
    CHECK(one.prims[i].payload.off == two.prims[i].payload.off);
  }
  // Depth leads the key, so the rect drawn at 1 sorts ahead of the text at 2.
  CHECK(one.prims[0].depth == 1);
  CHECK(valid(one) == INVALID);
}

TEST_CASE("drawlist: canonicalizing is idempotent and dedups the tables") {
  DrawList d;
  // Four registrations of two distinct styles: interning stops the duplicates
  // inside one list, so plant them by hand the way append would.
  d.styles = { ink(0xBB), ink(0xAA), ink(0xBB), ink(0xAA) };
  push_rect(d, 0, 0, { .x = 0, .y = 0, .w = 1, .h = 1 }, NONE);
  push_rect(d, 0, 1, { .x = 2, .y = 0, .w = 1, .h = 1 }, NONE);
  push_rect(d, 0, 2, { .x = 4, .y = 0, .w = 1, .h = 1 }, NONE);
  push_rect(d, 0, 3, { .x = 6, .y = 0, .w = 1, .h = 1 }, NONE);
  d.clips = { { .x = 9, .y = 9, .w = 9, .h = 9 }, { .x = 9, .y = 9, .w = 9, .h = 9 } };
  d.prims[0].clip = 0;
  d.prims[1].clip = 1;
  REQUIRE(valid(d) == INVALID);

  drawlist_canonicalize(d);
  CHECK(d.styles.size() == 2);
  CHECK(d.clips.size() == 1);
  CHECK(valid(d) == INVALID);
  // Sorted by field bytes, so the smaller stroke colour comes first.
  CHECK(d.styles[0].stroke_rgba == 0xAA);
  CHECK(d.styles[1].stroke_rgba == 0xBB);

  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  uint32_t const once{ drawlist_digest(d, m) };
  drawlist_canonicalize(d);
  CHECK(drawlist_digest(d, m) == once);
  CHECK(d.styles.size() == 2);
}

TEST_CASE("drawlist: the digest hears content and the font, not indices") {
  Metrics bundled;
  REQUIRE(metrics_create(nullptr, 0, bundled));

  DrawList d;
  uint32_t const s{ drawlist_style(d, ink(0xAA)) };
  push_text(d, 0, s, { .x = 0, .y = 0 }, "Idle", state(0));
  drawlist_canonicalize(d);
  uint32_t const base{ drawlist_digest(d, bundled) };

  auto const moved = [&](auto mutate) {
    DrawList copy{ d };
    mutate(copy);
    drawlist_canonicalize(copy);
    return drawlist_digest(copy, bundled) != base;
  };
  CHECK(moved([](DrawList &c) { c.prims[0].depth = 1; }));
  CHECK(moved([](DrawList &c) { c.points[0].x = 1; }));
  CHECK(moved([](DrawList &c) { c.prims[0].origin_ordinal = 1; }));
  CHECK(moved([](DrawList &c) { c.styles[0].fill_rgba = 1; }));
  CHECK(moved([](DrawList &c) { c.text.bytes[0] = 'X'; }));
  CHECK(moved([](DrawList &c) {
    c.clips.push_back({ .x = 0, .y = 0, .w = 1, .h = 1 });
    c.prims[0].clip = 0;
  }));

  // A different font over the same primitives is a different picture, because
  // the advances those coordinates came from differ.
  Metrics other{ bundled };
  other.identity ^= 1U;
  CHECK(drawlist_digest(d, other) != base);
}

TEST_CASE("drawlist: append rebases all four index spaces") {
  DrawList host;
  uint32_t const host_style{ drawlist_style(host, ink(0xAA)) };
  push_text(host, 0, host_style, { .x = 1, .y = 1 }, "host", state(0));
  scav_rect const clip{ .x = 0, .y = 0, .w = 100, .h = 100 };
  host.prims[0].clip = drawlist_clip(host, clip);

  DrawList guest;
  uint32_t const shared{ drawlist_style(guest, ink(0xAA)) };  // the same style
  uint32_t const own{ drawlist_style(guest, ink(0xCC)) };
  push_text(guest, 5, own, { .x = 2, .y = 2 }, "guest", state(1));
  push_rect(guest, 5, shared, { .x = 3, .y = 3, .w = 4, .h = 4 }, state(2));
  guest.prims[1].clip = drawlist_clip(guest, clip);  // and the same clip

  drawlist_append(host, guest);
  REQUIRE(valid(host) == INVALID);
  REQUIRE(host.prims.size() == 3);
  // A shared style interns instead of duplicating, so two lists sharing a
  // palette do not double it.
  CHECK(host.styles.size() == 2);
  CHECK(host.clips.size() == 1);
  CHECK(host.prims[2].style == host_style);
  CHECK(host.prims[2].clip == host.prims[0].clip);

  // The guest's own payload and points still read back as its own.
  scav_span const payload{ host.prims[1].payload };
  std::string_view const text{
    reinterpret_cast<char const *>(host.text.bytes.data() + payload.off), payload.len
  };
  CHECK(text == "guest");
  CHECK(host.points[host.prims[1].points.off].x == 2);
  CHECK(host.points[host.prims[2].points.off].x == 3);
  // Depth decides the interleave, not the order things were appended in.
  CHECK(host.prims[0].depth == 0);
  CHECK(host.prims[1].depth == 5);
}

TEST_CASE("drawlist: appending an empty list changes nothing") {
  DrawList host;
  uint32_t const s{ drawlist_style(host, ink(0xAA)) };
  push_rect(host, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, NONE);
  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  drawlist_canonicalize(host);
  uint32_t const before{ drawlist_digest(host, m) };

  drawlist_append(host, DrawList{});
  drawlist_canonicalize(host);
  CHECK(drawlist_digest(host, m) == before);
}

TEST_CASE("drawlist: the C surface reads every array and refuses nulls") {
  scav_drawlist *list{ nullptr };
  scav_metrics *metrics{ nullptr };
  REQUIRE(scav_drawlist_create(&list) == SCAV_OK);
  REQUIRE(scav_metrics_create(nullptr, 0, &metrics) == SCAV_OK);

  uint32_t const s{ drawlist_style(list->list, ink(0xAA)) };
  push_text(list->list, 3, s, { .x = 7, .y = 8 }, "Idle", state(4));
  list->list.prims[0].clip =
      drawlist_clip(list->list, { .x = 0, .y = 0, .w = 9, .h = 9 });

  uint32_t prims{ 0 };
  uint32_t styles{ 0 };
  uint32_t points{ 0 };
  uint32_t clips{ 0 };
  uint32_t text{ 0 };
  REQUIRE(scav_drawlist_counts(list, &prims, &styles, &points, &clips, &text) == SCAV_OK);
  CHECK(prims == 1);
  CHECK(styles == 1);
  CHECK(points == 1);
  CHECK(clips == 1);
  CHECK(text == 4);

  scav_prim const *prim_rows{ nullptr };
  scav_style const *style_rows{ nullptr };
  scav_point const *point_rows{ nullptr };
  scav_rect const *clip_rows{ nullptr };
  uint32_t count{ 0 };
  REQUIRE(scav_drawlist_prims(list, &prim_rows, &count) == SCAV_OK);
  CHECK(count == 1);
  CHECK(prim_rows[0].depth == 3);
  CHECK(prim_rows[0].kind == SCAV_PRIM_TEXT);
  REQUIRE(scav_drawlist_styles(list, &style_rows, &count) == SCAV_OK);
  CHECK(style_rows[0].stroke_rgba == 0xAA);
  REQUIRE(scav_drawlist_points(list, &point_rows, &count) == SCAV_OK);
  CHECK(point_rows[0].x == 7);
  REQUIRE(scav_drawlist_clips(list, &clip_rows, &count) == SCAV_OK);
  CHECK(clip_rows[0].w == 9);

  scav_byte const *bytes{ nullptr };
  uint32_t len{ 0 };
  REQUIRE(scav_drawlist_str(list, prim_rows[0].payload, &bytes, &len) == SCAV_OK);
  REQUIRE(len == 4);
  CHECK(std::string_view{ reinterpret_cast<char const *>(bytes), len } == "Idle");
  // Zero length reads back NULL and zero; past the pool is an error.
  REQUIRE(scav_drawlist_str(list, { .off = 0, .len = 0 }, &bytes, &len) == SCAV_OK);
  CHECK(bytes == nullptr);
  CHECK(scav_drawlist_str(list, { .off = 0, .len = 99 }, &bytes, &len) ==
        SCAV_E_INVALID_ARG);

  uint32_t bad{ 0 };
  uint32_t digest{ 0 };
  REQUIRE(scav_drawlist_validate(list, &bad) == SCAV_OK);
  CHECK(bad == INVALID);
  REQUIRE(scav_drawlist_canonicalize(list) == SCAV_OK);
  REQUIRE(scav_drawlist_digest(list, metrics, &digest) == SCAV_OK);
  CHECK(digest == drawlist_digest(list->list, metrics->metrics));

  // An invalid list is refused rather than sorted: canonicalizing one would
  // index past an array.
  list->list.prims[0].style = 9;
  CHECK(scav_drawlist_validate(list, &bad) == SCAV_E_DRAWLIST);
  CHECK(bad == 0);
  CHECK(scav_drawlist_canonicalize(list) == SCAV_E_DRAWLIST);
  list->list.prims[0].style = 0;

  scav_drawlist *other{ nullptr };
  REQUIRE(scav_drawlist_create(&other) == SCAV_OK);
  REQUIRE(scav_drawlist_append(other, list) == SCAV_OK);
  REQUIRE(scav_drawlist_counts(other, &prims, nullptr, nullptr, nullptr, nullptr) ==
          SCAV_OK);
  CHECK(prims == 1);

  CHECK(scav_drawlist_counts(nullptr, &prims, nullptr, nullptr, nullptr, nullptr) ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_drawlist_prims(list, nullptr, &count) == SCAV_E_INVALID_ARG);
  CHECK(scav_drawlist_digest(list, nullptr, &digest) == SCAV_E_INVALID_ARG);
  CHECK(scav_drawlist_append(other, nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_drawlist_create(nullptr) == SCAV_E_INVALID_ARG);

  scav_drawlist_destroy(other);
  scav_drawlist_destroy(list);
  scav_metrics_destroy(metrics);
  scav_drawlist_destroy(nullptr);  // idempotent on NULL
}

TEST_CASE("images: registration carries the dimensions, and an id names one") {
  scav_images *images{ nullptr };
  REQUIRE(scav_images_create(&images) == SCAV_OK);

  scav_byte const png[4]{ 0x89, 'P', 'N', 'G' };
  REQUIRE(scav_image_register(images, "logo", png, 4, 32, 16, "image/png") == SCAV_OK);
  uint32_t count{ 0 };
  REQUIRE(scav_image_count(images, &count) == SCAV_OK);
  CHECK(count == 1);

  uint32_t index{ 0 };
  REQUIRE(scav_image_find(images, reinterpret_cast<scav_byte const *>("logo"), 4,
                          &index) == SCAV_OK);
  CHECK(index == 0);
  scav_extent extent{};
  REQUIRE(scav_image_extent(images, 0, &extent) == SCAV_OK);
  CHECK(extent.w == 32);
  CHECK(extent.h == 16);

  // Dimensions come from registration, so a zero one is the caller's error and
  // not something to infer from the bytes later.
  CHECK(scav_image_register(images, "bad", png, 4, 0, 16, "image/png") ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_image_register(images, "bad", png, 0, 8, 16, "image/png") ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_image_register(images, "", png, 4, 8, 16, "image/png") ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_image_register(images, "logo", png, 4, 8, 8, "image/png") == SCAV_E_STATE);

  // Many rows share one pool, so an id survives the vector growing.
  for (uint32_t i = 0; i < 64; ++i) {
    std::string const id{ "img" + std::to_string(i) };
    REQUIRE(scav_image_register(images, id.c_str(), png, 4, 1, 1, "image/png") ==
            SCAV_OK);
  }
  REQUIRE(scav_image_find(images, reinterpret_cast<scav_byte const *>("logo"), 4,
                          &index) == SCAV_OK);
  CHECK(index == 0);
  CHECK(scav_image_find(images, reinterpret_cast<scav_byte const *>("none"), 4,
                        &index) == SCAV_E_INVALID_ARG);
  CHECK(scav_image_extent(images, 9999, &extent) == SCAV_E_INVALID_ARG);

  scav_images_destroy(images);
  scav_images_destroy(nullptr);
}
