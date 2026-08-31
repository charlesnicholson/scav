// One primitive at a time: the viewBox, per-kind element shapes, escaping,
// classes, textLength, and every refusal.

#include "scav/scav_svg.h"

#include "draw/handles.h"
#include "scav/scav_core.h"
#include "scav/scav_draw.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

constexpr ElemRef NONE{ .kind = ElemKind::None, .ordinal = INVALID };

ElemRef state(uint32_t i) { return { .kind = ElemKind::State, .ordinal = i }; }

Metrics bundled() {
  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  return m;
}

scav_style shape(uint32_t stroke, uint32_t fill) {
  return { .stroke_rgba = stroke,
           .fill_rgba = fill,
           .stroke_w = 16,
           .dash = 0,
           .font_size_grid = 0 };
}

scav_style glyphs(int32_t size) {
  return { .stroke_rgba = 0,
           .fill_rgba = 0x000000FFU,
           .stroke_w = 0,
           .dash = 0,
           .font_size_grid = size };
}

// The document, or the status that refused to write one.
struct Written {
  SvgStatus status;
  uint32_t bad;
  std::string doc;
};

Written write(DrawList const &d, SvgOptions const &o = {}, Images const &i = {}) {
  Written w{ .status = SvgStatus::Ok, .bad = 0, .doc = {} };
  w.status = svg_write(d, bundled(), i, o, w.doc, w.bad);
  return w;
}

bool has(std::string_view doc, std::string_view want) {
  return doc.find(want) != std::string_view::npos;
}

}  // namespace

TEST_CASE("svg: an empty list is a well-formed document with an empty viewBox") {
  Written const w{ write(DrawList{}) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(w.doc.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg"));
  CHECK(has(w.doc, "viewBox=\"0 0 0 0\""));
  CHECK(w.doc.ends_with("</svg>\n"));
  CHECK(has(w.doc, "xmlns=\"http://www.w3.org/2000/svg\""));
}

TEST_CASE("svg: no float is printed, ever") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x112233FFU, 0xFFFFFFFFU)) };
  push_rrect(d, 0, s, { .x = -37, .y = 5, .w = 100, .h = 41 }, 7, state(0));
  push_text(d, 0, drawlist_style(d, glyphs(160)), { .x = 1, .y = 2 }, "Idle", state(0));
  Written const w{ write(d, { .embed_font = false, .margin = 13 }) };
  REQUIRE(w.status == SvgStatus::Ok);

  // No digit in the body is followed by a decimal point and another digit, and
  // nothing carries an exponent. The XML declaration's own `version="1.0"` is
  // the one literal that looks like one, so the scan starts after it.
  auto const digit = [](char c) { return (c >= '0') && (c <= '9'); };
  size_t const body{ w.doc.find("<svg") };
  REQUIRE(body != std::string::npos);
  for (size_t i = body + 1; (i + 1) < w.doc.size(); ++i) {
    CAPTURE(w.doc.substr((i > 12) ? (i - 12) : 0, 28));
    if ((w.doc[i] == '.') && digit(w.doc[i - 1])) { CHECK(!digit(w.doc[i + 1])); }
    if (((w.doc[i] == 'e') || (w.doc[i] == 'E')) && digit(w.doc[i - 1])) {
      CHECK(((w.doc[i + 1] != '+') && (w.doc[i + 1] != '-')));
    }
  }
}

TEST_CASE("svg: the viewBox carries the extent and the frame is whole points") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x000000FFU, 0)) };
  // 160 x 96 grid units at (32, 16), so ten by six points.
  push_rect(d, 0, s, { .x = 32, .y = 16, .w = 160, .h = 96 }, NONE);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(has(w.doc, "viewBox=\"32 16 160 96\""));
  CHECK(has(w.doc, "width=\"10\""));
  CHECK(has(w.doc, "height=\"6\""));

  // A margin widens both, and the origin moves back by it.
  Written const margined{ write(d, { .embed_font = false, .margin = 8 }) };
  REQUIRE(margined.status == SvgStatus::Ok);
  CHECK(has(margined.doc, "viewBox=\"24 8 176 112\""));
  CHECK(has(margined.doc, "width=\"11\""));

  // An extent that does not divide into whole points is ceiled, so the frame
  // can only ever be a sliver large.
  DrawList odd;
  push_rect(odd,
            0,
            drawlist_style(odd, shape(0x000000FFU, 0)),
            { .x = 0, .y = 0, .w = 17, .h = 1 },
            NONE);
  Written const ceiled{ write(odd) };
  REQUIRE(ceiled.status == SvgStatus::Ok);
  CHECK(has(ceiled.doc, "width=\"2\""));
  CHECK(has(ceiled.doc, "height=\"1\""));
}

TEST_CASE("svg: bounds are the tight box, and a circle reaches its radius") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x000000FFU, 0)) };
  CHECK(svg_bounds(d).w == 0);

  push_rect(d, 0, s, { .x = 10, .y = 10, .w = 10, .h = 10 }, NONE);
  scav_rect const one{ svg_bounds(d) };
  CHECK(one.x == 10);
  CHECK(one.w == 10);

  // The circle's centre is inside the rect but its radius is not, so the box
  // has to grow: a backend sizing to centres would clip it.
  push_circle(d, 0, s, { .x = 15, .y = 15 }, 40, NONE);
  scav_rect const two{ svg_bounds(d) };
  CHECK(two.x == -25);
  CHECK(two.y == -25);
  CHECK(two.w == 80);
}

TEST_CASE("svg: each kind lands as the element a viewer expects") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0xAABBCCFFU, 0x102030FFU)) };
  scav_point const pts[3]{ { .x = 0, .y = 0 }, { .x = 10, .y = 0 }, { .x = 10, .y = 10 } };

  push_rect(d, 0, s, { .x = 1, .y = 2, .w = 3, .h = 4 }, NONE);
  push_rrect(d, 0, s, { .x = 1, .y = 2, .w = 3, .h = 4 }, 2, NONE);
  push_line(d, 0, s, pts[0], pts[1], NONE);
  push_polyline(d, 0, s, pts, 3, NONE);
  push_path(d, 0, s, pts, 3, NONE);
  push_circle(d, 0, s, pts[0], 5, NONE);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);

  CHECK(has(w.doc, "<rect x=\"1\" y=\"2\" width=\"3\" height=\"4\" fill="));
  CHECK(has(w.doc, "rx=\"2\""));
  CHECK(has(w.doc, "<line x1=\"0\" y1=\"0\" x2=\"10\" y2=\"0\""));
  CHECK(has(w.doc, "<polyline points=\"0,0 10,0 10,10\""));
  CHECK(has(w.doc, "<polygon points=\"0,0 10,0 10,10\""));
  CHECK(has(w.doc, "<circle cx=\"0\" cy=\"0\" r=\"5\""));
  // Colours are #rrggbb: the CSS Color 4 alpha pair is ignored rather than
  // refused by older consumers, which is worse than not using it.
  CHECK(has(w.doc, "fill=\"#102030\""));
  CHECK(has(w.doc, "stroke=\"#aabbcc\""));
  CHECK(has(w.doc, "stroke-width=\"16\""));
  // An open run is a stroke, so it must not be filled.
  CHECK(has(w.doc, "<polyline points=\"0,0 10,0 10,10\" fill=\"none\""));
}

TEST_CASE("svg: a transparent paint is none, and a partial one is a fixed decimal") {
  DrawList d;
  push_rect(d,
            0,
            drawlist_style(d, shape(0x000000FFU, 0x11223300U)),
            { .x = 0, .y = 0, .w = 1, .h = 1 },
            NONE);
  push_rect(d,
            0,
            drawlist_style(d, shape(0x000000FFU, 0x11223380U)),
            { .x = 0, .y = 0, .w = 1, .h = 1 },
            NONE);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(has(w.doc, "fill=\"none\""));
  // 0x80 of 255 is 501 thousandths, assembled from integer division rather
  // than converted from a double.
  CHECK(has(w.doc, "fill-opacity=\"0.501\""));
}

TEST_CASE("svg: a dashed style becomes a dasharray scaled to its stroke") {
  DrawList d;
  scav_style dashed{ shape(0x808080FFU, 0) };
  dashed.dash = 1;
  push_line(d, 0, drawlist_style(d, dashed), { .x = 0, .y = 0 }, { .x = 8, .y = 0 }, NONE);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(has(w.doc, "stroke-dasharray=\"64,48\""));
}

TEST_CASE("svg: text carries textLength from our own advance sum") {
  DrawList d;
  push_text(d, 0, drawlist_style(d, glyphs(160)), { .x = 5, .y = 40 }, "Idle", state(7));
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);

  scav_extent want{};
  REQUIRE(
      measure_text(bundled(), reinterpret_cast<scav_byte const *>("Idle"), 4, 160, want) ==
      MeasureStatus::Ok);
  std::string expected{ "textLength=\"" };
  string_append_u32(expected, static_cast<uint32_t>(want.w));
  expected += '"';
  CHECK(has(w.doc, expected));
  // spacing, not glyph scaling: a substituted font goes slightly loose rather
  // than overflowing its box.
  CHECK(has(w.doc, "lengthAdjust=\"spacing\""));
  CHECK(has(w.doc, "font-size=\"160\""));
  CHECK(has(w.doc, ">Idle</text>"));
  // One bundled font, named with a fallback, and kerning off on both sides.
  CHECK(has(w.doc, "font-family: \"JetBrains Mono\", monospace"));
  CHECK(has(w.doc, "font-kerning: none"));
}

TEST_CASE("svg: empty text carries no textLength to be zero about") {
  DrawList d;
  push_text(d, 0, drawlist_style(d, glyphs(160)), { .x = 0, .y = 0 }, "", NONE);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(!has(w.doc, "textLength"));
}

TEST_CASE("svg: the five predefined entities are escaped and nothing else is") {
  DrawList d;
  push_text(d,
            0,
            drawlist_style(d, glyphs(160)),
            { .x = 0, .y = 0 },
            "a&b<c>d\"e'f",
            NONE);
  // Multi-byte UTF-8 passes through: SVG is UTF-8 and the pool is NFC.
  push_text(d,
            0,
            drawlist_style(d, glyphs(160)),
            { .x = 0, .y = 0 },
            "na\xc3\xafve",
            NONE);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(has(w.doc, "a&amp;b&lt;c&gt;d&quot;e&apos;f"));
  CHECK(has(w.doc, "na\xc3\xafve"));
}

TEST_CASE("svg: a class is synthesized from the origin, never carried in the IR") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x000000FFU, 0)) };
  push_rect(d, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, state(1234));
  push_rect(d,
            0,
            s,
            { .x = 0, .y = 0, .w = 1, .h = 1 },
            { .kind = ElemKind::Transition, .ordinal = 7 });
  push_rect(d, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, NONE);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(has(w.doc, "class=\"scav-state scav-id-1234\""));
  CHECK(has(w.doc, "class=\"scav-trans scav-id-7\""));

  CHECK(svg_class(d.prims[0]) == "scav-state scav-id-1234");
  CHECK(svg_class(d.prims[1]) == "scav-trans scav-id-7");
  // A primitive belonging to no entity gets no class rather than an empty one.
  CHECK(svg_class(d.prims[2]).empty());
  CHECK(!has(w.doc, "class=\"\""));
}

TEST_CASE("svg: every element kind carries its class") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x000000FFU, 0)) };
  scav_point const pts[2]{ { .x = 0, .y = 0 }, { .x = 1, .y = 1 } };
  push_rect(d, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, state(0));
  push_line(d, 0, s, pts[0], pts[1], state(1));
  push_polyline(d, 0, s, pts, 2, state(2));
  push_path(d, 0, s, pts, 2, state(3));
  push_circle(d, 0, s, pts[0], 1, state(4));
  push_text(d, 0, drawlist_style(d, glyphs(160)), pts[0], "x", state(5));
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  for (uint32_t i = 0; i < 6; ++i) {
    std::string want{ "scav-id-" };
    string_append_u32(want, i);
    CAPTURE(i);
    CHECK(has(w.doc, want + "\""));
  }
}

TEST_CASE("svg: --embed-font base64s the bundled TTF whole") {
  DrawList d;
  push_text(d, 0, drawlist_style(d, glyphs(160)), { .x = 0, .y = 0 }, "x", NONE);
  Written const bare{ write(d) };
  Written const embedded{ write(d, { .embed_font = true, .margin = 0 }) };
  REQUIRE(bare.status == SvgStatus::Ok);
  REQUIRE(embedded.status == SvgStatus::Ok);
  CHECK(!has(bare.doc, "@font-face"));
  CHECK(has(embedded.doc, "<defs><style>@font-face"));
  CHECK(has(embedded.doc, "src: url(data:font/ttf;base64,"));
  CHECK(has(embedded.doc, "format(\"truetype\")"));

  // Whole, not subsetted: base64 is four characters per three bytes.
  uint32_t len{ 0 };
  (void)bundled_font(len);
  CHECK(embedded.doc.size() > (bare.doc.size() + ((len / 3U) * 4U)));
  // And never converted to paths, which would discard selection.
  CHECK(!has(embedded.doc, "<path"));
}

TEST_CASE("svg: an image goes inline with the mime it was registered under") {
  Images images;
  scav_byte const png[5]{ 0x89, 'P', 'N', 'G', 0x0D };
  REQUIRE(image_register(images, "logo", png, 5, 16, 16, "image/png"));

  DrawList d;
  push_image(d,
             0,
             drawlist_style(d, shape(0, 0)),
             { .x = 0, .y = 0, .w = 32, .h = 32 },
             "logo",
             NONE);
  Written const w{ write(d, {}, images) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(has(w.doc, "<image x=\"0\" y=\"0\" width=\"32\" height=\"32\""));
  CHECK(has(w.doc, "href=\"data:image/png;base64,"));
  CHECK(has(w.doc, "iVBORw0"));  // the five bytes, padded
}

TEST_CASE("svg: base64 covers all three tail lengths") {
  auto const encoded = [](std::string_view bytes) {
    Images images;
    REQUIRE(image_register(images,
                           "i",
                           reinterpret_cast<scav_byte const *>(bytes.data()),
                           static_cast<uint32_t>(bytes.size()),
                           1,
                           1,
                           "application/x"));
    DrawList d;
    push_image(d,
               0,
               drawlist_style(d, shape(0, 0)),
               { .x = 0, .y = 0, .w = 1, .h = 1 },
               "i",
               NONE);
    Written const w{ write(d, {}, images) };
    REQUIRE(w.status == SvgStatus::Ok);
    size_t const at{ w.doc.find("base64,") + 7 };
    return w.doc.substr(at, w.doc.find('"', at) - at);
  };
  CHECK(encoded("M") == "TQ==");
  CHECK(encoded("Ma") == "TWE=");
  CHECK(encoded("Man") == "TWFu");
  CHECK(encoded("Many") == "TWFueQ==");
}

TEST_CASE("svg: an unrenderable primitive is refused, and names itself") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x000000FFU, 0)) };
  push_rect(d, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, NONE);
  push_arc(d, 0, s, { .x = 0, .y = 0, .w = 8, .h = 8 }, 0, 90 * 64, NONE);
  Written const w{ write(d) };
  // An arc needs endpoints, and deriving them from an angle needs trigonometry
  // no integer path here supplies. Refused rather than approximated.
  CHECK(w.status == SvgStatus::UnsupportedPrim);
  CHECK(w.bad == 1);
  CHECK(w.doc.empty());  // nothing half-written
}

TEST_CASE("svg: an image naming nothing registered is refused") {
  DrawList d;
  push_image(d,
             0,
             drawlist_style(d, shape(0, 0)),
             { .x = 0, .y = 0, .w = 1, .h = 1 },
             "absent",
             NONE);
  Written const w{ write(d) };
  CHECK(w.status == SvgStatus::UnknownImage);
  CHECK(w.bad == 0);
  CHECK(w.doc.empty());
}

TEST_CASE("svg: an invalid drawlist is refused before anything is measured") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x000000FFU, 0)) };
  push_rect(d, 0, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, NONE);
  d.prims[0].style = 9;
  Written const w{ write(d) };
  CHECK(w.status == SvgStatus::InvalidDrawList);
  CHECK(w.bad == 0);
  CHECK(w.doc.empty());
}

TEST_CASE("svg: text the font cannot render is refused, not silently narrowed") {
  DrawList d;
  push_text(d,
            0,
            drawlist_style(d, glyphs(160)),
            { .x = 0, .y = 0 },
            "\xF3\xB0\x80\x81",
            NONE);
  Written const w{ write(d) };
  CHECK(w.status == SvgStatus::MissingGlyph);
  CHECK(w.doc.empty());
}

TEST_CASE("svg: an extent past an integer viewBox is refused") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x000000FFU, 0)) };
  push_rect(d, 0, s, { .x = -COORD_MAX, .y = 0, .w = COORD_MAX, .h = 1 }, NONE);
  push_rect(d, 0, s, { .x = COORD_MAX - 1, .y = 0, .w = 1, .h = 1 }, NONE);
  Written const w{ write(d) };
  CHECK(w.status == SvgStatus::ExtentOverflow);
  CHECK(w.doc.empty());
}

TEST_CASE("svg: a negative coordinate prints its own sign") {
  DrawList d;
  push_rect(d,
            0,
            drawlist_style(d, shape(0x000000FFU, 0)),
            { .x = -100, .y = -50, .w = 10, .h = 10 },
            NONE);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  CHECK(has(w.doc, "viewBox=\"-100 -50 10 10\""));
  CHECK(has(w.doc, "x=\"-100\" y=\"-50\""));
}

TEST_CASE("svg: writing twice appends rather than replacing") {
  DrawList d;
  push_rect(d,
            0,
            drawlist_style(d, shape(0x000000FFU, 0)),
            { .x = 0, .y = 0, .w = 1, .h = 1 },
            NONE);
  std::string out{ "prefix" };
  uint32_t bad{ 0 };
  REQUIRE(svg_write(d, bundled(), {}, {}, out, bad) == SvgStatus::Ok);
  CHECK(out.starts_with("prefix<?xml"));
}

TEST_CASE("svg: depth order is emission order, since the list is canonical") {
  DrawList d;
  uint32_t const s{ drawlist_style(d, shape(0x000000FFU, 0)) };
  push_rect(d, 5, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, state(1));
  push_rect(d, 1, s, { .x = 0, .y = 0, .w = 1, .h = 1 }, state(0));
  drawlist_canonicalize(d);
  Written const w{ write(d) };
  REQUIRE(w.status == SvgStatus::Ok);
  // Painter's algorithm: whatever sorts first is written first, so the deeper
  // primitive is behind. The backend does not re-sort; canonical form did.
  CHECK(w.doc.find("scav-id-0") < w.doc.find("scav-id-1"));
}

TEST_CASE("svg: the C surface queries then writes, and refuses nulls") {
  scav_drawlist *list{ nullptr };
  scav_metrics *metrics{ nullptr };
  REQUIRE(scav_drawlist_create(&list) == SCAV_OK);
  REQUIRE(scav_metrics_create(nullptr, 0, &metrics) == SCAV_OK);
  push_text(list->list,
            0,
            drawlist_style(list->list, glyphs(160)),
            { .x = 0, .y = 0 },
            "Idle",
            state(3));

  uint32_t count{ 0 };
  REQUIRE(scav_svg_write(list, metrics, nullptr, nullptr, nullptr, 0, &count) == SCAV_OK);
  REQUIRE(count > 0);

  std::vector<scav_byte> buffer(count);
  // A cap too small writes the required count rather than truncating.
  uint32_t again{ 0 };
  CHECK(
      scav_svg_write(list, metrics, nullptr, nullptr, buffer.data(), count - 1, &again) ==
      SCAV_E_CAPACITY);
  CHECK(again == count);
  REQUIRE(scav_svg_write(list, metrics, nullptr, nullptr, buffer.data(), count, &again) ==
          SCAV_OK);
  std::string const doc{ reinterpret_cast<char const *>(buffer.data()), count };
  CHECK(doc.starts_with("<?xml"));
  CHECK(has(doc, "scav-id-3"));

  scav_svg_options opts{ .embed_font = 1, .margin = 4 };
  REQUIRE(scav_svg_write(list, metrics, nullptr, &opts, nullptr, 0, &again) == SCAV_OK);
  CHECK(again > count);
  opts.embed_font = 2;
  CHECK(scav_svg_write(list, metrics, nullptr, &opts, nullptr, 0, &again) ==
        SCAV_E_INVALID_ARG);

  scav_rect bounds{};
  REQUIRE(scav_svg_bounds(list, &bounds) == SCAV_OK);
  CHECK(bounds.w == 0);  // one text primitive is one point

  CHECK(scav_svg_write(nullptr, metrics, nullptr, nullptr, nullptr, 0, &again) ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_svg_write(list, nullptr, nullptr, nullptr, nullptr, 0, &again) ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_svg_write(list, metrics, nullptr, nullptr, nullptr, 0, nullptr) ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_svg_write(list, metrics, nullptr, nullptr, nullptr, count, &again) ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_svg_bounds(nullptr, &bounds) == SCAV_E_INVALID_ARG);

  // An unrenderable primitive is one code, whatever made it unrenderable.
  push_arc(list->list,
           0,
           drawlist_style(list->list, shape(0x000000FFU, 0)),
           { .x = 0, .y = 0, .w = 1, .h = 1 },
           0,
           64,
           NONE);
  CHECK(scav_svg_write(list, metrics, nullptr, nullptr, nullptr, 0, &again) ==
        SCAV_E_DRAWLIST);

  scav_metrics_destroy(metrics);
  scav_drawlist_destroy(list);
}
