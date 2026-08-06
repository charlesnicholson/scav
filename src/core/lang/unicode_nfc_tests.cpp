// NFC, checked against Unicode's own conformance suite plus the cases the
// algorithm can get wrong independently of the table.

#include "core/lang/unicode_nfc.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace scav;

#include "core/lang/unicode_nfc_vectors.inc"  // NOLINT(bugprone-suspicious-include)

std::vector<uint32_t> cps(std::vector<uint32_t> const &v) { return v; }

std::vector<uint32_t> nfc(std::vector<uint32_t> const &in) {
  std::vector<uint32_t> out;
  nfc_normalize(in, out);
  return out;
}

std::string describe(std::vector<uint32_t> const &v) {
  static constexpr char const *DIGITS{ "0123456789ABCDEF" };
  std::string out;
  for (uint32_t const cp : v) {
    if (!out.empty()) { out.push_back(' '); }
    out += "U+";
    bool started{ false };
    for (int32_t shift = 20; shift >= 0; shift -= 4) {
      uint32_t const nibble{ (cp >> static_cast<uint32_t>(shift)) & 0xFU };
      if ((nibble != 0) || started || (shift <= 12)) {
        out.push_back(DIGITS[nibble]);
        started = true;
      }
    }
  }
  return out;
}

}  // namespace

TEST_CASE("nfc: ASCII needs no work and normalizes to itself") {
  for (uint32_t cp = 0; cp < 0x80; ++cp) { CHECK_FALSE(nfc_needs_work(cp)); }
  std::vector<uint32_t> ascii;
  for (uint32_t cp = 1; cp < 0x80; ++cp) { ascii.push_back(cp); }
  std::vector<uint32_t> out;
  CHECK_FALSE(nfc_normalize(ascii, out));
  CHECK(out == ascii);
}

TEST_CASE("nfc: an empty sequence is already normalized") {
  std::vector<uint32_t> const empty;
  std::vector<uint32_t> out{ 1, 2, 3 };
  CHECK_FALSE(nfc_normalize(empty, out));
  CHECK(out.empty());
}

TEST_CASE("nfc: combining classes match the UCD") {
  CHECK(nfc_combining_class('a') == 0);
  CHECK(nfc_combining_class(0x0301) == 230);  // combining acute
  CHECK(nfc_combining_class(0x0327) == 202);  // combining cedilla
  CHECK(nfc_combining_class(0x0334) == 1);    // combining tilde overlay
  CHECK(nfc_combining_class(0x00E9) == 0);    // precomposed e-acute is a starter
  CHECK(nfc_combining_class(0x10FFFF) == 0);
}

TEST_CASE("nfc: quick check flags exactly what may move") {
  CHECK(nfc_needs_work(0x0301));  // a combining mark reorders
  CHECK(nfc_needs_work(0x0340));  // NFC_QC=No, a singleton decomposition
  CHECK_FALSE(nfc_needs_work(0x00E9));
  CHECK_FALSE(nfc_needs_work(0x0041));
  CHECK_FALSE(nfc_needs_work(0x10FFFF));
}

TEST_CASE("nfc: composes a base and a combining mark") {
  CHECK(nfc({ 'e', 0x0301 }) == cps({ 0x00E9 }));
  CHECK(nfc({ 'A', 0x030A }) == cps({ 0x00C5 }));
  // Reordering runs first: ogonek is class 202 and acute is 230, so ogonek
  // reaches the base and the acute is left over with nothing to compose into.
  CHECK(nfc({ 0x0041, 0x0301, 0x0328 }) == cps({ 0x0104, 0x0301 }));
}

TEST_CASE("nfc: recomposes a fully decomposed sequence") {
  // U+1E69 is s-dot-below-dot-above: two marks with different classes, and the
  // order they compose in is the order canonical ordering puts them in.
  CHECK(nfc({ 0x0073, 0x0323, 0x0307 }) == cps({ 0x1E69 }));
  CHECK(nfc({ 0x0073, 0x0307, 0x0323 }) == cps({ 0x1E69 }));
  CHECK(nfc({ 0x1E69 }) == cps({ 0x1E69 }));
}

TEST_CASE("nfc: canonical ordering sorts marks by combining class, stably") {
  // 0x0334 is class 1, 0x0301 is class 230. Reordering puts the lower class
  // first; two marks of the same class keep their order.
  CHECK(nfc({ 'a', 0x0301, 0x0334 }) == cps({ 0x00E1, 0x0334 }));
  CHECK(nfc({ 'q', 0x0307, 0x0323 }) == cps({ 'q', 0x0323, 0x0307 }));
  CHECK(nfc({ 'q', 0x0301, 0x0300 }) == cps({ 'q', 0x0301, 0x0300 }));
}

TEST_CASE("nfc: a composition exclusion stays decomposed") {
  // U+0344 has a canonical decomposition but is composition-excluded, so NFC
  // decomposes it and does not put it back.
  CHECK(nfc({ 0x0344 }) == cps({ 0x0308, 0x0301 }));
  // U+0958 is a script-specific exclusion.
  CHECK(nfc({ 0x0958 }) == cps({ 0x0915, 0x093C }));
  CHECK(nfc({ 0x0915, 0x093C }) == cps({ 0x0915, 0x093C }));
}

TEST_CASE("nfc: a singleton decomposition is replaced and not restored") {
  CHECK(nfc({ 0x0340 }) == cps({ 0x0300 }));  // combining grave tone mark
  CHECK(nfc({ 0x212B }) == cps({ 0x00C5 }));  // angstrom sign -> A with ring
  CHECK(nfc({ 0x2126 }) == cps({ 0x03A9 }));  // ohm sign -> capital omega
}

TEST_CASE("nfc: a blocked mark does not reach past the one in front of it") {
  // Same combining class, so the second is blocked from the base even though
  // the base could compose with it in isolation.
  CHECK(nfc({ 'a', 0x0328, 0x0301 }) == cps({ 0x0105, 0x0301 }));
  CHECK(nfc({ 'a', 0x0301, 0x0328 }) == cps({ 0x0105, 0x0301 }));
}

TEST_CASE("nfc: a leading combining mark has no starter to attach to") {
  CHECK(nfc({ 0x0301 }) == cps({ 0x0301 }));
  CHECK(nfc({ 0x0301, 'e' }) == cps({ 0x0301, 'e' }));
  CHECK(nfc({ 0x0301, 0x0300 }) == cps({ 0x0301, 0x0300 }));
}

TEST_CASE("nfc: Hangul composes and decomposes algorithmically") {
  CHECK(nfc({ 0x1100, 0x1161 }) == cps({ 0xAC00 }));          // L + V -> LV
  CHECK(nfc({ 0x1100, 0x1161, 0x11A8 }) == cps({ 0xAC01 }));  // L + V + T -> LVT
  CHECK(nfc({ 0xAC00, 0x11A8 }) == cps({ 0xAC01 }));          // LV + T -> LVT
  CHECK(nfc({ 0xAC01, 0x11A8 }) == cps({ 0xAC01, 0x11A8 }));  // LVT takes no more
  CHECK(nfc({ 0xD7A3 }) == cps({ 0xD7A3 }));                  // the last syllable
  CHECK(nfc({ 0x1112, 0x1175, 0x11C2 }) == cps({ 0xD7A3 }));
}

TEST_CASE("nfc: every Hangul syllable round-trips through its jamo") {
  // 11,172 syllables, none of which has a table entry -- if the arithmetic is
  // wrong anywhere it is wrong here.
  constexpr uint32_t S_BASE{ 0xAC00 };
  constexpr uint32_t COUNT{ 19 * 21 * 28 };
  for (uint32_t i = 0; i < COUNT; ++i) {
    uint32_t const syllable{ S_BASE + i };
    std::vector<uint32_t> const composed{ nfc({ syllable }) };
    REQUIRE(composed.size() == 1);
    CHECK(composed[0] == syllable);

    uint32_t const l{ 0x1100 + (i / (21U * 28U)) };
    uint32_t const v{ 0x1161 + ((i % (21U * 28U)) / 28U) };
    uint32_t const t{ i % 28U };
    std::vector<uint32_t> jamo{ l, v };
    if (t != 0) { jamo.push_back(0x11A7 + t); }
    CHECK(nfc(jamo) == composed);
  }
}

TEST_CASE("nfc: normalizing an already-normalized string reports no change") {
  std::vector<uint32_t> out;
  CHECK_FALSE(nfc_normalize({ 'a', 'b', 'c' }, out));
  CHECK(nfc_normalize({ 'e', 0x0301 }, out));
  CHECK(out == cps({ 0x00E9 }));
  // A precomposed character followed by a lower-class mark is *not* already
  // NFC: it decomposes, reorders, and recomposes around the other mark.
  CHECK(nfc_normalize({ 0x00E9, 0x0328 }, out));
  CHECK(out == cps({ 0x0119, 0x0301 }));
  // One that really is unchanged, down the same slow path.
  CHECK_FALSE(nfc_normalize({ 0x00E9, 0x0301 }, out));
}

TEST_CASE("nfc: idempotent on the conformance suite") {
  uint32_t at{ 0 };
  for (uint32_t i = 0; i < NFC_VECTOR_COUNT; ++i) {
    uint32_t const src_len{ static_cast<uint32_t>(NFC_VECTOR_LENS[i] >> 8U) };
    uint32_t const exp_len{ static_cast<uint32_t>(NFC_VECTOR_LENS[i] & 0xFFU) };

    std::vector<uint32_t> source;
    source.reserve(src_len);
    for (uint32_t k = 0; k < src_len; ++k) { source.push_back(NFC_VECTOR_DATA[at + k]); }
    at += src_len;

    std::vector<uint32_t> expected;
    if (exp_len == 0) {
      expected = source;  // the generator elides an expectation equal to its source
    } else {
      expected.reserve(exp_len);
      for (uint32_t k = 0; k < exp_len; ++k) {
        expected.push_back(NFC_VECTOR_DATA[at + k]);
      }
      at += exp_len;
    }

    std::vector<uint32_t> produced;
    nfc_normalize(source, produced);
    if (produced != expected) {
      FAIL("case " << i << ": NFC(" << describe(source) << ") = " << describe(produced)
                   << ", want " << describe(expected));
    }

    // NFC is idempotent, which the suite does not state as a row but which any
    // composition bug breaks.
    std::vector<uint32_t> again;
    nfc_normalize(produced, again);
    if (again != expected) {
      FAIL("case " << i << " is not idempotent: " << describe(again));
    }
  }
  // Every codepoint in the table belongs to some case; a mismatch means the
  // generator and the reader disagree about the encoding.
  CHECK(at == static_cast<uint32_t>(NFC_VECTOR_DATA.size()));
  MESSAGE("conformance cases checked: " << NFC_VECTOR_COUNT);
}
