#include "core/lang/unicode_nfc.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

#include "core/lang/unicode_nfc_tables.inc"  // NOLINT(bugprone-suspicious-include)

// Hangul composition and decomposition, Unicode 3.12. Table-free by
// construction, which is why the generator drops the 11,172 syllables it would
// otherwise emit.
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

// Every table below is sorted by key, so every lookup is a binary search. The
// hot path never reaches one: ASCII short-circuits above the call.
template <size_t N>
uint32_t lower_bound_u32(std::array<uint32_t, N> const &keys,
                         uint32_t count,
                         uint32_t key) {
  uint32_t lo{ 0 };
  uint32_t hi{ count };
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

template <size_t N>
uint32_t lower_bound_u64(std::array<uint64_t, N> const &keys,
                         uint32_t count,
                         uint64_t key) {
  uint32_t lo{ 0 };
  uint32_t hi{ count };
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

  uint64_t const key{ (static_cast<uint64_t>(starter) << 32U) | combining };
  uint32_t const at{ lower_bound_u64(COMPOSE_KEYS, COMPOSE_COUNT, key) };
  if ((at >= COMPOSE_COUNT) || (COMPOSE_KEYS[at] != key)) { return 0; }
  return COMPOSE_VALUES[at];
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

  uint32_t const at{ lower_bound_u32(DECOMP_KEYS, DECOMP_COUNT, cp) };
  if ((at >= DECOMP_COUNT) || (DECOMP_KEYS[at] != cp)) {
    out.push_back(cp);
    return;
  }
  // Already fully expanded by the generator, so this is one copy and no loop to
  // a fixed point.
  uint32_t const off{ DECOMP_OFFSETS[at] };
  uint32_t const len{ DECOMP_LENGTHS[at] };
  for (uint32_t i = 0; i < len; ++i) { out.push_back(DECOMP_DATA[off + i]); }
}

// Insertion sort over each run of non-starters, which is what the canonical
// ordering algorithm is: only adjacent out-of-order pairs may swap, and equal
// combining classes must keep their input order.
void canonical_order(std::vector<uint32_t> &v) {
  size_t const n{ v.size() };
  for (size_t i = 1; i < n; ++i) {
    uint32_t const cc{ nfc_combining_class(v[i]) };
    if (cc == 0) { continue; }
    size_t j{ i };
    while ((j > 0) && (nfc_combining_class(v[j - 1]) > cc)) {
      uint32_t const tmp{ v[j - 1] };
      v[j - 1] = v[j];
      v[j] = tmp;
      --j;
    }
  }
}

}  // namespace

bool nfc_needs_work(uint32_t cp) {
  if (cp < 0x300) { return false; }  // the first unsafe codepoint is U+0300
  // Ranges ascend and do not overlap, so the last one starting at or below `cp`
  // is the only candidate.
  uint32_t lo{ 0 };
  uint32_t hi{ NFC_UNSAFE_RANGE_COUNT };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (NFC_UNSAFE_LO[mid] <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) { return false; }
  return cp <= NFC_UNSAFE_HI[lo - 1];
}

uint32_t nfc_combining_class(uint32_t cp) {
  if (cp < 0x300) { return 0; }
  uint32_t const at{ lower_bound_u32(CCC_KEYS, CCC_COUNT, cp) };
  if ((at >= CCC_COUNT) || (CCC_KEYS[at] != cp)) { return 0; }
  return CCC_VALUES[at];
}

bool nfc_normalize(std::vector<uint32_t> const &in, std::vector<uint32_t> &out) {
  out.clear();

  bool any_unsafe{ false };
  for (uint32_t const cp : in) {
    if (nfc_needs_work(cp)) {
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

  // Unicode 3.11 canonical composition. `starter` indexes into `out`; a
  // combining mark composes with it only when nothing between them blocks it,
  // which is what `last_class` tracks.
  out.reserve(decomposed.size());
  size_t starter{ 0 };
  bool have_starter{ false };
  uint32_t last_class{ 0 };
  for (uint32_t const cp : decomposed) {
    uint32_t const cc{ nfc_combining_class(cp) };
    bool const blocked{ have_starter && (last_class != 0) && (last_class >= cc) };
    if (have_starter && !blocked) {
      if (uint32_t const composed{ compose_pair(out[starter], cp) }; composed != 0) {
        out[starter] = composed;
        // last_class is deliberately not updated: the combining mark was
        // absorbed, so it never becomes the blocker for the next one.
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
