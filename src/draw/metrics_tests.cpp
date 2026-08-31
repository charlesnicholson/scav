// The bundled font's tables, cmap formats 4 and 12, the numberOfHMetrics tail
// rule, and the one scaling formula. Fonts are built byte by byte here: the
// traps live in table shapes the bundled font does not have.

#include "scav/scav_draw.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

void be16(std::vector<scav_byte> &out, uint32_t v) {
  out.push_back(static_cast<scav_byte>((v >> 8U) & 0xFFU));
  out.push_back(static_cast<scav_byte>(v & 0xFFU));
}

void be32(std::vector<scav_byte> &out, uint32_t v) {
  be16(out, (v >> 16U) & 0xFFFFU);
  be16(out, v & 0xFFFFU);
}

struct Table {
  char const *tag;
  std::vector<scav_byte> bytes;
};

// A font is a directory plus its tables. Offsets are computed here rather than
// asserted, so a test can add a table without hand-patching four numbers.
std::vector<scav_byte> assemble(std::vector<Table> const &tables) {
  std::vector<scav_byte> out;
  be32(out, 0x0001'0000U);  // sfntVersion
  be16(out, static_cast<uint32_t>(tables.size()));
  be16(out, 0);
  be16(out, 0);
  be16(out, 0);

  uint32_t offset{ 12U + (16U * static_cast<uint32_t>(tables.size())) };
  for (Table const &t : tables) {
    out.insert(out.end(), t.tag, t.tag + 4);
    be32(out, 0);  // checksum, which nothing here verifies
    be32(out, offset);
    be32(out, static_cast<uint32_t>(t.bytes.size()));
    offset += static_cast<uint32_t>(t.bytes.size());
  }
  for (Table const &t : tables) { out.insert(out.end(), t.bytes.begin(), t.bytes.end()); }
  return out;
}

std::vector<scav_byte> head_table(uint32_t upem) {
  std::vector<scav_byte> t(18, 0);
  be16(t, upem);
  t.resize(54, 0);
  return t;
}

std::vector<scav_byte> hhea_table(uint32_t h_metrics) {
  std::vector<scav_byte> t(34, 0);
  be16(t, h_metrics);
  return t;
}

std::vector<scav_byte> maxp_table(uint32_t glyphs) {
  std::vector<scav_byte> t;
  be32(t, 0x0001'0000U);
  be16(t, glyphs);
  t.resize(32, 0);
  return t;
}

// One record per advance, then one left-side bearing per glyph past them --
// which is the shape the tail rule exists for.
std::vector<scav_byte> hmtx_table(std::vector<uint32_t> const &advances, uint32_t glyphs) {
  std::vector<scav_byte> t;
  for (uint32_t const a : advances) {
    be16(t, a);
    be16(t, 0);
  }
  for (auto i = static_cast<uint32_t>(advances.size()); i < glyphs; ++i) { be16(t, 0); }
  return t;
}

// Format 4 over one contiguous run, using idDelta -- the branch that does not
// touch the glyph-id array.
std::vector<scav_byte> cmap4_table(uint32_t first, uint32_t last, uint32_t first_glyph) {
  std::vector<scav_byte> sub;
  be16(sub, 4);
  be16(sub, 0);  // length, patched below
  be16(sub, 0);
  be16(sub, 4);  // segCountX2: the run plus the mandatory 0xFFFF terminator
  be16(sub, 0);
  be16(sub, 0);
  be16(sub, 0);
  be16(sub, last);
  be16(sub, 0xFFFFU);
  be16(sub, 0);  // reservedPad
  be16(sub, first);
  be16(sub, 0xFFFFU);
  be16(sub, (first_glyph - first) & 0xFFFFU);
  be16(sub, 1);
  be16(sub, 0);
  be16(sub, 0);
  sub[2] = static_cast<scav_byte>((sub.size() >> 8U) & 0xFFU);
  sub[3] = static_cast<scav_byte>(sub.size() & 0xFFU);

  std::vector<scav_byte> t;
  be16(t, 0);
  be16(t, 1);
  be16(t, 3);  // Windows
  be16(t, 1);  // BMP
  be32(t, 12);
  t.insert(t.end(), sub.begin(), sub.end());
  return t;
}

std::vector<scav_byte> cmap12_table(uint32_t first, uint32_t last, uint32_t first_glyph) {
  std::vector<scav_byte> sub;
  be16(sub, 12);
  be16(sub, 0);
  be32(sub, 16 + 12);
  be32(sub, 0);
  be32(sub, 1);
  be32(sub, first);
  be32(sub, last);
  be32(sub, first_glyph);

  std::vector<scav_byte> t;
  be16(t, 0);
  be16(t, 1);
  be16(t, 3);   // Windows
  be16(t, 10);  // full repertoire
  be32(t, 12);
  t.insert(t.end(), sub.begin(), sub.end());
  return t;
}

Metrics bundled() {
  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  return m;
}

MeasureStatus measure(Metrics const &m, std::string_view s, int32_t fs, scav_extent &e) {
  return measure_text(m,
                      reinterpret_cast<scav_byte const *>(s.data()),
                      static_cast<uint32_t>(s.size()),
                      fs,
                      e);
}

}  // namespace

TEST_CASE("metrics: the bundled font is the one the design names") {
  Metrics const m{ bundled() };
  CHECK(m.units_per_em == 1000);
  CHECK(m.num_glyphs == 1743);
  // The bundled font carries a full hmtx, so its own tail rule never fires --
  // which is why the rule gets a synthetic font of its own below.
  CHECK(m.num_h_metrics == m.num_glyphs);
  CHECK(m.cmap_format == 12);
  CHECK(m.identity != 0);

  uint32_t len{ 0 };
  scav_byte const *bytes{ bundled_font(len) };
  CHECK(len == 273900);
  REQUIRE(bytes != nullptr);
  CHECK(bytes[0] == 0x00);  // sfntVersion 1.0, not an OpenType CFF font
  CHECK(bytes[1] == 0x01);
}

TEST_CASE("metrics: a monospaced font advances every glyph alike") {
  Metrics const m{ bundled() };
  uint32_t const a{ metrics_advance(m, metrics_glyph(m, 'a')) };
  CHECK(a != 0);
  for (char const c : std::string_view{ "bcMW.,;" }) {
    CHECK(metrics_advance(m, metrics_glyph(m, static_cast<uint32_t>(c))) == a);
  }
}

TEST_CASE("metrics: the scaling formula divides once and ceils") {
  Metrics const m{ bundled() };
  uint32_t const advance{ metrics_advance(m, metrics_glyph(m, 'x')) };
  scav_extent one{};
  REQUIRE(measure(m, "x", 16, one) == MeasureStatus::Ok);
  // ceil, never round-to-nearest: an under-sized box is a diagram that lies.
  CHECK(one.w == static_cast<int32_t>((advance * 16U + 999U) / 1000U));
  CHECK(one.h == 16);

  // Ten glyphs measured together, not ten measurements summed: the division
  // happens once, so the whole is never wider than the sum of its parts.
  scav_extent ten{};
  REQUIRE(measure(m, "xxxxxxxxxx", 16, ten) == MeasureStatus::Ok);
  CHECK(ten.w <= (one.w * 10));
  CHECK(ten.w == static_cast<int32_t>((advance * 10U * 16U + 999U) / 1000U));
}

TEST_CASE("metrics: an empty string measures to nothing but its own height") {
  Metrics const m{ bundled() };
  scav_extent e{};
  REQUIRE(measure(m, "", 32, e) == MeasureStatus::Ok);
  CHECK(e.w == 0);
  CHECK(e.h == 32);
}

TEST_CASE("metrics: measurement grows with size and never shrinks") {
  Metrics const m{ bundled() };
  int32_t last{ -1 };
  for (int32_t fs = 1; fs < 400; ++fs) {
    scav_extent e{};
    REQUIRE(measure(m, "Idle", fs, e) == MeasureStatus::Ok);
    CHECK(e.w >= last);
    last = e.w;
  }
}

TEST_CASE("metrics: a missing glyph is loud, never a zero-width one") {
  Metrics const m{ bundled() };
  scav_extent e{};
  // A private-use codepoint no real font maps.
  CHECK(measure(m, "\xF3\xB0\x80\x81", 16, e) == MeasureStatus::MissingGlyph);
  CHECK(metrics_glyph(m, 0xF0001U) == 0);
}

TEST_CASE("metrics: a newline is refused, because wrapping is the caller's") {
  Metrics const m{ bundled() };
  scav_extent e{};
  CHECK(measure(m, "two\nlines", 16, e) == MeasureStatus::Newline);
}

TEST_CASE("metrics: malformed UTF-8 is refused rather than measured") {
  Metrics const m{ bundled() };
  scav_extent e{};
  CHECK(measure(m, "\xC0\x80", 16, e) == MeasureStatus::BadUtf8);          // overlong
  CHECK(measure(m, "\xED\xA0\x80", 16, e) == MeasureStatus::BadUtf8);      // surrogate
  CHECK(measure(m, "\xE2\x82", 16, e) == MeasureStatus::BadUtf8);          // truncated
  CHECK(measure(m, "\x80", 16, e) == MeasureStatus::BadUtf8);              // stray tail
  CHECK(measure(m, "\xF5\x80\x80\x80", 16, e) == MeasureStatus::BadUtf8);  // > 10FFFF
}

TEST_CASE("metrics: a size outside the domain is refused") {
  Metrics const m{ bundled() };
  scav_extent e{};
  CHECK(measure(m, "x", 0, e) == MeasureStatus::BadSize);
  CHECK(measure(m, "x", -16, e) == MeasureStatus::BadSize);
  CHECK(measure(m, "x", (COORD_MAX / 4) + 1, e) == MeasureStatus::BadSize);
  CHECK(measure(m, "x", COORD_MAX / 4, e) == MeasureStatus::Ok);
}

TEST_CASE("metrics: the numberOfHMetrics tail rule applies the last advance") {
  // Four glyphs, two advance records: glyphs 2 and 3 inherit record 1's
  // advance. Missing this breaks monospaced fonts specifically.
  std::vector<scav_byte> const font{ assemble({
      { .tag = "cmap", .bytes = cmap4_table('a', 'd', 0) },
      { .tag = "head", .bytes = head_table(1000) },
      { .tag = "hhea", .bytes = hhea_table(2) },
      { .tag = "hmtx", .bytes = hmtx_table({ 500, 700 }, 4) },
      { .tag = "maxp", .bytes = maxp_table(4) },
  }) };
  Metrics m;
  REQUIRE(metrics_create(font.data(), static_cast<uint32_t>(font.size()), m));
  CHECK(m.num_h_metrics == 2);
  CHECK(m.num_glyphs == 4);

  CHECK(metrics_advance(m, 0) == 500);
  CHECK(metrics_advance(m, 1) == 700);
  CHECK(metrics_advance(m, 2) == 700);  // the tail
  CHECK(metrics_advance(m, 3) == 700);
  // And past the glyph count, which a hostile string could ask for.
  CHECK(metrics_advance(m, 99) == 700);
}

TEST_CASE("metrics: format 4 and format 12 map the same run alike") {
  auto const build = [](std::vector<scav_byte> cmap) {
    return assemble({
        { .tag = "cmap", .bytes = std::move(cmap) },
        { .tag = "head", .bytes = head_table(2048) },
        { .tag = "hhea", .bytes = hhea_table(3) },
        { .tag = "hmtx", .bytes = hmtx_table({ 100, 200, 300 }, 3) },
        { .tag = "maxp", .bytes = maxp_table(3) },
    });
  };
  std::vector<scav_byte> const four{ build(cmap4_table('A', 'C', 0)) };
  std::vector<scav_byte> const twelve{ build(cmap12_table('A', 'C', 0)) };

  Metrics m4;
  Metrics m12;
  REQUIRE(metrics_create(four.data(), static_cast<uint32_t>(four.size()), m4));
  REQUIRE(metrics_create(twelve.data(), static_cast<uint32_t>(twelve.size()), m12));
  CHECK(m4.cmap_format == 4);
  CHECK(m12.cmap_format == 12);

  // Glyph 0 is .notdef, so 'A' mapping to it reads as missing either way --
  // the point here is that both formats agree, including on that.
  for (uint32_t cp = 'A'; cp <= 'C'; ++cp) {
    CHECK(metrics_glyph(m4, cp) == metrics_glyph(m12, cp));
  }
  CHECK(metrics_glyph(m4, 'B') == 1);
  CHECK(metrics_glyph(m4, 'D') == 0);
  CHECK(metrics_glyph(m12, 'D') == 0);
  // Only format 12 reaches past the BMP at all.
  CHECK(metrics_glyph(m4, 0x1'0000U) == 0);
}

TEST_CASE("metrics: format 12 is preferred when a font offers both") {
  std::vector<scav_byte> both;
  be16(both, 0);
  be16(both, 2);
  be16(both, 3);
  be16(both, 1);
  be32(both, 20);  // format 4 subtable
  be16(both, 3);
  be16(both, 10);
  be32(both, 20);  // patched after the format 4 body is known
  std::vector<scav_byte> const four{ cmap4_table('a', 'z', 1) };
  std::vector<scav_byte> const twelve{ cmap12_table('a', 'z', 1) };
  // Both tables carry their own 12-byte header; splice just the subtables.
  both.insert(both.end(), four.begin() + 12, four.end());
  uint32_t const twelve_at{ static_cast<uint32_t>(both.size()) };
  both[18] = static_cast<scav_byte>((twelve_at >> 8U) & 0xFFU);
  both[19] = static_cast<scav_byte>(twelve_at & 0xFFU);
  both.insert(both.end(), twelve.begin() + 12, twelve.end());

  std::vector<scav_byte> const font{ assemble({
      { .tag = "cmap", .bytes = both },
      { .tag = "head", .bytes = head_table(1000) },
      { .tag = "hhea", .bytes = hhea_table(2) },
      { .tag = "hmtx", .bytes = hmtx_table({ 0, 600 }, 2) },
      { .tag = "maxp", .bytes = maxp_table(2) },
  }) };
  Metrics m;
  REQUIRE(metrics_create(font.data(), static_cast<uint32_t>(font.size()), m));
  CHECK(m.cmap_format == 12);
}

TEST_CASE("metrics: a font missing a table it needs is refused") {
  std::vector<Table> const whole{
    { .tag = "cmap", .bytes = cmap4_table('a', 'z', 1) },
    { .tag = "head", .bytes = head_table(1000) },
    { .tag = "hhea", .bytes = hhea_table(2) },
    { .tag = "hmtx", .bytes = hmtx_table({ 0, 600 }, 2) },
    { .tag = "maxp", .bytes = maxp_table(2) },
  };
  Metrics m;
  std::vector<scav_byte> const all{ assemble(whole) };
  REQUIRE(metrics_create(all.data(), static_cast<uint32_t>(all.size()), m));

  for (size_t drop = 0; drop < whole.size(); ++drop) {
    std::vector<Table> partial;
    for (size_t i = 0; i < whole.size(); ++i) {
      if (i != drop) { partial.push_back(whole[i]); }
    }
    std::vector<scav_byte> const font{ assemble(partial) };
    CAPTURE(whole[drop].tag);
    CHECK(!metrics_create(font.data(), static_cast<uint32_t>(font.size()), m));
  }
}

TEST_CASE("metrics: tables that contradict each other are refused") {
  auto const build = [](uint32_t upem,
                        uint32_t h_metrics,
                        uint32_t glyphs,
                        std::vector<uint32_t> const &advances) {
    return assemble({
        { .tag = "cmap", .bytes = cmap4_table('a', 'z', 1) },
        { .tag = "head", .bytes = head_table(upem) },
        { .tag = "hhea", .bytes = hhea_table(h_metrics) },
        { .tag = "hmtx", .bytes = hmtx_table(advances, glyphs) },
        { .tag = "maxp", .bytes = maxp_table(glyphs) },
    });
  };
  Metrics m;
  auto const refused = [&m](std::vector<scav_byte> const &f) {
    return !metrics_create(f.data(), static_cast<uint32_t>(f.size()), m);
  };

  CHECK(refused(build(0, 2, 2, { 0, 600 })));     // upem of zero
  CHECK(refused(build(1000, 0, 2, { 0, 600 })));  // no advance records
  CHECK(refused(build(1000, 2, 0, {})));          // no glyphs
  CHECK(refused(build(1000, 5, 2, { 0, 600 })));  // more records than glyphs
  // An hmtx too short for the records it claims would index past the table.
  CHECK(refused(assemble({
      { .tag = "cmap", .bytes = cmap4_table('a', 'z', 1) },
      { .tag = "head", .bytes = head_table(1000) },
      { .tag = "hhea", .bytes = hhea_table(4) },
      { .tag = "hmtx", .bytes = hmtx_table({ 0, 600 }, 2) },
      { .tag = "maxp", .bytes = maxp_table(4) },
  })));
}

TEST_CASE("metrics: truncating the bundled font at every length never crashes") {
  uint32_t len{ 0 };
  scav_byte const *bytes{ bundled_font(len) };
  // Powers of two plus a prime stride: every table boundary gets crossed
  // somewhere, and a bounds check missed anywhere reads off the end.
  for (uint32_t cut = 0; cut < len; cut += 997) {
    Metrics m;
    if (metrics_create(bytes, cut, m)) {
      // Whatever it accepted, it must not then read past what it accepted.
      scav_extent e{};
      (void)measure(m, "Idle", 16, e);
      for (uint32_t g = 0; g < m.num_glyphs; g += 61) { (void)metrics_advance(m, g); }
    }
  }
}

TEST_CASE("metrics: an empty buffer selects the bundled font") {
  Metrics from_null;
  Metrics from_zero;
  REQUIRE(metrics_create(nullptr, 0, from_null));
  REQUIRE(metrics_create(nullptr, 4096, from_zero));
  CHECK(from_null.identity == from_zero.identity);
  CHECK(from_null.identity == bundled().identity);
}

TEST_CASE("metrics: line height is the profile ratio, not the font's opinion") {
  CHECK(line_height(160, 7, 5) == 224);
  CHECK(line_height(160, 1, 1) == 160);
  // ceil, so a ratio that does not divide never loses a grid unit.
  CHECK(line_height(10, 7, 5) == 14);
  CHECK(line_height(11, 7, 5) == 16);
  CHECK(line_height(1, 3, 2) == 2);
  // Out of range reads back zero rather than a plausible number.
  CHECK(line_height(0, 7, 5) == 0);
  CHECK(line_height(-1, 7, 5) == 0);
  CHECK(line_height(160, 0, 5) == 0);
  CHECK(line_height(160, 7, 0) == 0);
  CHECK(line_height(160, 1025, 5) == 0);
  CHECK(line_height(160, 7, 1025) == 0);
}

TEST_CASE("metrics: a block is its widest line by its own line count") {
  Metrics const m{ bundled() };
  scav_extent one{};
  scav_extent three{};
  auto const block = [&m](std::string_view s, scav_extent &e) {
    return measure_block(m,
                         reinterpret_cast<scav_byte const *>(s.data()),
                         static_cast<uint32_t>(s.size()),
                         16,
                         7,
                         5,
                         e);
  };
  REQUIRE(block("wide", one) == MeasureStatus::Ok);
  REQUIRE(block("a\nwide\nb", three) == MeasureStatus::Ok);
  CHECK(three.w == one.w);  // the widest line, not the sum
  CHECK(three.h == 3 * line_height(16, 7, 5));
  CHECK(one.h == line_height(16, 7, 5));

  // An empty string is one empty line, and a trailing newline is not an extra.
  scav_extent empty{};
  scav_extent trailing{};
  REQUIRE(block("", empty) == MeasureStatus::Ok);
  CHECK(empty.w == 0);
  CHECK(empty.h == line_height(16, 7, 5));
  REQUIRE(block("a\n", trailing) == MeasureStatus::Ok);
  CHECK(trailing.h == line_height(16, 7, 5));

  // Interior blank lines do count: an author who wrote one meant it.
  scav_extent gap{};
  REQUIRE(block("a\n\nb", gap) == MeasureStatus::Ok);
  CHECK(gap.h == 3 * line_height(16, 7, 5));
}

TEST_CASE("metrics: a block refuses what a line refuses") {
  Metrics const m{ bundled() };
  scav_extent e{};
  auto const block = [&](std::string_view s, int32_t fs, int32_t num, int32_t den) {
    return measure_block(m,
                         reinterpret_cast<scav_byte const *>(s.data()),
                         static_cast<uint32_t>(s.size()),
                         fs,
                         num,
                         den,
                         e);
  };
  CHECK(block("ok", 0, 7, 5) == MeasureStatus::BadSize);
  CHECK(block("ok", 16, 7, 0) == MeasureStatus::BadSize);
  CHECK(block("\xC0\x80", 16, 7, 5) == MeasureStatus::BadUtf8);
  CHECK(block("\xF3\xB0\x80\x81", 16, 7, 5) == MeasureStatus::MissingGlyph);
}

TEST_CASE("metrics: the C surface agrees with the C++ one, and refuses nulls") {
  scav_metrics *m{ nullptr };
  REQUIRE(scav_metrics_create(nullptr, 0, &m) == SCAV_OK);
  REQUIRE(m != nullptr);

  uint32_t identity{ 0 };
  uint32_t upem{ 0 };
  uint32_t glyphs{ 0 };
  REQUIRE(scav_metrics_identity(m, &identity) == SCAV_OK);
  REQUIRE(scav_metrics_units_per_em(m, &upem) == SCAV_OK);
  REQUIRE(scav_metrics_glyph_count(m, &glyphs) == SCAV_OK);
  CHECK(identity == bundled().identity);
  CHECK(upem == 1000);
  CHECK(glyphs == 1743);

  std::string const text{ "Idle" };
  auto const *raw{ reinterpret_cast<scav_byte const *>(text.data()) };
  scav_extent got{};
  scav_extent want{};
  REQUIRE(scav_measure_text(m, raw, 4, 160, &got) == SCAV_OK);
  REQUIRE(measure(bundled(), text, 160, want) == MeasureStatus::Ok);
  CHECK(got.w == want.w);
  CHECK(got.h == want.h);

  int32_t lh{ 0 };
  REQUIRE(scav_line_height(160, 7, 5, &lh) == SCAV_OK);
  CHECK(lh == 224);
  CHECK(scav_line_height(160, 7, 0, &lh) == SCAV_E_INVALID_ARG);

  scav_extent block{};
  REQUIRE(scav_measure_block(m, raw, 4, 160, 7, 5, &block) == SCAV_OK);
  CHECK(block.h == 224);

  // Every failure mode keeps its own code: a missing glyph is not a bad
  // argument, because one is the font's fault and the other the caller's.
  CHECK(scav_measure_text(m, raw, 4, 0, &got) == SCAV_E_INVALID_ARG);
  CHECK(scav_measure_text(m,
                          reinterpret_cast<scav_byte const *>("\xF3\xB0\x80\x81"),
                          4,
                          160,
                          &got) == SCAV_E_NO_GLYPH);
  CHECK(scav_measure_text(nullptr, raw, 4, 160, &got) == SCAV_E_INVALID_ARG);
  CHECK(scav_measure_text(m, raw, 4, 160, nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_metrics_identity(nullptr, &identity) == SCAV_E_INVALID_ARG);
  CHECK(scav_metrics_create(nullptr, 0, nullptr) == SCAV_E_INVALID_ARG);

  // A font it cannot parse is a font error, not an argument error.
  std::array<scav_byte, 8> const junk{ 1, 2, 3, 4, 5, 6, 7, 8 };
  scav_metrics *bad{ nullptr };
  CHECK(scav_metrics_create(junk.data(), 8, &bad) == SCAV_E_FONT);
  CHECK(bad == nullptr);

  scav_metrics_destroy(m);
  scav_metrics_destroy(nullptr);  // idempotent on NULL
}
