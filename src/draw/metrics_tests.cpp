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

// The size the C surface checks against, as this build measures it.
constexpr uint32_t EXTENT_SIZE{ static_cast<uint32_t>(sizeof(scav_extent)) };

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

// One cmap table wrapping one subtable under a platform and encoding the caller
// chooses: those two are all the picker ranks a subtable by.
std::vector<scav_byte> cmap_wrap(uint32_t platform,
                                 uint32_t encoding,
                                 std::vector<scav_byte> const &sub) {
  std::vector<scav_byte> t;
  be16(t, 0);
  be16(t, 1);
  be16(t, platform);
  be16(t, encoding);
  be32(t, 12);
  t.insert(t.end(), sub.begin(), sub.end());
  return t;
}

struct Seg {
  uint32_t end;
  uint32_t start;
  uint32_t delta;  // idDelta, two's complement in sixteen bits
  uint32_t range;  // idRangeOffset, a byte offset from its own slot
};

// Format 4 over segments written verbatim -- the 0xFFFF terminator included
// only if the caller asks -- then the glyph id array the range offsets index.
std::vector<scav_byte> cmap4_sub(std::vector<Seg> const &segs,
                                 std::vector<uint32_t> const &glyphs) {
  std::vector<scav_byte> sub;
  be16(sub, 4);
  be16(sub, 0);  // length, patched below
  be16(sub, 0);
  be16(sub, 2U * static_cast<uint32_t>(segs.size()));
  be16(sub, 0);
  be16(sub, 0);
  be16(sub, 0);
  for (Seg const &s : segs) { be16(sub, s.end); }
  be16(sub, 0);  // reservedPad
  for (Seg const &s : segs) { be16(sub, s.start); }
  for (Seg const &s : segs) { be16(sub, s.delta); }
  for (Seg const &s : segs) { be16(sub, s.range); }
  for (uint32_t const g : glyphs) { be16(sub, g); }
  sub[2] = static_cast<scav_byte>((sub.size() >> 8U) & 0xFFU);
  sub[3] = static_cast<scav_byte>(sub.size() & 0xFFU);
  return sub;
}

// An idRangeOffset that reaches glyph `slot` of the array from segment `i` of
// `n`: the slot is read relative to where the offset itself was read from.
uint32_t range_offset(uint32_t n, uint32_t i, uint32_t slot) {
  return 2U * ((n - i) + slot);
}

// The cmap goes last, so cutting the cmap table cuts the file: every bounds
// check reads against the file's length rather than the table's.
std::vector<scav_byte> font_with_cmap(std::vector<scav_byte> cmap, uint32_t cut) {
  if (cut < cmap.size()) { cmap.resize(cut); }
  return assemble({
      { .tag = "head", .bytes = head_table(1000) },
      { .tag = "hhea", .bytes = hhea_table(2) },
      { .tag = "hmtx", .bytes = hmtx_table({ 500, 600 }, 8) },
      { .tag = "maxp", .bytes = maxp_table(8) },
      { .tag = "cmap", .bytes = std::move(cmap) },
  });
}

Metrics from_bytes(std::vector<scav_byte> const &font) {
  Metrics m;
  REQUIRE(metrics_create(font.data(), static_cast<uint32_t>(font.size()), m));
  return m;
}

bool refuses(std::vector<scav_byte> const &font) {
  Metrics m;
  return !metrics_create(font.data(), static_cast<uint32_t>(font.size()), m);
}

uint32_t rd16(std::vector<scav_byte> const &b, uint32_t at) {
  return (static_cast<uint32_t>(b[at]) << 8U) | b[at + 1U];
}

uint32_t rd32(std::vector<scav_byte> const &b, uint32_t at) {
  return (rd16(b, at) << 16U) | rd16(b, at + 2U);
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
  CHECK(measure(m, "\xC2\x41", 16, e) == MeasureStatus::BadUtf8);  // no continuation
  // No bytes at all where a length says there are some.
  CHECK(measure_text(m, nullptr, 5, 16, e) == MeasureStatus::BadUtf8);
  CHECK(e.w == 0);
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
  // And a product past the coordinate domain, which every bound above is in.
  CHECK(line_height(COORD_MAX, 1024, 1) == 0);
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
  // Two lines of the tallest line height there is overflows the height a line
  // of it does not, so the sum is checked and not just each term.
  CHECK(block("\n\n", COORD_MAX / 4, 4, 1) == MeasureStatus::BadSize);

  // No bytes at all where a length says there are some, which measure_text
  // refuses and this used to read through.
  CHECK(measure_block(m, nullptr, 5, 16, 7, 5, e) == MeasureStatus::BadUtf8);
  CHECK(e.w == 0);
  CHECK(e.h == 0);
  CHECK(measure_block(m, nullptr, 0, 16, 7, 5, e) == MeasureStatus::Ok);
  CHECK(e.h == line_height(16, 7, 5));
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
  REQUIRE(scav_measure_text(m, raw, 4, 160, &got, EXTENT_SIZE) == SCAV_OK);
  REQUIRE(measure(bundled(), text, 160, want) == MeasureStatus::Ok);
  CHECK(got.w == want.w);
  CHECK(got.h == want.h);

  int32_t lh{ 0 };
  REQUIRE(scav_line_height(160, 7, 5, &lh) == SCAV_OK);
  CHECK(lh == 224);
  CHECK(scav_line_height(160, 7, 0, &lh) == SCAV_E_INVALID_ARG);

  scav_extent block{};
  REQUIRE(scav_measure_block(m, raw, 4, 160, 7, 5, &block, EXTENT_SIZE) == SCAV_OK);
  CHECK(block.h == 224);

  // Every failure mode keeps its own code: a missing glyph is not a bad
  // argument, because one is the font's fault and the other the caller's.
  CHECK(scav_measure_text(m, raw, 4, 0, &got, EXTENT_SIZE) == SCAV_E_INVALID_ARG);
  CHECK(scav_measure_text(m,
                          reinterpret_cast<scav_byte const *>("\xF3\xB0\x80\x81"),
                          4,
                          160,
                          &got,
                          EXTENT_SIZE) == SCAV_E_NO_GLYPH);
  CHECK(scav_measure_text(nullptr, raw, 4, 160, &got, EXTENT_SIZE) == SCAV_E_INVALID_ARG);
  CHECK(scav_measure_text(m, raw, 4, 160, nullptr, EXTENT_SIZE) == SCAV_E_INVALID_ARG);
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

TEST_CASE("metrics: a table directory that runs off the end is refused") {
  // Sized once and filled from the front, so nothing is resized after a push.
  auto const header = [](uint32_t tables, uint32_t bytes) {
    std::vector<scav_byte> head;
    be32(head, 0x0001'0000U);
    be16(head, tables);
    std::vector<scav_byte> f(bytes, 0);
    for (uint32_t i = 0; (i < head.size()) && (i < bytes); ++i) { f[i] = head[i]; }
    return f;
  };
  std::vector<scav_byte> const good{ assemble({
      { .tag = "cmap", .bytes = cmap4_table('a', 'z', 1) },
      { .tag = "head", .bytes = head_table(1000) },
      { .tag = "hhea", .bytes = hhea_table(2) },
      { .tag = "hmtx", .bytes = hmtx_table({ 0, 600 }, 2) },
      { .tag = "maxp", .bytes = maxp_table(2) },
  }) };
  REQUIRE(!refuses(good));

  // The first record's offset field sits at 20 and its length at 24.
  std::vector<scav_byte> far_off{ good };
  std::vector<scav_byte> far_len{ good };
  for (uint32_t i = 0; i < 4; ++i) {
    far_off[20U + i] = 0xFF;
    far_len[24U + i] = 0xFF;
  }

  struct Case {
    char const *what;
    std::vector<scav_byte> font;
  };
  std::vector<Case> const cases{
    { .what = "four bytes: the table count is not there", .font = header(4, 4) },
    { .what = "one record, cut before its offset", .font = header(2, 20) },
    { .what = "one record, cut before its length", .font = header(1, 24) },
    { .what = "a record whose offset is past the file", .font = far_off },
    { .what = "a record whose length runs off the file", .font = far_len },
  };
  for (Case const &c : cases) {
    CAPTURE(c.what);
    CHECK(refuses(c.font));
  }
}

TEST_CASE("metrics: a table too short for the field it must carry is refused") {
  // The short table goes last, so its field lands past the file rather than in
  // the next table's bytes: every read is bounded by the file, not the table.
  auto const with_short = [](char const *tag) {
    std::vector<Table> tables{
      { .tag = "cmap", .bytes = cmap4_table('a', 'z', 1) },
      { .tag = "hmtx", .bytes = hmtx_table({ 0, 600 }, 2) },
      { .tag = "head", .bytes = head_table(1000) },
      { .tag = "hhea", .bytes = hhea_table(2) },
      { .tag = "maxp", .bytes = maxp_table(2) },
    };
    Table short_one{ .tag = tag, .bytes = std::vector<scav_byte>(4, 0) };
    for (uint32_t i = 0; i < tables.size(); ++i) {
      if (std::string_view{ tables[i].tag } == std::string_view{ tag }) {
        tables.erase(tables.begin() + i);
        break;
      }
    }
    tables.push_back(std::move(short_one));
    return assemble(tables);
  };
  for (char const *tag : { "head", "maxp", "hhea" }) {
    CAPTURE(tag);
    CHECK(refuses(with_short(tag)));
  }
}

TEST_CASE("metrics: a cmap with no subtable this code can read is refused") {
  // A subtable record is 8 bytes at table offset 4, so a table cut at 12, 14 or
  // 16 stops the second record's platform, encoding or offset in turn.
  auto const two_records = [](uint32_t bytes) {
    std::vector<scav_byte> t;
    be16(t, 0);
    be16(t, 2);
    be16(t, 3);
    be16(t, 1);
    be32(t, 0xFFFFU);  // past the table, so this record contributes nothing
    t.resize(bytes, 0);
    return t;
  };
  std::vector<scav_byte> unreadable_offset;
  be16(unreadable_offset, 0);
  be16(unreadable_offset, 1);
  be16(unreadable_offset, 3);
  be16(unreadable_offset, 1);
  be32(unreadable_offset, 11);  // inside the table, but one byte from the file

  std::vector<Seg> const whole{ { .end = 0xFFFFU, .start = 0, .delta = 1, .range = 0 } };
  std::vector<scav_byte> const twelve{ cmap12_table(0x20U, 0x30U, 1) };
  std::vector<scav_byte> const twelve_sub{ twelve.begin() + 12, twelve.end() };

  struct Case {
    char const *what;
    std::vector<scav_byte> font;
  };
  std::vector<Case> const cases{
    { .what = "a cmap cut before its subtable count",
      .font = font_with_cmap(cmap4_table('a', 'z', 1), 2) },
    { .what = "a record cut before its platform",
      .font = font_with_cmap(two_records(12), 12) },
    { .what = "a record cut before its encoding",
      .font = font_with_cmap(two_records(14), 14) },
    { .what = "a record cut before its subtable offset",
      .font = font_with_cmap(two_records(16), 16) },
    { .what = "every subtable offset past the table",
      .font = font_with_cmap(two_records(20), 20) },
    { .what = "a subtable offset one byte from the file's end",
      .font = font_with_cmap(unreadable_offset, 12) },
    { .what = "a Macintosh subtable, which this code does not read",
      .font = font_with_cmap(cmap_wrap(1, 0, cmap4_sub(whole, {})), 0xFFFFU) },
    // Format and encoding are ranked together: a full-repertoire table under a
    // BMP-only encoding is a contradiction, and neither half alone qualifies.
    { .what = "a format 12 subtable under a BMP-only encoding",
      .font = font_with_cmap(cmap_wrap(0, 3, twelve_sub), 0xFFFFU) },
  };
  for (Case const &c : cases) {
    CAPTURE(c.what);
    CHECK(refuses(c.font));
  }
}

TEST_CASE("metrics: a format 4 table that lies about its own size maps nothing") {
  // For one segment the arrays land at 26 (end), 30 (start), 32 (delta) and 34
  // (range) within the cmap table; cutting the file there stops the read after.
  std::vector<Seg> const one{ { .end = 0xFFFFU, .start = 0, .delta = 1, .range = 0 } };
  std::vector<Seg> const two{ { .end = 0x10U, .start = 0, .delta = 1, .range = 0 },
                              { .end = 0xFFFFU, .start = 0x20U, .delta = 1, .range = 0 } };
  std::vector<scav_byte> const wide{ cmap_wrap(3, 1, cmap4_sub(two, {})) };
  std::vector<scav_byte> const narrow{ cmap_wrap(3, 1, cmap4_sub(one, {})) };

  struct Case {
    char const *what;
    std::vector<scav_byte> font;
  };
  std::vector<Case> const cases{
    { .what = "cut before the segment count", .font = font_with_cmap(narrow, 14) },
    { .what = "cut before the second end code", .font = font_with_cmap(wide, 28) },
    { .what = "cut before the start codes", .font = font_with_cmap(narrow, 30) },
    { .what = "cut before the deltas", .font = font_with_cmap(narrow, 32) },
    { .what = "cut before the range offsets", .font = font_with_cmap(narrow, 34) },
  };
  for (Case const &c : cases) {
    CAPTURE(c.what);
    Metrics const m{ from_bytes(c.font) };
    REQUIRE(m.cmap_format == 4);
    CHECK(metrics_glyph(m, 0x41U) == 0);
  }

  // A segment count of zero is a table with no segments at all, which is not
  // the same as one whose first segment starts at zero.
  std::vector<scav_byte> empty{ cmap_wrap(3, 1, cmap4_sub(one, {})) };
  empty[12U + 6U] = 0;
  empty[12U + 7U] = 0;
  Metrics const none{ from_bytes(font_with_cmap(empty, 0xFFFFU)) };
  CHECK(metrics_glyph(none, 0x41U) == 0);
}

TEST_CASE("metrics: format 4 maps through an idRangeOffset, or through nothing") {
  // Two segments and a two-glyph array: one codepoint reaches a live glyph
  // through the offset, and one reaches the array's own zero.
  std::vector<Seg> const segs{
    { .end = 0x42U, .start = 0x41U, .delta = 0, .range = 0 },
    { .end = 0xFFFFU, .start = 0xFFFFU, .delta = 1, .range = 0 },
  };
  std::vector<Seg> patched{ segs };
  patched[0].range = range_offset(2, 0, 0);
  Metrics const m{ from_bytes(
      font_with_cmap(cmap_wrap(3, 1, cmap4_sub(patched, { 5, 0 })), 0xFFFFU)) };
  REQUIRE(m.cmap_format == 4);
  CHECK(metrics_glyph(m, 0x41U) == 5);  // out of the array, then the delta
  CHECK(metrics_glyph(m, 0x42U) == 0);  // the array's own zero, not a glyph

  // An offset reaching past the end of the file reads back nothing rather than
  // whatever happens to follow the table.
  std::vector<Seg> far{ segs };
  far[0].range = 0xFFF0U;
  Metrics const past{ from_bytes(
      font_with_cmap(cmap_wrap(3, 1, cmap4_sub(far, { 5, 0 })), 0xFFFFU)) };
  CHECK(metrics_glyph(past, 0x41U) == 0);

  // Without the mandatory 0xFFFF terminator, a codepoint above the last segment
  // runs out of segments, which is a miss rather than a read past the array.
  std::vector<Seg> const unterminated{
    { .end = 0x42U, .start = 0x41U, .delta = 0xFFC0U, .range = 0 }
  };
  Metrics const open{ from_bytes(
      font_with_cmap(cmap_wrap(3, 1, cmap4_sub(unterminated, {})), 0xFFFFU)) };
  CHECK(metrics_glyph(open, 0x41U) == 1);
  CHECK(metrics_glyph(open, 0x43U) == 0);

  // A glyph id past the font's own count is nothing, rather than a row of the
  // hmtx that belongs to no glyph.
  std::vector<Seg> const beyond{
    { .end = 0x42U, .start = 0x41U, .delta = 0, .range = 0 }
  };
  Metrics const over{ from_bytes(
      font_with_cmap(cmap_wrap(3, 1, cmap4_sub(beyond, {})), 0xFFFFU)) };
  REQUIRE(over.num_glyphs == 8);
  CHECK(metrics_glyph(over, 0x41U) == 0);  // 0x41 itself, which is past eight
}

TEST_CASE("metrics: the bundled font's own format 4 subtable agrees with format 12") {
  // The picker passes over format 4 wherever a font offers format 12, so the
  // bundled font's format 4 table is reached by pointing a Metrics at it.
  Metrics const twelve{ bundled() };
  REQUIRE(twelve.cmap_format == 12);

  uint32_t cmap_off{ 0 };
  uint32_t const tables{ rd16(twelve.ttf, 4) };
  for (uint32_t i = 0; i < tables; ++i) {
    uint32_t const at{ 12U + (16U * i) };
    if (rd32(twelve.ttf, at) == 0x636D'6170U) { cmap_off = rd32(twelve.ttf, at + 8U); }
  }
  REQUIRE(cmap_off != 0);

  uint32_t sub{ 0 };
  uint32_t const subtables{ rd16(twelve.ttf, cmap_off + 2U) };
  for (uint32_t i = 0; i < subtables; ++i) {
    uint32_t const at{ cmap_off + 4U + (8U * i) };
    uint32_t const candidate{ cmap_off + rd32(twelve.ttf, at + 4U) };
    if (rd16(twelve.ttf, candidate) == 4U) { sub = candidate; }
  }
  REQUIRE(sub != 0);

  Metrics four{ twelve };
  four.cmap_sub = { .off = sub, .len = 0 };
  four.cmap_format = 4;

  // U+0021 sits in a segment whose idRangeOffset is non-zero, so its glyph
  // comes out of the array rather than out of the delta.
  uint32_t const seg_x2{ rd16(twelve.ttf, sub + 6U) };
  uint32_t const end_base{ sub + 14U };
  uint32_t const start_base{ end_base + seg_x2 + 2U };
  uint32_t const delta_base{ start_base + seg_x2 };
  uint32_t const range_base{ delta_base + seg_x2 };
  constexpr uint32_t CP{ 0x21U };
  uint32_t walked{ 0 };
  for (uint32_t i = 0; i < (seg_x2 / 2U); ++i) {
    if (rd16(twelve.ttf, end_base + (2U * i)) < CP) { continue; }
    uint32_t const start{ rd16(twelve.ttf, start_base + (2U * i)) };
    uint32_t const range{ rd16(twelve.ttf, range_base + (2U * i)) };
    REQUIRE(start <= CP);
    REQUIRE(range != 0);  // the path under test, not the delta one
    uint32_t const delta{ rd16(twelve.ttf, delta_base + (2U * i)) };
    uint32_t const at{ range_base + (2U * i) + range + (2U * (CP - start)) };
    walked = (rd16(twelve.ttf, at) + delta) & 0xFFFFU;
    break;
  }
  REQUIRE(walked != 0);

  uint32_t const glyph{ metrics_glyph(four, CP) };
  CHECK(glyph == walked);
  CHECK(glyph == metrics_glyph(twelve, CP));  // one font, read two ways
  CHECK(metrics_advance(four, glyph) != 0);
  CHECK(metrics_advance(four, glyph) == metrics_advance(twelve, glyph));
}

TEST_CASE("metrics: a format 12 table that lies about its own size maps nothing") {
  // A group is 12 bytes at subtable offset 16, and nGroups sits at 12; the cut
  // is measured from the start of the cmap table, whose subtable begins at 12.
  auto const groups = []() {
    std::vector<scav_byte> sub;
    be16(sub, 12);
    be16(sub, 0);
    be32(sub, 16 + (12 * 2));
    be32(sub, 0);
    be32(sub, 2);
    be32(sub, 0x20U);
    be32(sub, 0x30U);
    be32(sub, 1);
    be32(sub, 0x50U);
    be32(sub, 0x60U);
    be32(sub, 2);
    return cmap_wrap(3, 10, sub);
  };
  struct Case {
    char const *what;
    uint32_t cut;
  };
  std::vector<Case> const cases{
    { .what = "cut before the group count", .cut = 12 + 14 },
    { .what = "cut before the second group's first codepoint", .cut = 12 + 28 },
    { .what = "cut before its last codepoint", .cut = 12 + 32 },
    { .what = "cut before its start glyph", .cut = 12 + 36 },
  };
  for (Case const &c : cases) {
    CAPTURE(c.what);
    Metrics const m{ from_bytes(font_with_cmap(groups(), c.cut)) };
    REQUIRE(m.cmap_format == 12);
    CHECK(metrics_glyph(m, 0x40U) == 0);  // past group 0, into the cut
  }
}

TEST_CASE("metrics: a codepoint in a gap between groups is missing, not mapped") {
  // The groups ascend, so the first one starting above the codepoint ends the
  // search: nothing later can match, and reading on would be wasted work.
  Metrics const m{ bundled() };
  CHECK(metrics_glyph(m, 0x0EU) == 0);  // between U+000D and U+0020
  CHECK(metrics_glyph(m, 0x01U) == 0);  // below the first group of all
  scav_extent e{};
  CHECK(measure(m, "\x0E", 16, e) == MeasureStatus::MissingGlyph);
}

TEST_CASE("metrics: an hmtx that points past the font reads back no advance") {
  // Reachable only by hand: metrics_create refuses a font whose hmtx cannot
  // hold the records hhea claims, which is what makes the read safe.
  Metrics m{ bundled() };
  m.hmtx.off = static_cast<uint32_t>(m.ttf.size());
  CHECK(metrics_advance(m, 0) == 0);
}
