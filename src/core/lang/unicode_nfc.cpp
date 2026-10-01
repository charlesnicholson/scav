#include "core/lang/unicode_nfc.h"

#include "core/core_internal.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_int.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

#include "core/lang/unicode_nfc_tables.inc"  // NOLINT(bugprone-suspicious-include)

// Hangul, Unicode 3.12. Table-free, which is why the generator drops the
// 11,172 syllables it would otherwise emit.
constexpr uint32_t HANGUL_S_BASE{ 0xAC00 };
constexpr uint32_t HANGUL_L_BASE{ 0x1100 };
constexpr uint32_t HANGUL_V_BASE{ 0x1161 };
constexpr uint32_t HANGUL_T_BASE{ 0x11A7 };
constexpr uint32_t HANGUL_L_COUNT{ 19 };
constexpr uint32_t HANGUL_V_COUNT{ 21 };
constexpr uint32_t HANGUL_T_COUNT{ 28 };
constexpr uint32_t HANGUL_N_COUNT{ HANGUL_V_COUNT * HANGUL_T_COUNT };
constexpr uint32_t HANGUL_S_COUNT{ HANGUL_L_COUNT * HANGUL_N_COUNT };

bool is_hangul_syllable(uint32_t cp) {
  return (cp >= HANGUL_S_BASE) && (cp < HANGUL_S_BASE + HANGUL_S_COUNT);
}

// Every table is sorted by key, so a lookup is a binary search.
template <typename T>
uint32_t lower_bound_key(std::vector<T> const &keys, T key) {
  uint32_t lo{ 0 };
  uint32_t hi{ narrow_clamp<uint32_t>(keys.size()) };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (keys[mid] < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// A cursor over NFC_PACKED; past the end every read is zero.
struct Packed {
  uint32_t at{ 0 };

  uint32_t byte() {
    if (at >= NFC_PACKED.size()) { return 0; }
    return NFC_PACKED[at++];
  }

  uint32_t uvarint() {
    uint32_t value{ 0 };
    for (uint32_t shift = 0; shift < 32U; shift += 7U) {
      uint32_t const b{ byte() };
      value |= (b & 0x7FU) << shift;
      if ((b & 0x80U) == 0) { break; }
    }
    return value;
  }

  // Zigzag: even is non-negative, odd is negative.
  uint32_t delta_from(uint32_t base) {
    uint32_t const z{ uvarint() };
    return ((z & 1U) == 0) ? (base + (z >> 1U)) : (base - ((z >> 1U) + 1U));
  }
};

NfcTables decode() {
  NfcTables t;
  Packed in;
  t.unsafe_lo.reserve(NFC_UNSAFE_COUNT);
  t.unsafe_hi.reserve(NFC_UNSAFE_COUNT);
  uint32_t next{ 0 };
  for (uint32_t i = 0; i < NFC_UNSAFE_COUNT; ++i) {
    uint32_t const lo{ next + in.uvarint() };
    uint32_t const hi{ lo + in.uvarint() };
    t.unsafe_lo.push_back(lo);
    t.unsafe_hi.push_back(hi);
    next = hi + 1U;
  }

  t.ccc_keys.reserve(NFC_CCC_COUNT);
  t.ccc_values.reserve(NFC_CCC_COUNT);
  next = 0;
  for (uint32_t i = 0; i < NFC_CCC_RUNS; ++i) {
    uint32_t const lo{ next + in.uvarint() };
    uint32_t const hi{ lo + in.uvarint() };
    auto const klass{ static_cast<scav_byte>(in.byte()) };
    for (uint32_t cp = lo; (cp <= hi) && (t.ccc_keys.size() < NFC_CCC_COUNT); ++cp) {
      t.ccc_keys.push_back(cp);
      t.ccc_values.push_back(klass);
    }
    next = hi + 1U;
  }

  t.decomp_keys.reserve(NFC_DECOMP_COUNT);
  t.decomp_offsets.reserve(NFC_DECOMP_COUNT);
  t.decomp_lengths.reserve(NFC_DECOMP_COUNT);
  t.decomp_data.reserve(NFC_DECOMP_DATA_COUNT);
  next = 0;
  for (uint32_t i = 0; i < NFC_DECOMP_COUNT; ++i) {
    uint32_t const key{ next + in.uvarint() };
    uint32_t const len{ imin(in.uvarint(), 0xFFU) };
    t.decomp_keys.push_back(key);
    t.decomp_offsets.push_back(narrow_clamp<uint32_t>(t.decomp_data.size()));
    t.decomp_lengths.push_back(static_cast<scav_byte>(len));
    uint32_t cp{ key };
    for (uint32_t k = 0; k < len; ++k) {
      cp = in.delta_from(cp);
      t.decomp_data.push_back(cp);
    }
    next = key + 1U;
  }

  t.compose_keys.reserve(NFC_COMPOSE_COUNT);
  t.compose_values.reserve(NFC_COMPOSE_COUNT);
  uint32_t starter{ 0 };
  uint32_t combining{ 0 };
  for (uint32_t i = 0; i < NFC_COMPOSE_COUNT; ++i) {
    if (uint32_t const step{ in.uvarint() }; step != 0) {
      starter += step;
      combining = 0;
    }
    combining += in.uvarint();
    t.compose_keys.push_back((static_cast<uint64_t>(starter) << 32U) | combining);
    t.compose_values.push_back(in.delta_from(starter));
  }
  return t;
}

uint32_t compose_pair(uint32_t starter, uint32_t combining) {
  if ((starter >= HANGUL_L_BASE) && (starter < HANGUL_L_BASE + HANGUL_L_COUNT) &&
      (combining >= HANGUL_V_BASE) && (combining < HANGUL_V_BASE + HANGUL_V_COUNT)) {
    uint32_t const l{ starter - HANGUL_L_BASE };
    uint32_t const v{ combining - HANGUL_V_BASE };
    return HANGUL_S_BASE + (((l * HANGUL_V_COUNT) + v) * HANGUL_T_COUNT);
  }
  // An LV syllable takes a trailing jamo; an LVT one is already complete.
  if (is_hangul_syllable(starter) && (((starter - HANGUL_S_BASE) % HANGUL_T_COUNT) == 0) &&
      (combining > HANGUL_T_BASE) && (combining < HANGUL_T_BASE + HANGUL_T_COUNT)) {
    return starter + (combining - HANGUL_T_BASE);
  }

  NfcTables const &t{ unicode_nfc_tables() };
  uint64_t const key{ (static_cast<uint64_t>(starter) << 32U) | combining };
  uint32_t const at{ lower_bound_key(t.compose_keys, key) };
  if ((at >= t.compose_keys.size()) || (t.compose_keys[at] != key)) { return 0; }
  return t.compose_values[at];
}

void append_decomposition(uint32_t cp, std::vector<uint32_t> &out) {
  if (is_hangul_syllable(cp)) {
    uint32_t const index{ cp - HANGUL_S_BASE };
    out.push_back(HANGUL_L_BASE + (index / HANGUL_N_COUNT));
    out.push_back(HANGUL_V_BASE + ((index % HANGUL_N_COUNT) / HANGUL_T_COUNT));
    if (uint32_t const t{ index % HANGUL_T_COUNT }; t != 0) {
      out.push_back(HANGUL_T_BASE + t);
    }
    return;
  }

  NfcTables const &t{ unicode_nfc_tables() };
  uint32_t const at{ lower_bound_key(t.decomp_keys, cp) };
  if ((at >= t.decomp_keys.size()) || (t.decomp_keys[at] != cp)) {
    out.push_back(cp);
    return;
  }
  // Already fully expanded by the generator, so this is one copy and no loop to
  // a fixed point.
  uint32_t const off{ t.decomp_offsets[at] };
  uint32_t const len{ t.decomp_lengths[at] };
  for (uint32_t i = 0; i < len; ++i) { out.push_back(t.decomp_data[off + i]); }
}

// Insertion sort over each run of non-starters -- which is what canonical
// ordering is: adjacent swaps only, equal combining classes keep their order.
void canonical_order(std::vector<uint32_t> &v) {
  size_t const n{ v.size() };
  for (size_t i = 1; i < n; ++i) {
    uint32_t const cc{ unicode_nfc_combining_class(v[i]) };
    if (cc == 0) { continue; }
    size_t j{ i };
    while ((j > 0) && (unicode_nfc_combining_class(v[j - 1]) > cc)) {
      uint32_t const tmp{ v[j - 1] };
      v[j - 1] = v[j];
      v[j] = tmp;
      --j;
    }
  }
}

}  // namespace

NfcTables const &unicode_nfc_tables() {
  static NfcTables const TABLES{ decode() };
  return TABLES;
}

bool unicode_nfc_needs_work(uint32_t cp) {
  if (cp < 0x300) { return false; }  // the first unsafe codepoint is U+0300
  // Ranges ascend and do not overlap, so the last one starting at or below `cp`
  // is the only candidate.
  NfcTables const &t{ unicode_nfc_tables() };
  uint32_t lo{ 0 };
  uint32_t hi{ narrow_clamp<uint32_t>(t.unsafe_lo.size()) };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (t.unsafe_lo[mid] <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) { return false; }
  return cp <= t.unsafe_hi[lo - 1];
}

uint32_t unicode_nfc_combining_class(uint32_t cp) {
  if (cp < 0x300) { return 0; }
  NfcTables const &t{ unicode_nfc_tables() };
  uint32_t const at{ lower_bound_key(t.ccc_keys, cp) };
  if ((at >= t.ccc_keys.size()) || (t.ccc_keys[at] != cp)) { return 0; }
  return t.ccc_values[at];
}

bool unicode_nfc_normalize(std::vector<uint32_t> const &in, std::vector<uint32_t> &out) {
  out.clear();

  bool any_unsafe{ false };
  for (uint32_t const cp : in) {
    if (unicode_nfc_needs_work(cp)) {
      any_unsafe = true;
      break;
    }
  }
  if (!any_unsafe) {
    out = in;
    return false;
  }

  std::vector<uint32_t> decomposed;
  decomposed.reserve(in.size() + (in.size() / 2));
  for (uint32_t const cp : in) { append_decomposition(cp, decomposed); }
  canonical_order(decomposed);

  // Unicode 3.11 canonical composition. A mark composes with `starter` only when
  // nothing between them blocks it, which is what `last_class` tracks.
  out.reserve(decomposed.size());
  size_t starter{ 0 };
  bool have_starter{ false };
  uint32_t last_class{ 0 };
  for (uint32_t const cp : decomposed) {
    uint32_t const cc{ unicode_nfc_combining_class(cp) };
    bool const blocked{ have_starter && (last_class != 0) && (last_class >= cc) };
    if (have_starter && !blocked) {
      if (uint32_t const composed{ compose_pair(out[starter], cp) }; composed != 0) {
        out[starter] = composed;
        // last_class is not updated: the mark was absorbed, so it never
        // becomes the blocker for the next one.
        continue;
      }
    }
    if (cc == 0) {
      starter = out.size();
      have_starter = true;
    }
    last_class = cc;
    out.push_back(cp);
  }

  return out != in;
}

}  // namespace scav
