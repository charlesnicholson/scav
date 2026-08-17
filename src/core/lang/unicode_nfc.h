#ifndef SCAV_CORE_LANG_UNICODE_NFC_H_INCLUDED
#define SCAV_CORE_LANG_UNICODE_NFC_H_INCLUDED

// NFC over codepoints, against committed tables from gen_unicode_tables.py.
// Hangul is algorithmic in both directions and has no table entry.

#include <cstdint>
#include <vector>

namespace scav {

// True when `cp` has a non-zero combining class or NFC_QC says it may move.
// Text of codepoints this rejects is already NFC.
bool unicode_nfc_needs_work(uint32_t cp);

uint32_t unicode_nfc_combining_class(uint32_t cp);

// `out` is cleared first. Returns true when the result differs from `in`.
bool unicode_nfc_normalize(std::vector<uint32_t> const &in, std::vector<uint32_t> &out);

}  // namespace scav

#endif  // SCAV_CORE_LANG_UNICODE_NFC_H_INCLUDED
