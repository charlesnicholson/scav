// Advance widths from `hmtx`, codepoints to glyphs through `cmap`, and the one
// scaling formula. Advances never come from `glyf` or `CFF`, and no float is
// involved anywhere.

#include "scav/scav_draw.h"

#include "scav_embed_bundled_ttf.h"

#include "scav/scav_types.h"
#include "scav_int.h"
#include "scav_xxhash.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

// Bounds-checked big-endian reads. A font is untrusted input, so every read
// returns false past the end rather than trusting a length in the file.
struct Reader {
  scav_byte const *bytes;
  uint32_t len;
};

bool read_u16(Reader const &r, uint32_t at, uint32_t &out) {
  if ((at + 2U) > r.len) { return false; }
  out = (static_cast<uint32_t>(r.bytes[at]) << 8U) | r.bytes[at + 1U];
  return true;
}

bool read_i16(Reader const &r, uint32_t at, int32_t &out) {
  uint32_t raw{ 0 };
  if (!read_u16(r, at, raw)) { return false; }
  out = static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(raw)));
  return true;
}

bool read_u32(Reader const &r, uint32_t at, uint32_t &out) {
  if ((at + 4U) > r.len) { return false; }
  out = (static_cast<uint32_t>(r.bytes[at]) << 24U) |
        (static_cast<uint32_t>(r.bytes[at + 1U]) << 16U) |
        (static_cast<uint32_t>(r.bytes[at + 2U]) << 8U) | r.bytes[at + 3U];
  return true;
}

constexpr uint32_t tag(char a, char b, char c, char d) {
  return (static_cast<uint32_t>(static_cast<scav_byte>(a)) << 24U) |
         (static_cast<uint32_t>(static_cast<scav_byte>(b)) << 16U) |
         (static_cast<uint32_t>(static_cast<scav_byte>(c)) << 8U) |
         static_cast<scav_byte>(d);
}

// The table directory: 12-byte header then 16 bytes per record. A record whose
// extent runs off the end is rejected rather than clamped.
bool find_table(Reader const &r, uint32_t want, Span &out) {
  uint32_t count{ 0 };
  if (!read_u16(r, 4, count)) { return false; }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t const at{ 12U + (16U * i) };
    uint32_t got{ 0 };
    uint32_t off{ 0 };
    uint32_t length{ 0 };
    if (!read_u32(r, at, got) || !read_u32(r, at + 8U, off) ||
        !read_u32(r, at + 12U, length)) {
      return false;
    }
    if (got != want) { continue; }
    if ((off > r.len) || (length > (r.len - off))) { return false; }
    out = { .off = off, .len = length };
    return true;
  }
  return false;
}

// Format 12 beats format 4: it reaches past the BMP, and a chart may name a
// state in any plane. Within a format, the Unicode platform beats Windows only
// because one of them has to win and the subtables agree.
struct CmapPick {
  uint32_t off{ 0 };
  uint32_t format{ 0 };
  uint32_t rank{ 0 };
};

uint32_t cmap_rank(uint32_t platform, uint32_t encoding, uint32_t format) {
  bool const unicode_full{ (platform == 0U) && (encoding == 4U) };
  bool const windows_full{ (platform == 3U) && (encoding == 10U) };
  bool const unicode_bmp{ (platform == 0U) && (encoding == 3U) };
  bool const windows_bmp{ (platform == 3U) && (encoding == 1U) };
  if (format == 12U) {
    if (unicode_full) { return 4U; }
    if (windows_full) { return 3U; }
  }
  if (format == 4U) {
    if (unicode_bmp) { return 2U; }
    if (windows_bmp) { return 1U; }
  }
  return 0U;
}

bool pick_cmap(Reader const &r, Span cmap, CmapPick &out) {
  uint32_t count{ 0 };
  if (!read_u16(r, cmap.off + 2U, count)) { return false; }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t const at{ cmap.off + 4U + (8U * i) };
    uint32_t platform{ 0 };
    uint32_t encoding{ 0 };
    uint32_t rel{ 0 };
    if (!read_u16(r, at, platform) || !read_u16(r, at + 2U, encoding) ||
        !read_u32(r, at + 4U, rel)) {
      return false;
    }
    if (rel >= cmap.len) { continue; }
    uint32_t const sub{ cmap.off + rel };
    uint32_t format{ 0 };
    if (!read_u16(r, sub, format)) { continue; }
    uint32_t const rank{ cmap_rank(platform, encoding, format) };
    if (rank > out.rank) { out = { .off = sub, .format = format, .rank = rank }; }
  }
  return out.rank != 0U;
}

// Segment search over format 4. Linear rather than the binary search the
// header's searchRange invites: those fields are routinely wrong in the wild,
// and a chart's label is a few dozen codepoints.
uint32_t lookup_format4(Reader const &r, uint32_t sub, uint32_t cp) {
  if (cp > 0xFFFFU) { return 0U; }
  uint32_t seg_x2{ 0 };
  if (!read_u16(r, sub + 6U, seg_x2) || (seg_x2 < 2U)) { return 0U; }
  uint32_t const segs{ seg_x2 / 2U };
  uint32_t const end_base{ sub + 14U };
  uint32_t const start_base{ end_base + seg_x2 + 2U };
  uint32_t const delta_base{ start_base + seg_x2 };
  uint32_t const range_base{ delta_base + seg_x2 };

  for (uint32_t i = 0; i < segs; ++i) {
    uint32_t end{ 0 };
    if (!read_u16(r, end_base + (2U * i), end)) { return 0U; }
    if (end < cp) { continue; }
    uint32_t start{ 0 };
    if (!read_u16(r, start_base + (2U * i), start) || (start > cp)) { return 0U; }
    int32_t delta{ 0 };
    uint32_t range{ 0 };
    if (!read_i16(r, delta_base + (2U * i), delta) ||
        !read_u16(r, range_base + (2U * i), range)) {
      return 0U;
    }
    if (range == 0U) { return (cp + static_cast<uint32_t>(delta)) & 0xFFFFU; }
    // idRangeOffset is a byte offset from its own slot, which is the one place
    // in the format that is relative to where it was read from.
    uint32_t const at{ range_base + (2U * i) + range + (2U * (cp - start)) };
    uint32_t glyph{ 0 };
    if (!read_u16(r, at, glyph) || (glyph == 0U)) { return 0U; }
    return (glyph + static_cast<uint32_t>(delta)) & 0xFFFFU;
  }
  return 0U;
}

uint32_t lookup_format12(Reader const &r, uint32_t sub, uint32_t cp) {
  uint32_t groups{ 0 };
  if (!read_u32(r, sub + 12U, groups)) { return 0U; }
  for (uint32_t i = 0; i < groups; ++i) {
    uint32_t const at{ sub + 16U + (12U * i) };
    uint32_t first{ 0 };
    uint32_t last{ 0 };
    uint32_t glyph{ 0 };
    if (!read_u32(r, at, first) || !read_u32(r, at + 4U, last) ||
        !read_u32(r, at + 8U, glyph)) {
      return 0U;
    }
    if (cp < first) { return 0U; }  // groups ascend, so no later one can match
    if (cp <= last) { return glyph + (cp - first); }
  }
  return 0U;
}

// One codepoint out of NFC UTF-8, or false. Rejects overlongs, surrogates and
// anything past U+10FFFF: the pool is normalized, so a violation is a bug
// upstream and silence would measure the wrong string.
bool decode_utf8(scav_byte const *s, uint32_t len, uint32_t &at, uint32_t &cp) {
  if (at >= len) { return false; }
  uint32_t const lead{ s[at] };
  uint32_t need{ 0 };
  uint32_t value{ 0 };
  if (lead < 0x80U) {
    ++at;
    cp = lead;
    return true;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    need = 1U;
    value = lead & 0x1FU;
  } else if ((lead & 0xF0U) == 0xE0U) {
    need = 2U;
    value = lead & 0x0FU;
  } else if ((lead & 0xF8U) == 0xF0U) {
    need = 3U;
    value = lead & 0x07U;
  } else {
    return false;
  }
  if ((at + need) >= len) { return false; }  // the continuations must all fit
  for (uint32_t i = 1; i <= need; ++i) {
    uint32_t const cont{ s[at + i] };
    if ((cont & 0xC0U) != 0x80U) { return false; }
    value = (value << 6U) | (cont & 0x3FU);
  }
  constexpr std::array<uint32_t, 4> MIN_FOR{ 0U, 0x80U, 0x800U, 0x10000U };
  if ((value < MIN_FOR[need]) || (value > 0x10FFFFU) ||
      ((value >= 0xD800U) && (value <= 0xDFFFU))) {
    return false;
  }
  at += need + 1U;
  cp = value;
  return true;
}

}  // namespace

scav_byte const *bundled_font(uint32_t &len) { return bundled_ttf_bytes(len); }

bool metrics_create(scav_byte const *ttf, uint32_t len, Metrics &out) {
  if ((ttf == nullptr) || (len == 0U)) { ttf = bundled_font(len); }
  Reader const r{ .bytes = ttf, .len = len };

  Span head{};
  Span hhea{};
  Span maxp{};
  Span hmtx{};
  Span cmap{};
  if (!find_table(r, tag('h', 'e', 'a', 'd'), head) ||
      !find_table(r, tag('h', 'h', 'e', 'a'), hhea) ||
      !find_table(r, tag('m', 'a', 'x', 'p'), maxp) ||
      !find_table(r, tag('h', 'm', 't', 'x'), hmtx) ||
      !find_table(r, tag('c', 'm', 'a', 'p'), cmap)) {
    return false;
  }

  uint32_t upem{ 0 };
  uint32_t glyphs{ 0 };
  uint32_t h_metrics{ 0 };
  if (!read_u16(r, head.off + 18U, upem) || !read_u16(r, maxp.off + 4U, glyphs) ||
      !read_u16(r, hhea.off + 34U, h_metrics)) {
    return false;
  }
  // Tables that disagree with each other: an hmtx shorter than it claims, or a
  // record count above the glyph count, would index past the table later.
  if ((upem == 0U) || (glyphs == 0U) || (h_metrics == 0U) || (h_metrics > glyphs) ||
      ((4ULL * h_metrics) > hmtx.len)) {
    return false;
  }

  CmapPick pick{};
  if (!pick_cmap(r, cmap, pick)) { return false; }

  out.ttf.assign(ttf, ttf + len);
  out.identity = xxhash32(out.ttf.data(), out.ttf.size(), 0);
  out.units_per_em = upem;
  out.num_glyphs = glyphs;
  out.num_h_metrics = h_metrics;
  out.hmtx = hmtx;
  out.cmap_sub = { .off = pick.off, .len = 0 };
  out.cmap_format = pick.format;
  return true;
}

uint32_t metrics_glyph(Metrics const &m, uint32_t codepoint) {
  Reader const r{ .bytes = m.ttf.data(), .len = static_cast<uint32_t>(m.ttf.size()) };
  uint32_t const glyph{ (m.cmap_format == 12U)
                            ? lookup_format12(r, m.cmap_sub.off, codepoint)
                            : lookup_format4(r, m.cmap_sub.off, codepoint) };
  return (glyph < m.num_glyphs) ? glyph : 0U;
}

uint32_t metrics_advance(Metrics const &m, uint32_t glyph) {
  // The tail rule: past the last record, that record's advance applies to every
  // remaining glyph. Missing it breaks monospaced fonts specifically.
  uint32_t const row{ imin(glyph, m.num_h_metrics - 1U) };
  Reader const r{ .bytes = m.ttf.data(), .len = static_cast<uint32_t>(m.ttf.size()) };
  uint32_t advance{ 0 };
  if (!read_u16(r, m.hmtx.off + (4U * row), advance)) { return 0U; }
  return advance;
}

MeasureStatus measure_text(Metrics const &m,
                           scav_byte const *utf8_nfc,
                           uint32_t len,
                           int32_t font_size_grid,
                           scav_extent &out) {
  out = {};
  if ((font_size_grid <= 0) || (font_size_grid > (COORD_MAX / 4))) {
    return MeasureStatus::BadSize;
  }
  if ((utf8_nfc == nullptr) && (len != 0U)) { return MeasureStatus::BadUtf8; }

  Wide funits{ 0 };
  uint32_t at{ 0 };
  while (at < len) {
    uint32_t cp{ 0 };
    if (!decode_utf8(utf8_nfc, len, at, cp)) { return MeasureStatus::BadUtf8; }
    if (cp == 0x0AU) { return MeasureStatus::Newline; }
    uint32_t const glyph{ metrics_glyph(m, cp) };
    // Loud, because a silent zero produces a box narrower than its own text.
    if (glyph == 0U) { return MeasureStatus::MissingGlyph; }
    funits += metrics_advance(m, glyph);
  }

  // Accumulate wide, divide exactly once, ceil never round-to-nearest: an
  // under-sized box is a diagram that lies.
  Wide const w{ ceil_div(funits * font_size_grid, static_cast<Wide>(m.units_per_em)) };
  if (w > COORD_MAX) { return MeasureStatus::BadSize; }
  out = { .w = static_cast<int32_t>(w), .h = font_size_grid };
  return MeasureStatus::Ok;
}

int32_t line_height(int32_t font_size_grid, int32_t k_num, int32_t k_den) {
  if ((font_size_grid <= 0) || (k_num < 1) || (k_num > 1024) || (k_den < 1) ||
      (k_den > 1024)) {
    return 0;
  }
  Wide const h{ ceil_div(static_cast<Wide>(font_size_grid) * k_num,
                         static_cast<Wide>(k_den)) };
  return (h > COORD_MAX) ? 0 : static_cast<int32_t>(h);
}

MeasureStatus measure_block(Metrics const &m,
                            scav_byte const *utf8_nfc,
                            uint32_t len,
                            int32_t font_size_grid,
                            int32_t k_num,
                            int32_t k_den,
                            scav_extent &out) {
  out = {};
  int32_t const lh{ line_height(font_size_grid, k_num, k_den) };
  if (lh == 0) { return MeasureStatus::BadSize; }

  int32_t widest{ 0 };
  uint32_t lines{ 0 };
  uint32_t start{ 0 };
  // One trailing pass past the last byte, so a string with no newline is one
  // line and a trailing newline is not an extra empty one.
  for (uint32_t i = 0; i <= len; ++i) {
    if ((i != len) && (utf8_nfc[i] != 0x0AU)) { continue; }
    if ((i == len) && (i != 0U) && (utf8_nfc[i - 1U] == 0x0AU)) { break; }
    scav_extent line{};
    MeasureStatus const st{
      measure_text(m, utf8_nfc + start, i - start, font_size_grid, line)
    };
    if (st != MeasureStatus::Ok) { return st; }
    widest = imax(widest, line.w);
    ++lines;
    start = i + 1U;
  }
  if (lines == 0U) { lines = 1U; }

  Wide const h{ static_cast<Wide>(lh) * lines };
  if (h > COORD_MAX) { return MeasureStatus::BadSize; }
  out = { .w = widest, .h = static_cast<int32_t>(h) };
  return MeasureStatus::Ok;
}

}  // namespace scav
