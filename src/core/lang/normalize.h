#ifndef SCAV_CORE_LANG_NORMALIZE_H_INCLUDED
#define SCAV_CORE_LANG_NORMALIZE_H_INCLUDED

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

#include "core/lang/diagnostic.h"
#include "core/model/ids.h"
#include "scav/types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

// Returns false and leaves `out` empty when the input is not valid UTF-8.
// `out` is cleared first either way.
bool normalize_source(scav_byte const *bytes,
                      uint32_t len,
                      DocId doc,
                      std::vector<scav_byte> &out,
                      std::vector<Diagnostic> &diags);

// The pieces, exposed because they are separately testable and because the
// string-literal decoder needs the last two on decoded escapes rather than on a
// whole document.
bool utf8_decode(scav_byte const *bytes,
                 uint32_t len,
                 uint32_t at,
                 uint32_t &cp,
                 uint32_t &width,
                 DiagCode &err);
void utf8_encode(uint32_t cp, std::vector<scav_byte> &out);

// True when the bytes contain no codepoint that NFC could move. Byte-scans the
// ASCII run, which is the whole of a typical chart.
bool is_nfc(scav_byte const *bytes, uint32_t len);

// Assumes valid UTF-8; `out` is cleared first. Returns true when it changed
// something.
bool nfc_bytes(scav_byte const *bytes, uint32_t len, std::vector<scav_byte> &out);

// LF-only and no BOM, without the UTF-8 or NFC passes. Exposed for the raw
// string decoder, which works on bytes that are already normalized.
bool is_ascii(scav_byte const *bytes, uint32_t len);

std::string_view text_view(std::vector<scav_byte> const &bytes, Span span);

}  // namespace scav

#endif  // SCAV_CORE_LANG_NORMALIZE_H_INCLUDED
