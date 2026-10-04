#ifndef SCAV_CORE_LANG_UNICODE_NFC_H_INCLUDED
#define SCAV_CORE_LANG_UNICODE_NFC_H_INCLUDED

// NFC over codepoints, against committed tables from gen_unicode_tables.py.
// Hangul is algorithmic in both directions and has no table entry.

#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

// The committed packed tables, decoded. Every array ascends by key.
struct NfcTables {
  std::vector<uint32_t> unsafe_lo;  // NFC_QC != Yes or a non-zero class, as ranges
  std::vector<uint32_t> unsafe_hi;
  std::vector<uint32_t> ccc_keys;  // non-zero canonical combining classes
  std::vector<scav_byte> ccc_values;
  std::vector<uint32_t> decomp_keys;  // full canonical decompositions, flattened
  std::vector<uint32_t> decomp_offsets;
  std::vector<scav_byte> decomp_lengths;
  std::vector<uint32_t> decomp_data;
  std::vector<uint64_t> compose_keys;  // (starter << 32) | combining
  std::vector<uint32_t> compose_values;
};

// Decoded on the first call, under the C++ guarantee for a function-local static.
NfcTables const &unicode_nfc_tables();

// True when `cp` has a non-zero combining class or NFC_QC is not Yes. Text where
// this is false for every codepoint is already NFC.
bool unicode_nfc_needs_work(uint32_t cp);

uint32_t unicode_nfc_combining_class(uint32_t cp);

// `out` is cleared first. Returns true when the result differs from `in`.
bool unicode_nfc_normalize(std::vector<uint32_t> const &in, std::vector<uint32_t> &out);

}  // namespace scav

#endif  // SCAV_CORE_LANG_UNICODE_NFC_H_INCLUDED
