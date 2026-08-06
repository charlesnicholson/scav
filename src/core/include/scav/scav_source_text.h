#ifndef SCAV_SOURCE_TEXT_H_INCLUDED
#define SCAV_SOURCE_TEXT_H_INCLUDED

// Everything here is named `source_text_*`, after the header.
//
// Text is normalized on read (PRD 6): BOM stripped, line endings folded to LF,
// UTF-8 validated, NFC applied. Without this, core.autocrlf on Windows and NFD
// on macOS put different bytes in the string pool from the same commit, and the
// format hash follows the pool.
//
// Every offset downstream -- Statement.src, every diagnostic span -- indexes the
// *normalized* bytes, which is what makes a reported line and column the same
// number on every platform. The exception is the diagnostics this file itself
// produces: there is no normalized buffer yet when a UTF-8 error fires, so those
// spans index the raw input.

#include "scav/scav_diagnostics.h"
#include "scav/scav_ids.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

// Returns false and leaves `out` empty when the input is not valid UTF-8.
// `out` is cleared first either way.
bool source_text_normalize(scav_byte const *bytes,
                           uint32_t len,
                           DocId doc,
                           std::vector<scav_byte> &out,
                           std::vector<Diagnostic> &diags);

// The pieces, exposed because they are separately testable and because the
// string-literal decoder needs the last two on decoded escapes rather than on a
// whole document.
bool source_text_utf8_decode(scav_byte const *bytes,
                             uint32_t len,
                             uint32_t at,
                             uint32_t &cp,
                             uint32_t &width,
                             DiagCode &err);
void source_text_utf8_encode(uint32_t cp, std::vector<scav_byte> &out);

// True when the bytes contain no codepoint that NFC could move. Byte-scans the
// ASCII run, which is the whole of a typical chart.
bool source_text_is_nfc(scav_byte const *bytes, uint32_t len);

// Assumes valid UTF-8; `out` is cleared first. Returns true when it changed
// something.
bool source_text_to_nfc(scav_byte const *bytes, uint32_t len, std::vector<scav_byte> &out);

// LF-only and no BOM, without the UTF-8 or NFC passes. Exposed for the raw
// string decoder, which works on bytes that are already normalized.
bool source_text_is_ascii(scav_byte const *bytes, uint32_t len);

std::string_view source_text_view(std::vector<scav_byte> const &bytes, Span span);

}  // namespace scav

#endif  // SCAV_SOURCE_TEXT_H_INCLUDED
