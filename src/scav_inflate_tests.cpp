// Hand-built bitstreams for every block type and every rejection, a zlib stream
// as a reference, and truncation and bit-flip sweeps over valid streams.

#include "scav_inflate.h"

#include "scav/scav_types.h"

#include "doctest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scav;
using Bytes = std::vector<scav_byte>;

// LSB-first, as DEFLATE packs every field except a Huffman code.
struct BitWriter {
  Bytes bytes;
  uint32_t used{ 0 };  // bits filled in the last byte

  void put(uint32_t value, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      if (used == 0U) { bytes.push_back(0); }
      bytes.back() = static_cast<scav_byte>(bytes.back() | (((value >> i) & 1U) << used));
      used = (used + 1U) % 8U;
    }
  }

  void code(uint32_t value, uint32_t n) {  // a Huffman code, first bit highest
    for (uint32_t i = n; i-- > 0U;) { put((value >> i) & 1U, 1); }
  }

  void align() { used = 0; }

  void raw(std::string_view s) {
    for (char const c : s) { bytes.push_back(static_cast<scav_byte>(c)); }
  }
};

// RFC 1951 3.2.2, as an independent statement of the canonical assignment.
std::vector<uint32_t> canonical(std::vector<uint8_t> const &lengths) {
  std::array<uint32_t, 16> count{};
  for (uint8_t const l : lengths) { ++count[l]; }
  count[0] = 0;
  std::array<uint32_t, 16> next{};
  uint32_t code{ 0 };
  for (uint32_t bits = 1; bits < 16U; ++bits) {
    code = (code + count[bits - 1U]) << 1U;
    next[bits] = code;
  }
  std::vector<uint32_t> codes(lengths.size(), 0);
  for (size_t s = 0; s < lengths.size(); ++s) {
    if (lengths[s] != 0U) { codes[s] = next[lengths[s]]++; }
  }
  return codes;
}

struct Code {
  std::vector<uint8_t> lengths;
  std::vector<uint32_t> codes;

  explicit Code(std::vector<uint8_t> l)
      : lengths(std::move(l)), codes(canonical(lengths)) {}

  void put(BitWriter &w, uint32_t sym) const { w.code(codes[sym], lengths[sym]); }
};

Code fixed_lit() {
  std::vector<uint8_t> l(288, 8);
  for (uint32_t s = 144; s < 256U; ++s) { l[s] = 9; }
  for (uint32_t s = 256; s < 280U; ++s) { l[s] = 7; }
  return Code{ l };
}

// Complete over all 19 symbols: 0..12 take 4 bits, 13..18 take 5.
Code clen_code() {
  std::vector<uint8_t> l(19, 4);
  for (uint32_t s = 13; s < 19U; ++s) { l[s] = 5; }
  return Code{ l };
}

constexpr std::array<uint8_t, 19> CLEN_ORDER{ 16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                              11, 4,  12, 3, 13, 2, 14, 1, 15 };

void block_header(BitWriter &w, uint32_t final, uint32_t type) {
  w.put(final, 1);
  w.put(type, 2);
}

// HLIT, HDIST and HCLEN = 19, then the code-length code's lengths in list order.
void dynamic_counts(BitWriter &w, uint32_t nlit, uint32_t ndist, Code const &clen) {
  w.put(nlit - 257U, 5);
  w.put(ndist - 1U, 5);
  w.put(15, 4);
  for (uint8_t const s : CLEN_ORDER) { w.put(clen.lengths[s], 3); }
}

// A dynamic header writing each length as its own symbol, no repeats.
void dynamic_header(BitWriter &w, Code const &lit, Code const &dist) {
  Code const clen{ clen_code() };
  auto const nlit{ static_cast<uint32_t>(lit.lengths.size()) };
  auto const ndist{ static_cast<uint32_t>(dist.lengths.size()) };
  dynamic_counts(w, nlit, ndist, clen);
  for (uint8_t const l : lit.lengths) { clen.put(w, l); }
  for (uint8_t const l : dist.lengths) { clen.put(w, l); }
}

constexpr std::array<uint16_t, 29> LEN_BASE{ 3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                             15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr std::array<uint8_t, 29> LEN_EXTRA{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr std::array<uint16_t, 30> DIST_BASE{
  1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
constexpr std::array<uint8_t, 30> DIST_EXTRA{ 0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                              4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                              9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

// A <length, distance> pair as symbols plus extra bits, searched rather than computed.
void put_match(BitWriter &w,
               Code const &lit,
               Code const &dist,
               uint32_t length,
               uint32_t back) {
  uint32_t l{ 28 };
  while (LEN_BASE[l] > length) { --l; }
  lit.put(w, 257U + l);
  w.put(length - LEN_BASE[l], LEN_EXTRA[l]);
  uint32_t d{ 29 };
  while (DIST_BASE[d] > back) { --d; }
  dist.put(w, d);
  w.put(back - DIST_BASE[d], DIST_EXTRA[d]);
}

Code fixed_dist() { return Code{ std::vector<uint8_t>(32, 5) }; }

struct Result {
  InflateStatus status;
  Bytes out;
};

Result run(Bytes const &in, uint32_t cap = 1U << 20U) {
  Result r{ .status = InflateStatus::Ok, .out = Bytes(cap) };
  uint32_t n{ 0 };
  r.status = inflate(in.data(), static_cast<uint32_t>(in.size()), r.out.data(), cap, n);
  CHECK(n <= cap);
  r.out.resize(n);
  return r;
}

Bytes text(std::string_view s) { return { s.begin(), s.end() }; }

// Plain text as literals in one fixed block.
Bytes fixed_literals(std::string_view s) {
  Code const lit{ fixed_lit() };
  BitWriter w;
  block_header(w, 1, 1);
  for (char const c : s) { lit.put(w, static_cast<scav_byte>(c)); }
  lit.put(w, 256);
  return w.bytes;
}

}  // namespace

TEST_CASE("inflate: a stored block copies LEN bytes") {
  BitWriter w;
  block_header(w, 1, 0);
  w.align();
  w.put(5, 16);
  w.put(0xFFFFU ^ 5U, 16);
  w.raw("hello");
  Result const r{ run(w.bytes) };
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.out == text("hello"));
}

TEST_CASE("inflate: a stored block with NLEN not LEN's complement is rejected") {
  BitWriter w;
  block_header(w, 1, 0);
  w.align();
  w.put(5, 16);
  w.put(0xFFFFU ^ 4U, 16);
  w.raw("hello");
  CHECK(run(w.bytes).status == InflateStatus::BadStoredLength);
}

TEST_CASE("inflate: a stored block aligns after a Huffman block ends mid-byte") {
  Code const lit{ fixed_lit() };
  BitWriter w;
  block_header(w, 0, 1);
  lit.put(w, 'a');
  lit.put(w, 256);
  block_header(w, 0, 0);
  w.align();
  w.put(0, 16);
  w.put(0xFFFF, 16);
  block_header(w, 1, 0);
  w.align();
  w.put(3, 16);
  w.put(0xFFFFU ^ 3U, 16);
  w.raw("bcd");
  Result const r{ run(w.bytes) };
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.out == text("abcd"));
}

TEST_CASE("inflate: a stored block longer than the input or the output is rejected") {
  BitWriter w;
  block_header(w, 1, 0);
  w.align();
  w.put(6, 16);
  w.put(0xFFFFU ^ 6U, 16);
  w.raw("hello");
  CHECK(run(w.bytes).status == InflateStatus::Truncated);
  w.raw("!");
  CHECK(run(w.bytes, 5).status == InflateStatus::OutputFull);
  CHECK(run(w.bytes, 6).status == InflateStatus::Ok);
  w.bytes.resize(3);  // inside NLEN
  CHECK(run(w.bytes).status == InflateStatus::Truncated);
}

TEST_CASE("inflate: a fixed block written by hand") {
  Code const lit{ fixed_lit() };
  Code const dist{ fixed_dist() };
  BitWriter w;
  block_header(w, 1, 1);
  for (char const c : std::string_view{ "abc" }) { lit.put(w, static_cast<scav_byte>(c)); }
  lit.put(w, 0xE9);  // a 9-bit literal
  put_match(w, lit, dist, 4, 4);
  put_match(w, lit, dist, 10, 1);  // overlapping: repeats the last byte
  lit.put(w, 256);
  Result const r{ run(w.bytes) };
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.out == Bytes{ 'a',
                        'b',
                        'c',
                        0xE9,
                        'a',
                        'b',
                        'c',
                        0xE9,
                        0xE9,
                        0xE9,
                        0xE9,
                        0xE9,
                        0xE9,
                        0xE9,
                        0xE9,
                        0xE9,
                        0xE9,
                        0xE9 });
}

TEST_CASE("inflate: every length and every distance code, at both ends of its range") {
  Code const lit{ fixed_lit() };
  Code const dist{ fixed_dist() };
  auto check = [&](uint32_t length, uint32_t back) {
    BitWriter w;
    block_header(w, 1, 1);
    Bytes want;
    for (uint32_t i = 0; i < back; ++i) {
      auto const c{ static_cast<scav_byte>((i * 7U) + (i >> 8U)) };
      lit.put(w, c);
      want.push_back(c);
    }
    put_match(w, lit, dist, length, back);
    for (uint32_t i = 0; i < length; ++i) { want.push_back(want[want.size() - back]); }
    lit.put(w, 256);
    Result const r{ run(w.bytes) };
    CAPTURE(length);
    CAPTURE(back);
    CHECK(r.status == InflateStatus::Ok);
    CHECK(r.out == want);
  };
  for (uint32_t length = 3; length <= 258U; ++length) {
    check(length, 1);
    check(length, 3);
  }
  for (uint32_t d = 0; d < DIST_BASE.size(); ++d) {
    uint32_t const lo{ DIST_BASE[d] };
    uint32_t const hi{ lo + (1U << DIST_EXTRA[d]) - 1U };
    check(3, lo);
    check(258, lo);
    check(3, hi);
    check(258, hi);
  }
}

TEST_CASE("inflate: block type 3 is rejected") {
  BitWriter w;
  block_header(w, 1, 3);
  w.put(0, 16);
  CHECK(run(w.bytes).status == InflateStatus::BadBlockType);
}

TEST_CASE("inflate: fixed length symbols 286 and 287 are rejected") {
  for (uint32_t const sym : { 286U, 287U }) {
    Code const lit{ fixed_lit() };
    BitWriter w;
    block_header(w, 1, 1);
    lit.put(w, 'a');
    lit.put(w, sym);
    w.put(0, 16);
    Result const r{ run(w.bytes) };
    CHECK(r.status == InflateStatus::BadLengthSymbol);
    CHECK(r.out == text("a"));
  }
}

TEST_CASE("inflate: fixed distance symbols 30 and 31 are rejected") {
  for (uint32_t const sym : { 30U, 31U }) {
    Code const lit{ fixed_lit() };
    BitWriter w;
    block_header(w, 1, 1);
    for (int i = 0; i < 4; ++i) { lit.put(w, 'a'); }
    lit.put(w, 257);
    w.code(sym, 5);
    w.put(0, 16);
    CHECK(run(w.bytes).status == InflateStatus::BadDistanceSymbol);
  }
}

TEST_CASE("inflate: a distance farther back than the output is rejected") {
  Code const lit{ fixed_lit() };
  Code const dist{ fixed_dist() };
  for (uint32_t const back : { 1U, 3U, 32768U }) {
    BitWriter w;
    block_header(w, 1, 1);
    uint32_t const have{ (back == 1U) ? 0U : 2U };
    for (uint32_t i = 0; i < have; ++i) { lit.put(w, 'a'); }
    put_match(w, lit, dist, 3, back);
    lit.put(w, 256);
    CAPTURE(back);
    CHECK(run(w.bytes).status == InflateStatus::BadDistance);
  }
}

TEST_CASE("inflate: a block without end-of-block is truncated") {
  Bytes s{ fixed_literals("abc") };
  s.pop_back();
  CHECK(run(s).status == InflateStatus::Truncated);
  CHECK(run({}).status == InflateStatus::Truncated);
  BitWriter w;
  block_header(w, 0, 0);  // not final, and nothing follows
  w.align();
  w.put(0, 16);
  w.put(0xFFFF, 16);
  CHECK(run(w.bytes).status == InflateStatus::Truncated);
}

TEST_CASE(
    "inflate: output past the caller's bound is refused, and nothing past it written") {
  Bytes const s{ fixed_literals("abcdef") };
  for (uint32_t cap = 0; cap < 6U; ++cap) {
    Bytes out(8, 0x5A);
    uint32_t n{ 99 };
    CHECK(inflate(s.data(), static_cast<uint32_t>(s.size()), out.data(), cap, n) ==
          InflateStatus::OutputFull);
    CHECK(n == cap);
    for (uint32_t i = cap; i < 8U; ++i) { CHECK(out[i] == 0x5A); }
  }
  CHECK(run(s, 6).status == InflateStatus::Ok);

  Code const lit{ fixed_lit() };
  Code const dist{ fixed_dist() };
  BitWriter w;
  block_header(w, 1, 1);
  lit.put(w, 'a');
  put_match(w, lit, dist, 5, 1);
  lit.put(w, 256);
  CHECK(run(w.bytes, 5).status == InflateStatus::OutputFull);
  CHECK(run(w.bytes, 6).status == InflateStatus::Ok);

  uint32_t n{ 99 };
  Bytes const empty{ fixed_literals("") };
  CHECK(inflate(empty.data(), static_cast<uint32_t>(empty.size()), nullptr, 0, n) ==
        InflateStatus::Ok);
  CHECK(n == 0);
}

namespace {

// Literals a..c, end-of-block and length symbol 257, two 1-bit distance codes.
Code small_lit() {
  std::vector<uint8_t> l(258, 0);
  l['a'] = 2;
  l['b'] = 2;
  l['c'] = 3;
  l[256] = 3;
  l[257] = 2;
  return Code{ l };
}

Code small_dist() { return Code{ { 1, 1 } }; }

// "abc", then <3, 2> for "bcb", then "a".
Bytes small_dynamic() {
  Code const lit{ small_lit() };
  Code const dist{ small_dist() };
  BitWriter w;
  block_header(w, 1, 2);
  dynamic_header(w, lit, dist);
  lit.put(w, 'a');
  lit.put(w, 'b');
  lit.put(w, 'c');
  put_match(w, lit, dist, 3, 2);
  lit.put(w, 'a');
  lit.put(w, 256);
  return w.bytes;
}

// Literal/length lengths 1..15 for symbols 0..14, and 15 for end-of-block.
Code deep_lit() {
  std::vector<uint8_t> l(257, 0);
  for (uint32_t s = 0; s < 15U; ++s) { l[s] = static_cast<uint8_t>(s + 1U); }
  l[256] = 15;
  return Code{ l };
}

Bytes deep_dynamic() {
  Code const lit{ deep_lit() };
  BitWriter w;
  block_header(w, 1, 2);
  dynamic_header(w, lit, small_dist());
  for (uint32_t s = 15; s-- > 0U;) { lit.put(w, s); }
  lit.put(w, 256);
  return w.bytes;
}

// A dynamic header for these lengths, then padding in place of data.
Bytes dynamic_with(std::vector<uint8_t> const &lit, std::vector<uint8_t> const &dist) {
  BitWriter w;
  block_header(w, 1, 2);
  dynamic_header(w, Code{ lit }, Code{ dist });
  w.put(0, 32);
  return w.bytes;
}

}  // namespace

TEST_CASE("inflate: a dynamic block written by hand") {
  Result const r{ run(small_dynamic()) };
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.out == text("abcbcba"));
}

TEST_CASE("inflate: codes of every length from 1 to 15 bits") {
  Result const r{ run(deep_dynamic()) };
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.out == Bytes{ 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 });
}

TEST_CASE("inflate: code-length repeats 16, 17 and 18") {
  Code const clen{ clen_code() };
  std::vector<uint8_t> lengths(258, 0);
  for (uint32_t s = 'a'; s <= 'd'; ++s) { lengths[s] = 3; }
  lengths[256] = 2;
  lengths[257] = 2;
  Code const lit{ lengths };
  Code const dist{ { 1, 0, 0, 1 } };
  BitWriter w;
  block_header(w, 1, 2);
  dynamic_counts(w, 258, 4, clen);
  clen.put(w, 18);  // 11 + 86 zeros, symbols 0..96
  w.put(86, 7);
  clen.put(w, 3);   // 'a'
  clen.put(w, 16);  // 'b', 'c', 'd'
  w.put(0, 2);
  clen.put(w, 17);  // 3 + 7 zeros
  w.put(7, 3);
  clen.put(w, 18);  // 11 + 127 zeros
  w.put(127, 7);
  clen.put(w, 17);  // 3 + 4 zeros, through 255
  w.put(4, 3);
  clen.put(w, 2);  // 256
  clen.put(w, 2);  // 257
  for (uint8_t const l : dist.lengths) { clen.put(w, l); }
  for (uint32_t s = 'a'; s <= 'd'; ++s) { lit.put(w, s); }
  put_match(w, lit, dist, 3, 4);
  lit.put(w, 'd');
  lit.put(w, 256);
  Result const r{ run(w.bytes) };
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.out == text("abcdabcd"));
}

TEST_CASE("inflate: a repeat with no previous length, or past the end, is rejected") {
  Code const clen{ clen_code() };
  BitWriter first;
  block_header(first, 1, 2);
  dynamic_counts(first, 257, 1, clen);
  clen.put(first, 16);
  first.put(0, 2);
  first.put(0, 32);
  CHECK(run(first.bytes).status == InflateStatus::BadRepeat);

  BitWriter over;
  block_header(over, 1, 2);
  dynamic_counts(over, 257, 1, clen);
  clen.put(over, 18);
  over.put(127, 7);
  clen.put(over, 18);
  over.put(127, 7);  // 276 lengths, past the 258 declared
  over.put(0, 32);
  CHECK(run(over.bytes).status == InflateStatus::BadRepeat);
}

TEST_CASE("inflate: HLIT above 286 and HDIST above 30 are rejected") {
  Code const clen{ clen_code() };
  for (auto const &[nlit, ndist] : { std::pair{ 287U, 30U },
                                     std::pair{ 288U, 1U },
                                     std::pair{ 286U, 31U },
                                     std::pair{ 257U, 32U } }) {
    BitWriter w;
    block_header(w, 1, 2);
    dynamic_counts(w, nlit, ndist, clen);
    w.put(0, 32);
    CAPTURE(nlit);
    CAPTURE(ndist);
    CHECK(run(w.bytes).status == InflateStatus::BadCodeCounts);
  }
  std::vector<uint8_t> most(286, 0);  // 286 and 30 themselves are allowed
  most[0] = 1;
  most[256] = 1;
  Code const lit{ most };
  BitWriter edge;
  block_header(edge, 1, 2);
  dynamic_header(edge, lit, Code{ std::vector<uint8_t>(30, 0) });
  lit.put(edge, 256);
  CHECK(run(edge.bytes).status == InflateStatus::Ok);
}

TEST_CASE("inflate: an over-subscribed or incomplete code-length code is rejected") {
  BitWriter over;
  block_header(over, 1, 2);
  dynamic_counts(over, 257, 1, Code{ std::vector<uint8_t>(19, 1) });
  over.put(0, 32);
  CHECK(run(over.bytes).status == InflateStatus::OverSubscribed);

  std::vector<uint8_t> gap(19, 0);
  gap[0] = 1;  // one 1-bit code: allowed for data, never for the code-length code
  BitWriter incomplete;
  block_header(incomplete, 1, 2);
  dynamic_counts(incomplete, 257, 1, Code{ gap });
  incomplete.put(0, 32);
  CHECK(run(incomplete.bytes).status == InflateStatus::Incomplete);
}

TEST_CASE("inflate: an over-subscribed or incomplete data code is rejected") {
  std::vector<uint8_t> lit(257, 0);
  lit[256] = 1;
  lit['a'] = 1;
  lit['b'] = 1;
  CHECK(run(dynamic_with(lit, { 1, 1 })).status == InflateStatus::OverSubscribed);
  lit['b'] = 0;
  CHECK(run(dynamic_with(lit, { 1, 1, 1 })).status == InflateStatus::OverSubscribed);

  lit['a'] = 2;  // 1 + 2 bits leaves a 2-bit gap
  CHECK(run(dynamic_with(lit, { 1, 1 })).status == InflateStatus::Incomplete);
  lit['a'] = 1;
  CHECK(run(dynamic_with(lit, { 2 })).status == InflateStatus::Incomplete);
  CHECK(run(dynamic_with(lit, { 1, 2 })).status == InflateStatus::Incomplete);
}

TEST_CASE("inflate: a lone 1-bit code, or no distance code at all, is allowed") {
  std::vector<uint8_t> eob(257, 0);
  eob[256] = 1;
  BitWriter lone;
  block_header(lone, 1, 2);
  dynamic_header(lone, Code{ eob }, Code{ { 0, 1 } });
  lone.code(0, 1);
  CHECK(run(lone.bytes).status == InflateStatus::Ok);

  BitWriter unassigned;  // the lone code leaves pattern 1 unassigned
  block_header(unassigned, 1, 2);
  dynamic_header(unassigned, Code{ eob }, Code{ { 1 } });
  unassigned.code(1, 1);
  unassigned.put(0, 16);
  CHECK(run(unassigned.bytes).status == InflateStatus::BadLengthSymbol);

  std::vector<uint8_t> matching(258, 0);
  matching['a'] = 1;
  matching[256] = 2;
  matching[257] = 2;
  Code const m{ matching };
  BitWriter literal_only;
  block_header(literal_only, 1, 2);
  dynamic_header(literal_only, m, Code{ { 0 } });
  m.put(literal_only, 'a');
  m.put(literal_only, 256);
  Result const r{ run(literal_only.bytes) };
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.out == text("a"));

  BitWriter no_distance;  // a match, and no distance code to decode
  block_header(no_distance, 1, 2);
  dynamic_header(no_distance, m, Code{ { 0 } });
  m.put(no_distance, 'a');
  m.put(no_distance, 257);
  no_distance.put(0, 16);
  CHECK(run(no_distance.bytes).status == InflateStatus::BadDistanceSymbol);
}

TEST_CASE("inflate: a literal/length code without end-of-block is rejected") {
  std::vector<uint8_t> lit(257, 0);
  lit['a'] = 1;
  lit['b'] = 1;
  CHECK(run(dynamic_with(lit, { 1, 1 })).status == InflateStatus::NoEndOfBlock);
}

namespace {

// zlib 1.2.12 at level 9, raw: one dynamic block holding `reference_text()`.
constexpr std::array<scav_byte, 133> REFERENCE{
  0xB5, 0xD1, 0xB9, 0x11, 0xC4, 0x20, 0x0C, 0x00, 0xC0, 0x5A, 0x0D, 0xFA, 0x10, 0x20, 0x7E,
  0xBB, 0xFD, 0xAB, 0xE1, 0x66, 0x50, 0xBA, 0xE9, 0x3E, 0x01, 0x75, 0x44, 0x83, 0x55, 0x13,
  0x73, 0xAA, 0x0B, 0x2C, 0x0E, 0xC5, 0xF0, 0x5C, 0x63, 0x68, 0xB8, 0x4D, 0x45, 0xD4, 0x36,
  0x36, 0x98, 0x99, 0x62, 0x88, 0x94, 0xE7, 0x3D, 0xCE, 0x29, 0xE5, 0x76, 0xA8, 0xE3, 0x2A,
  0x0C, 0x11, 0xB8, 0x2C, 0xEC, 0x74, 0xDA, 0x3D, 0x7E, 0x79, 0xD0, 0xAE, 0x82, 0x80, 0x52,
  0x37, 0x0D, 0x7E, 0x7B, 0x51, 0x2D, 0xFD, 0x1E, 0x5B, 0x22, 0xA4, 0x64, 0x87, 0xA7, 0x7C,
  0xA3, 0xE6, 0x5C, 0xC7, 0x27, 0x93, 0xCF, 0x3D, 0x76, 0x2E, 0x8E, 0xE6, 0x5C, 0xAC, 0xE2,
  0x5C, 0x7C, 0xC8, 0xB9, 0xB8, 0x8A, 0x73, 0x71, 0x32, 0xE7, 0x62, 0x58, 0xCE, 0xC5, 0xA2,
  0xCE, 0xC5, 0x1D, 0x9D, 0x8B, 0x11, 0x9C, 0x8B, 0x0F, 0xFF, 0x95, 0xF6, 0x03,
};

Bytes reference_text() {
  Bytes t;
  for (uint32_t i = 0; i < 1500U; ++i) {
    t.push_back(static_cast<scav_byte>(((i * i) % 23U) + ((i / 97U) % 5U) + 97U));
  }
  return t;
}

void put_le32(Bytes &b, uint32_t v) {
  for (uint32_t i = 0; i < 4U; ++i) { b.push_back(static_cast<scav_byte>(v >> (8U * i))); }
}

// A gzip member around `deflated`, with the header's optional fields as asked.
Bytes gzip(Bytes const &deflated, Bytes const &plain, scav_byte flags) {
  Bytes g{ 0x1F, 0x8B, 8, flags, 0, 0, 0, 0, 2, 3 };
  if ((flags & 0x04U) != 0U) { g.insert(g.end(), { 3, 0, 'x', 'y', 'z' }); }
  if ((flags & 0x08U) != 0U) { g.insert(g.end(), { 'f', '.', 't', 't', 'f', 0 }); }
  if ((flags & 0x10U) != 0U) { g.insert(g.end(), { 'n', 'o', 't', 'e', 0 }); }
  if ((flags & 0x02U) != 0U) {
    uint32_t const h{ crc32(g.data(), static_cast<uint32_t>(g.size()), 0) };
    g.push_back(static_cast<scav_byte>(h));
    g.push_back(static_cast<scav_byte>(h >> 8U));
  }
  g.insert(g.end(), deflated.begin(), deflated.end());
  put_le32(g, crc32(plain.data(), static_cast<uint32_t>(plain.size()), 0));
  put_le32(g, static_cast<uint32_t>(plain.size()));
  return g;
}

Result run_gz(Bytes const &in, uint32_t cap = 1U << 16U) {
  Result r{ .status = InflateStatus::Ok, .out = Bytes(cap) };
  uint32_t n{ 0 };
  r.status = gunzip(in.data(), static_cast<uint32_t>(in.size()), r.out.data(), cap, n);
  CHECK(n <= cap);
  r.out.resize(n);
  return r;
}

Bytes reference() { return { REFERENCE.begin(), REFERENCE.end() }; }

}  // namespace

TEST_CASE("inflate: a zlib stream decodes to its text") {
  Result const r{ run(reference()) };
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.out == reference_text());
  CHECK(run(reference(), 1500).status == InflateStatus::Ok);
  CHECK(run(reference(), 1499).status == InflateStatus::OutputFull);
}

TEST_CASE("crc32: the check value, chaining, and the empty input") {
  std::string_view const nine{ "123456789" };
  auto const *const p{ reinterpret_cast<scav_byte const *>(nine.data()) };
  CHECK(crc32(p, 9, 0) == 0xCBF4'3926U);
  CHECK(crc32(p + 4, 5, crc32(p, 4, 0)) == 0xCBF4'3926U);
  CHECK(crc32(nullptr, 0, 0) == 0U);
}

TEST_CASE("gunzip: every optional header field is skipped, and the trailer checked") {
  Bytes const plain{ reference_text() };
  for (uint32_t flags = 0; flags < 32U; ++flags) {
    CAPTURE(flags);
    Result const r{ run_gz(gzip(reference(), plain, static_cast<scav_byte>(flags))) };
    CHECK(r.status == InflateStatus::Ok);
    CHECK(r.out == plain);
  }
  Bytes const g{ gzip(reference(), plain, 0) };
  uint32_t size{ 0 };
  CHECK(gzip_size(g.data(), static_cast<uint32_t>(g.size()), size));
  CHECK(size == 1500);
  CHECK(!gzip_size(g.data(), 17, size));
  CHECK(run_gz(g, 1500).status == InflateStatus::Ok);
  CHECK(run_gz(g, 1499).status == InflateStatus::OutputFull);
}

TEST_CASE("gunzip: a bad magic, method, flag or header CRC is rejected") {
  Bytes const plain{ reference_text() };
  Bytes const good{ gzip(reference(), plain, 0x02) };
  for (uint32_t const at : { 0U, 1U, 2U }) {
    Bytes g{ good };
    g[at] ^= 0x01U;
    CHECK(run_gz(g).status == InflateStatus::BadHeader);
  }
  for (uint32_t const bit : { 0x20U, 0x40U, 0x80U }) {
    Bytes g{ good };
    g[3] = static_cast<scav_byte>(g[3] | bit);
    CHECK(run_gz(g).status == InflateStatus::BadHeader);
  }
  Bytes g{ good };
  g[10] ^= 0x01U;  // the header CRC itself
  CHECK(run_gz(g).status == InflateStatus::BadHeader);
}

TEST_CASE("gunzip: a wrong CRC-32 or ISIZE, or bytes before the trailer, is rejected") {
  Bytes const plain{ reference_text() };
  Bytes const good{ gzip(reference(), plain, 0) };
  Bytes crc{ good };
  crc[crc.size() - 8U] ^= 0x01U;
  CHECK(run_gz(crc).status == InflateStatus::BadChecksum);
  Bytes size{ good };
  size[size.size() - 4U] ^= 0x01U;
  CHECK(run_gz(size).status == InflateStatus::BadSize);
  Bytes gap{ reference() };
  gap.push_back(0);
  CHECK(run_gz(gzip(gap, plain, 0)).status == InflateStatus::BadSize);
}

TEST_CASE("gunzip: a header cut inside any optional field is truncated") {
  Bytes const plain{ text("") };
  Bytes const empty{ fixed_literals("") };
  for (scav_byte const flags : { scav_byte{ 0x04 },
                                 scav_byte{ 0x08 },
                                 scav_byte{ 0x10 },
                                 scav_byte{ 0x02 },
                                 scav_byte{ 0x1E } }) {
    Bytes const g{ gzip(empty, plain, flags) };
    for (uint32_t cut = 0; cut < g.size(); ++cut) {
      Bytes const in{ g.begin(), g.begin() + cut };
      CAPTURE(flags);
      CAPTURE(cut);
      InflateStatus const st{ run_gz(in).status };
      CHECK(st != InflateStatus::Ok);
      if (cut < 18U) { CHECK(st == InflateStatus::Truncated); }
    }
  }
  Bytes big_extra{ 0x1F, 0x8B, 8, 0x04, 0, 0, 0, 0, 0, 3, 0xFF, 0xFF };
  big_extra.resize(40, 0);
  CHECK(run_gz(big_extra).status == InflateStatus::Truncated);
  Bytes short_extra{ 0x1F, 0x8B, 8, 0x04, 0, 0, 0, 0, 0, 3 };
  short_extra.resize(19, 0);  // one byte of XLEN before the trailer
  CHECK(run_gz(short_extra).status == InflateStatus::Truncated);
}

namespace {

// Every valid stream here, for the sweeps; the last one is gzip.
std::vector<std::pair<Bytes, Bytes>> sweep_streams() {
  Bytes const one_block_stored{ [] {
    BitWriter w;
    block_header(w, 1, 0);
    w.align();
    w.put(5, 16);
    w.put(0xFFFFU ^ 5U, 16);
    w.raw("hello");
    return w.bytes;
  }() };
  return {
    { reference(), reference_text() },
    { small_dynamic(), text("abcbcba") },
    { deep_dynamic(), Bytes{ 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 } },
    { fixed_literals("fixed huffman"), text("fixed huffman") },
    { one_block_stored, text("hello") },
    { gzip(reference(), reference_text(), 0x1E), reference_text() },
  };
}

// splitmix64, position-addressed, so a failure reproduces from the seed.
uint64_t rnd(uint64_t seed, uint64_t index) {
  uint64_t x{ seed + (index * 0x9E37'79B9'7F4A'7C15ULL) };
  x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31U);
}

}  // namespace

TEST_CASE(
    "inflate: every truncation of a valid stream is refused, with a correct prefix") {
  auto const streams{ sweep_streams() };
  for (size_t k = 0; k < streams.size(); ++k) {
    auto const &[stream, plain] = streams[k];
    bool const gz{ k + 1U == streams.size() };
    for (uint32_t cut = 0; cut < stream.size(); ++cut) {
      Bytes const in{ stream.begin(), stream.begin() + cut };
      Result const r{ gz ? run_gz(in) : run(in) };
      CAPTURE(k);
      CAPTURE(cut);
      CHECK(r.status != InflateStatus::Ok);
      REQUIRE(r.out.size() <= plain.size());
      CHECK(std::equal(r.out.begin(), r.out.end(), plain.begin()));
    }
  }
}

TEST_CASE("inflate: every single-bit flip of a valid stream stays in bounds") {
  auto const streams{ sweep_streams() };
  for (size_t k = 0; k < streams.size(); ++k) {
    auto const &[stream, plain] = streams[k];
    bool const gz{ k + 1U == streams.size() };
    uint32_t const cap{ static_cast<uint32_t>(plain.size()) + 64U };
    for (uint32_t bit = 0; bit < (8U * stream.size()); ++bit) {
      Bytes in{ stream };
      in[bit / 8U] = static_cast<scav_byte>(in[bit / 8U] ^ (1U << (bit % 8U)));
      Bytes out(cap + 16U, 0xA5);
      uint32_t n{ 0 };
      auto const len{ static_cast<uint32_t>(in.size()) };
      InflateStatus const st{ gz ? gunzip(in.data(), len, out.data(), cap, n)
                                 : inflate(in.data(), len, out.data(), cap, n) };
      CAPTURE(k);
      CAPTURE(bit);
      CHECK(n <= cap);
      for (uint32_t i = cap; i < out.size(); ++i) { REQUIRE(out[i] == 0xA5); }
      if (gz && (st == InflateStatus::Ok)) {  // only padding bits can flip unnoticed
        CHECK(std::equal(out.begin(), out.begin() + n, plain.begin(), plain.end()));
      }
    }
  }
}

TEST_CASE("inflate: a mutation sweep never reads or writes out of bounds") {
  auto const streams{ sweep_streams() };
  for (uint64_t key = 0; key < 20000U; ++key) {
    auto const &[stream, plain] = streams[key % streams.size()];
    Bytes in{ stream };
    uint32_t const rounds{ static_cast<uint32_t>(rnd(key, 0) % 4U) + 1U };
    for (uint32_t round = 0; round < rounds; ++round) {
      uint64_t const r{ rnd(key, round + 1U) };
      auto const at{ static_cast<uint32_t>((r >> 8U) % in.size()) };
      if ((r % 3U) == 0U) {
        in[at] = static_cast<scav_byte>(r >> 32U);
      } else if ((r % 3U) == 1U) {
        in.resize(at + 1U);
      } else {
        in.insert(in.begin() + at, static_cast<scav_byte>(r >> 40U));
      }
    }
    // Exact-size input and output buffers, so ASan sees any overrun.
    std::vector<scav_byte> const exact{ in };
    uint32_t const cap{ static_cast<uint32_t>(rnd(key, 99) % (plain.size() + 8U)) };
    std::vector<scav_byte> out(cap);
    uint32_t n{ 0 };
    auto const len{ static_cast<uint32_t>(exact.size()) };
    (void)inflate(exact.data(), len, out.data(), cap, n);
    CHECK(n <= cap);
    (void)gunzip(exact.data(), len, out.data(), cap, n);
    CHECK(n <= cap);
  }
}
