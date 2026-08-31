// The corpus rendered end to end, the svg/ golden, and the one property that
// makes the whole chain trustworthy: builder and backend measure alike.

#include "scav/scav_core.h"
#include "scav/scav_draw.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_svg.h"
#include "scav/scav_types.h"

#include "scav_xxhash.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

constexpr std::array<char const *, 10> CORPUS{
  "axis.scav", "brew.scav", "dock.scav", "estop.scav",       "led.scav",
  "mill.scav", "ota.scav",  "tcp.scav",  "toolchanger.scav", "vac.scav"
};

Metrics bundled() {
  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  return m;
}

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

struct Rendered {
  Chart chart;
  DrawList list;
  std::string doc;
};

Rendered render(char const *name, Metrics const &m, scav_profile const &p) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  Rendered r;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE_MESSAGE(load_file(path.c_str(), loader, r.chart, diags, failed), path);

  Spaces spaces;
  REQUIRE_MESSAGE(measure_chart(r.chart, m, p, spaces), name);
  std::vector<scav_placed> placed;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  REQUIRE_MESSAGE(layout_run(r.chart, as_spaces(spaces), opts, placed, diags), name);
  REQUIRE_MESSAGE(emit_chart(r.list,
                             r.chart,
                             m,
                             palette_standard(),
                             as_spaces(spaces),
                             placed.data(),
                             static_cast<uint32_t>(placed.size()),
                             0),
                  name);
  drawlist_canonicalize(r.list);

  uint32_t bad{ 0 };
  SvgOptions const svg{ .embed_font = false, .margin = p.pad };
  REQUIRE_MESSAGE(svg_write(r.list, m, {}, svg, r.doc, bad) == SvgStatus::Ok, name);
  return r;
}

uint32_t count_of(std::string_view doc, std::string_view needle) {
  uint32_t n{ 0 };
  for (size_t at = doc.find(needle); at != std::string_view::npos;
       at = doc.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("svg corpus: every chart renders to the committed golden") {
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };

  // A hash and a byte count, not the documents: an SVG change should move one
  // line per chart in review, and the documents themselves live in svg/ only
  // for the one chart a human reads.
  std::string actual;
  for (char const *name : CORPUS) {
    CAPTURE(name);
    Rendered const r{ render(name, m, p) };
    actual += name;
    actual += ' ';
    string_append_hex32(
        actual,
        xxhash32(reinterpret_cast<scav_byte const *>(r.doc.data()), r.doc.size(), 0));
    actual += ' ';
    string_append_u32(actual, static_cast<uint32_t>(r.doc.size()));
    actual += '\n';
  }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/svg/corpus.txt", golden));
  std::string const want{ reinterpret_cast<char const *>(golden.data()), golden.size() };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/svg_corpus.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/svg_corpus.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("svg corpus: vac's document is committed whole, for a human to read") {
  Metrics const m{ bundled() };
  Rendered const r{ render("vac.scav", m, readable()) };

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/svg/vac.svg", golden));
  std::string const want{ reinterpret_cast<char const *>(golden.data()), golden.size() };
  if (want != r.doc) {
    write_file(SCAV_TEST_OUT_DIR "/vac.svg",
               reinterpret_cast<scav_byte const *>(r.doc.data()),
               r.doc.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/vac.svg");
  }
  CHECK(want == r.doc);
}

TEST_CASE("svg corpus: builder and backend agree on every box") {
  // The one metrics implementation, asserted rather than assumed. Every text
  // primitive's textLength has to be the width the builder measured when it
  // decided where to put that text, or the diagram lies about its own contents.
  Metrics const m{ bundled() };
  Rendered const r{ render("vac.scav", m, readable()) };

  uint32_t checked{ 0 };
  for (scav_prim const &p : r.list.prims) {
    if (p.kind != SCAV_PRIM_TEXT) { continue; }
    std::string_view const text{ reinterpret_cast<char const *>(r.list.text.bytes.data() +
                                                                p.payload.off),
                                 p.payload.len };
    if (text.empty()) { continue; }
    scav_extent ext{};
    REQUIRE(measure_text(m,
                         reinterpret_cast<scav_byte const *>(text.data()),
                         static_cast<uint32_t>(text.size()),
                         r.list.styles[p.style].font_size_grid,
                         ext) == MeasureStatus::Ok);
    std::string want{ "textLength=\"" };
    string_append_u32(want, static_cast<uint32_t>(ext.w));
    want += '"';
    CAPTURE(text);
    CHECK(r.doc.find(want) != std::string::npos);
    ++checked;
  }
  CHECK(checked > 0);
}

TEST_CASE("svg corpus: every drawn primitive reaches the document") {
  Metrics const m{ bundled() };
  Rendered const r{ render("tcp.scav", m, readable()) };

  uint32_t rects{ 0 };
  uint32_t polylines{ 0 };
  uint32_t texts{ 0 };
  for (scav_prim const &p : r.list.prims) {
    if ((p.kind == SCAV_PRIM_RECT) || (p.kind == SCAV_PRIM_RRECT)) { ++rects; }
    if (p.kind == SCAV_PRIM_POLYLINE) { ++polylines; }
    if (p.kind == SCAV_PRIM_TEXT) { ++texts; }
  }
  CHECK(count_of(r.doc, "<rect ") == rects);
  CHECK(count_of(r.doc, "<polyline ") == polylines);
  CHECK(count_of(r.doc, "<text ") == texts);
  // And nothing was dropped on the way: tcp is the long-hierarchical-edge
  // chart, and a missing polyline there is the incumbent's failure mode.
  CHECK(polylines > 0);
}

TEST_CASE("svg corpus: the document is stable across repeated renders") {
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };
  CHECK(render("mill.scav", m, p).doc == render("mill.scav", m, p).doc);
}

TEST_CASE("svg corpus: an embedded font is the only thing --embed-font adds") {
  Metrics const m{ bundled() };
  Rendered const r{ render("led.scav", m, readable()) };

  std::string embedded;
  uint32_t bad{ 0 };
  REQUIRE(svg_write(r.list,
                    m,
                    {},
                    { .embed_font = true, .margin = readable().pad },
                    embedded,
                    bad) == SvgStatus::Ok);
  // The body is byte-identical; only the defs block differs. A reader diffing
  // two renders should see the font arrive and nothing else move.
  size_t const line{ embedded.find("  <defs>") };
  REQUIRE(line != std::string::npos);
  size_t const after{ embedded.find('\n', line) + 1 };
  std::string stripped{ embedded };
  stripped.erase(line, after - line);
  CHECK(stripped == r.doc);
}
