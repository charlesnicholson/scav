#include "scav/scav_core.h"

#include "core/lang/unicode_nfc.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

namespace {

constexpr uint32_t SURROGATE_LO{ 0xD800 };
constexpr uint32_t SURROGATE_HI{ 0xDFFF };
constexpr uint32_t CODEPOINT_MAX{ 0x10FFFF };

bool is_continuation(scav_byte b) { return (b & 0xC0U) == 0x80U; }

// Fills `cp` from the `n` continuation bytes after `at`. Truncated and
// InvalidByte are distinguished because they are different mistakes.
bool take_continuations(scav_byte const *bytes,
                        size_t len,
                        size_t at,
                        uint32_t n,
                        uint32_t &cp,
                        DiagCode &err) {
  for (uint32_t i = 1; i <= n; ++i) {
    if (at + i >= len) {
      err = DiagCode::Utf8Truncated;
      return false;
    }
    if (!is_continuation(bytes[at + i])) {
      err = DiagCode::Utf8InvalidByte;
      return false;
    }
    cp = (cp << 6U) | (bytes[at + i] & 0x3FU);
  }
  return true;
}

}  // namespace

bool source_text_utf8_decode(scav_byte const *bytes,
                             size_t len,
                             size_t at,
                             uint32_t &cp,
                             uint32_t &width,
                             DiagCode &err) {
  err = DiagCode::Ok;
  // One byte on failure, so a caller that wants to keep scanning makes progress.
  width = 1;
  if (at >= len) {
    err = DiagCode::Utf8Truncated;
    return false;
  }

  scav_byte const b0{ bytes[at] };
  if (b0 < 0x80U) {
    cp = b0;
    return true;
  }
  if (b0 < 0xC0U) {  // a continuation byte with nothing to continue
    err = DiagCode::Utf8InvalidByte;
    return false;
  }

  uint32_t trail{ 0 };
  uint32_t lowest{ 0 };
  if (b0 < 0xE0U) {
    cp = b0 & 0x1FU;
    trail = 1;
    lowest = 0x80;
  } else if (b0 < 0xF0U) {
    cp = b0 & 0x0FU;
    trail = 2;
    lowest = 0x800;
  } else if (b0 < 0xF8U) {
    cp = b0 & 0x07U;
    trail = 3;
    lowest = 0x10000;
  } else {  // 0xF8..0xFF never appears in UTF-8
    err = DiagCode::Utf8InvalidByte;
    return false;
  }

  if (!take_continuations(bytes, len, at, trail, cp, err)) { return false; }

  // Checked after decoding rather than by a lead-byte range table: one rule per
  // failure mode reads better, and the diagnostic says which one broke.
  if (cp < lowest) {
    err = DiagCode::Utf8Overlong;
    return false;
  }
  if ((cp >= SURROGATE_LO) && (cp <= SURROGATE_HI)) {
    err = DiagCode::Utf8Surrogate;
    return false;
  }
  if (cp > CODEPOINT_MAX) {
    err = DiagCode::Utf8OutOfRange;
    return false;
  }

  width = trail + 1;
  return true;
}

void source_text_utf8_encode(uint32_t cp, std::vector<scav_byte> &out) {
  if (cp < 0x80U) {
    out.push_back(static_cast<scav_byte>(cp));
  } else if (cp < 0x800U) {
    out.push_back(static_cast<scav_byte>(0xC0U | (cp >> 6U)));
    out.push_back(static_cast<scav_byte>(0x80U | (cp & 0x3FU)));
  } else if (cp < 0x10000U) {
    out.push_back(static_cast<scav_byte>(0xE0U | (cp >> 12U)));
    out.push_back(static_cast<scav_byte>(0x80U | ((cp >> 6U) & 0x3FU)));
    out.push_back(static_cast<scav_byte>(0x80U | (cp & 0x3FU)));
  } else {
    out.push_back(static_cast<scav_byte>(0xF0U | (cp >> 18U)));
    out.push_back(static_cast<scav_byte>(0x80U | ((cp >> 12U) & 0x3FU)));
    out.push_back(static_cast<scav_byte>(0x80U | ((cp >> 6U) & 0x3FU)));
    out.push_back(static_cast<scav_byte>(0x80U | (cp & 0x3FU)));
  }
}

bool source_text_is_ascii(scav_byte const *bytes, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (bytes[i] >= 0x80U) { return false; }
  }
  return true;
}

bool source_text_is_nfc(scav_byte const *bytes, size_t len) {
  size_t at{ 0 };
  while (at < len) {
    if (bytes[at] < 0x80U) {  // ASCII is NFC by construction, and is the hot path
      ++at;
      continue;
    }
    uint32_t cp{ 0 };
    uint32_t width{ 0 };
    DiagCode err{ DiagCode::Ok };
    if (!source_text_utf8_decode(bytes, len, at, cp, width, err)) { return false; }
    if (nfc_needs_work(cp)) { return false; }
    at += width;
  }
  return true;
}

bool source_text_to_nfc(scav_byte const *bytes, size_t len, std::vector<scav_byte> &out) {
  out.clear();

  std::vector<uint32_t> codepoints;
  codepoints.reserve(len);
  size_t at{ 0 };
  while (at < len) {
    uint32_t cp{ 0 };
    uint32_t width{ 0 };
    DiagCode err{ DiagCode::Ok };
    if (!source_text_utf8_decode(bytes, len, at, cp, width, err)) {
      // The caller validated already, so this cannot fire on a real document.
      // Copying the byte through keeps a fuzz case from losing data silently.
      out.push_back(bytes[at]);
      ++at;
      continue;
    }
    codepoints.push_back(cp);
    at += width;
  }

  std::vector<uint32_t> normalized;
  bool const changed{ nfc_normalize(codepoints, normalized) };
  if (!changed) {
    out.assign(bytes, bytes + len);
    return false;
  }

  out.reserve(len);
  for (uint32_t const cp : normalized) { source_text_utf8_encode(cp, out); }
  return true;
}

bool source_text_normalize(scav_byte const *bytes,
                           size_t len,
                           DocId doc,
                           std::vector<scav_byte> &out,
                           std::vector<Diagnostic> &diags) {
  out.clear();

  // size_t stops here: every offset downstream lands in a {uint32 off, len}
  // span, so an unaddressable document is rejected rather than truncated.
  uint32_t checked_len{ 0 };
  if (!narrow(len, checked_len)) {
    diags.push_back({ .code = DiagCode::DocumentTooLarge, .doc = doc, .src = {} });
    return false;
  }

  // A UTF-8 BOM is a byte-order mark for an encoding that has no byte order, so
  // it is signature-only and is dropped rather than kept as U+FEFF.
  uint32_t at{ 0 };
  if ((checked_len >= 3) && (bytes[0] == 0xEFU) && (bytes[1] == 0xBBU) &&
      (bytes[2] == 0xBFU)) {
    at = 3;
  }

  // Validate and fold line endings in one pass. Spans on anything reported here
  // index the raw input, because `out` is what is being built.
  out.reserve(checked_len - at);
  while (at < checked_len) {
    scav_byte const b{ bytes[at] };
    if (b == '\r') {
      out.push_back('\n');
      at += ((at + 1 < checked_len) && (bytes[at + 1] == '\n')) ? 2 : 1;
      continue;
    }
    if (b < 0x80U) {
      out.push_back(b);
      ++at;
      continue;
    }

    uint32_t cp{ 0 };
    uint32_t width{ 0 };
    DiagCode err{ DiagCode::Ok };
    if (!source_text_utf8_decode(bytes, checked_len, at, cp, width, err)) {
      diags.push_back({ .code = err, .doc = doc, .src = make_span(at, 1) });
      out.clear();
      return false;
    }
    out.insert(out.end(), bytes + at, bytes + at + width);
    at += width;
  }

  if (!source_text_is_nfc(out.data(), out.size())) {
    std::vector<scav_byte> composed;
    source_text_to_nfc(out.data(), out.size(), composed);
    out.swap(composed);
  }
  return true;
}

std::string_view source_text_view(std::vector<scav_byte> const &bytes, Span span) {
  if (span.len == 0) { return {}; }
  return { reinterpret_cast<char const *>(bytes.data() + span.off), span.len };
}

}  // namespace scav
