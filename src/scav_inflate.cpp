// Canonical Huffman decoding: a 9-bit table resolves short codes in one step,
// and longer ones walk the per-length counts a bit at a time.

#include "scav_inflate.h"

#include "scav/scav_types.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace scav {

namespace {

constexpr uint32_t MAX_BITS{ 15 };
constexpr uint32_t FAST_BITS{ 9 };
constexpr uint32_t FAST_MASK{ (1U << FAST_BITS) - 1U };
constexpr uint32_t MAX_LITLEN{ 286 };  // symbols a dynamic header may define
constexpr uint32_t MAX_DIST{ 30 };
constexpr uint32_t FIXED_LITLEN{ 288 };  // the fixed code also assigns 286 and 287
constexpr uint32_t FIXED_DIST{ 32 };     // and distance symbols 30 and 31

// Symbols 257..285, then distance symbols 0..29: a base plus that many extra bits.
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
// The order a dynamic header lists the code-length code's lengths in.
constexpr std::array<uint8_t, 19> CLEN_ORDER{ 16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                              11, 4,  12, 3, 13, 2, 14, 1, 15 };

constexpr std::array<uint32_t, 256> CRC_TABLE{ [] {
  std::array<uint32_t, 256> t{};
  for (uint32_t n = 0; n < 256U; ++n) {
    uint32_t c{ n };
    for (uint32_t k = 0; k < 8U; ++k) {
      c = ((c & 1U) != 0U) ? (0xEDB8'8320U ^ (c >> 1U)) : (c >> 1U);
    }
    t[n] = c;
  }
  return t;
}() };

// LSB-first bit reader. Bits of `buf` at and above `count` are zero, so a peek
// past the end of the input reads zeros.
struct Bits {
  scav_byte const *in;
  uint32_t len;
  uint32_t pos;    // next byte to load
  uint64_t buf;    // unconsumed bits, the next one lowest
  uint32_t count;  // how many of `buf` are real
};

void refill(Bits &b) {
  while ((b.count <= 56U) && (b.pos < b.len)) {
    b.buf |= static_cast<uint64_t>(b.in[b.pos]) << b.count;
    ++b.pos;
    b.count += 8U;
  }
}

void drop(Bits &b, uint32_t n) {
  b.buf >>= n;
  b.count -= n;
}

// Up to 16 bits, false if the input holds fewer.
bool take(Bits &b, uint32_t n, uint32_t &out) {
  if (b.count < n) {
    refill(b);
    if (b.count < n) { return false; }
  }
  out = static_cast<uint32_t>(b.buf & ((uint64_t{ 1 } << n) - 1U));
  drop(b, n);
  return true;
}

// `fast` holds (length << 9) | symbol for every code of up to 9 bits, indexed
// by the next 9 input bits; 0 sends the decode to the per-length walk.
struct Huffman {
  std::array<uint16_t, 1U << FAST_BITS> fast;
  std::array<uint16_t, MAX_BITS + 1U>
      count;  // codes of each length; [0] counts unused symbols
  std::array<uint16_t, FIXED_LITLEN> symbol;  // symbols in canonical order
};

// `strict` requires a complete code; otherwise one 1-bit code, or none, may leave a gap.
InflateStatus build(Huffman &h, uint8_t const *lengths, uint32_t n, bool strict) {
  h.count.fill(0);
  for (uint32_t s = 0; s < n; ++s) { ++h.count[lengths[s]]; }

  int32_t left{ 1 };  // unassigned codes at the current length
  for (uint32_t len = 1; len <= MAX_BITS; ++len) {
    left = (left * 2) - h.count[len];
    if (left < 0) { return InflateStatus::OverSubscribed; }
  }
  bool const lone{ (n - h.count[0]) == h.count[1] };
  if ((left != 0) && (strict || !lone)) { return InflateStatus::Incomplete; }

  std::array<uint16_t, MAX_BITS + 1U> next{};
  for (uint32_t len = 1; len < MAX_BITS; ++len) {
    next[len + 1U] = static_cast<uint16_t>(next[len] + h.count[len]);
  }
  for (uint32_t s = 0; s < n; ++s) {
    if (lengths[s] != 0U) { h.symbol[next[lengths[s]]++] = static_cast<uint16_t>(s); }
  }

  h.fast.fill(0);
  uint32_t code{ 0 };
  uint32_t index{ 0 };
  for (uint32_t len = 1; len <= FAST_BITS; ++len) {
    for (uint32_t k = 0; k < h.count[len]; ++k) {
      uint32_t rev{ 0 };
      for (uint32_t i = 0; i < len; ++i) { rev |= ((code >> i) & 1U) << (len - 1U - i); }
      auto const entry{ static_cast<uint16_t>((len << FAST_BITS) | h.symbol[index]) };
      for (uint32_t at = rev; at <= FAST_MASK; at += 1U << len) { h.fast[at] = entry; }
      ++code;
      ++index;
    }
    code <<= 1U;
  }
  return InflateStatus::Ok;
}

// `bad` is the status for a bit pattern the code does not assign.
InflateStatus decode(Bits &b, Huffman const &h, InflateStatus bad, uint32_t &sym) {
  if (b.count < MAX_BITS) { refill(b); }
  uint32_t const entry{ h.fast[b.buf & FAST_MASK] };
  if (entry != 0U) {
    uint32_t const len{ entry >> FAST_BITS };
    if (len > b.count) { return InflateStatus::Truncated; }
    drop(b, len);
    sym = entry & FAST_MASK;
    return InflateStatus::Ok;
  }
  int32_t code{ 0 };   // the bits so far, first one highest
  int32_t first{ 0 };  // the first code of this length
  int32_t index{ 0 };  // that code's position in `symbol`
  for (uint32_t len = 1; len <= MAX_BITS; ++len) {
    if (len > b.count) { return InflateStatus::Truncated; }
    code |= static_cast<int32_t>((b.buf >> (len - 1U)) & 1U);
    int32_t const count{ h.count[len] };
    if ((code - first) < count) {
      drop(b, len);
      sym = h.symbol[static_cast<uint32_t>(index + (code - first))];
      return InflateStatus::Ok;
    }
    index += count;
    first = (first + count) * 2;
    code *= 2;
  }
  return bad;
}

InflateStatus stored(Bits &b, scav_byte *out, uint32_t cap, uint32_t &out_len) {
  drop(b, b.count % 8U);
  uint32_t n{ 0 };
  uint32_t check{ 0 };
  if (!take(b, 16, n) || !take(b, 16, check)) { return InflateStatus::Truncated; }
  if ((n ^ 0xFFFFU) != check) { return InflateStatus::BadStoredLength; }
  b.pos -= b.count / 8U;  // hand back the whole bytes the buffer still holds
  b.buf = 0;
  b.count = 0;
  if (n > (b.len - b.pos)) { return InflateStatus::Truncated; }
  if (n > (cap - out_len)) { return InflateStatus::OutputFull; }
  std::copy_n(b.in + b.pos, n, out + out_len);
  b.pos += n;
  out_len += n;
  return InflateStatus::Ok;
}

InflateStatus codes(Bits &b,
                    Huffman const &lit,
                    Huffman const &dist,
                    scav_byte *out,
                    uint32_t cap,
                    uint32_t &out_len) {
  for (;;) {
    uint32_t sym{ 0 };
    InflateStatus st{ decode(b, lit, InflateStatus::BadLengthSymbol, sym) };
    if (st != InflateStatus::Ok) { return st; }
    if (sym < 256U) {
      if (out_len == cap) { return InflateStatus::OutputFull; }
      out[out_len++] = static_cast<scav_byte>(sym);
      continue;
    }
    if (sym == 256U) { return InflateStatus::Ok; }
    sym -= 257U;
    if (sym >= LEN_BASE.size()) { return InflateStatus::BadLengthSymbol; }
    uint32_t extra{ 0 };
    if (!take(b, LEN_EXTRA[sym], extra)) { return InflateStatus::Truncated; }
    uint32_t const length{ LEN_BASE[sym] + extra };

    st = decode(b, dist, InflateStatus::BadDistanceSymbol, sym);
    if (st != InflateStatus::Ok) { return st; }
    if (sym >= DIST_BASE.size()) { return InflateStatus::BadDistanceSymbol; }
    if (!take(b, DIST_EXTRA[sym], extra)) { return InflateStatus::Truncated; }
    uint32_t const back{ DIST_BASE[sym] + extra };

    if (back > out_len) { return InflateStatus::BadDistance; }
    if (length > (cap - out_len)) { return InflateStatus::OutputFull; }
    scav_byte *const to{ out + out_len };
    scav_byte const *const from{ to - back };
    if (back >= length) {
      std::copy_n(from, length, to);
    } else {  // overlapping, so a byte written here may be read again
      for (uint32_t i = 0; i < length; ++i) { to[i] = from[i]; }
    }
    out_len += length;
  }
}

InflateStatus fixed(Bits &b, scav_byte *out, uint32_t cap, uint32_t &out_len) {
  std::array<uint8_t, FIXED_LITLEN> lengths{};
  std::fill_n(lengths.begin(), 144, uint8_t{ 8 });
  std::fill_n(lengths.begin() + 144, 112, uint8_t{ 9 });
  std::fill_n(lengths.begin() + 256, 24, uint8_t{ 7 });
  std::fill_n(lengths.begin() + 280, 8, uint8_t{ 8 });
  Huffman lit{};
  Huffman dist{};
  (void)build(lit, lengths.data(), FIXED_LITLEN, true);
  lengths.fill(5);
  (void)build(dist, lengths.data(), FIXED_DIST, true);
  return codes(b, lit, dist, out, cap, out_len);
}

InflateStatus dynamic(Bits &b, scav_byte *out, uint32_t cap, uint32_t &out_len) {
  uint32_t nlit{ 0 };
  uint32_t ndist{ 0 };
  uint32_t nclen{ 0 };
  if (!take(b, 5, nlit) || !take(b, 5, ndist) || !take(b, 4, nclen)) {
    return InflateStatus::Truncated;
  }
  nlit += 257U;
  ndist += 1U;
  nclen += 4U;
  if ((nlit > MAX_LITLEN) || (ndist > MAX_DIST)) { return InflateStatus::BadCodeCounts; }

  std::array<uint8_t, MAX_LITLEN + MAX_DIST> lengths{};
  for (uint32_t i = 0; i < nclen; ++i) {
    uint32_t len{ 0 };
    if (!take(b, 3, len)) { return InflateStatus::Truncated; }
    lengths[CLEN_ORDER[i]] = static_cast<uint8_t>(len);
  }
  Huffman lit{};
  InflateStatus st{ build(lit, lengths.data(), 19U, true) };
  if (st != InflateStatus::Ok) { return st; }

  lengths.fill(0);
  uint32_t const total{ nlit + ndist };
  uint32_t i{ 0 };
  while (i < total) {
    uint32_t sym{ 0 };
    st = decode(b,
                lit,
                InflateStatus::Incomplete,
                sym);  // a complete code assigns every pattern
    if (st != InflateStatus::Ok) { return st; }
    if (sym < 16U) {
      lengths[i++] = static_cast<uint8_t>(sym);
      continue;
    }
    uint8_t value{ 0 };
    uint32_t repeat{ 0 };
    bool ok{ false };
    if (sym == 16U) {
      if (i == 0U) { return InflateStatus::BadRepeat; }
      value = lengths[i - 1U];
      ok = take(b, 2, repeat);
      repeat += 3U;
    } else if (sym == 17U) {
      ok = take(b, 3, repeat);
      repeat += 3U;
    } else {
      ok = take(b, 7, repeat);
      repeat += 11U;
    }
    if (!ok) { return InflateStatus::Truncated; }
    if (repeat > (total - i)) { return InflateStatus::BadRepeat; }
    std::fill_n(lengths.begin() + i, repeat, value);
    i += repeat;
  }
  if (lengths[256] == 0U) { return InflateStatus::NoEndOfBlock; }

  Huffman dist{};
  st = build(lit, lengths.data(), nlit, false);
  if (st != InflateStatus::Ok) { return st; }
  st = build(dist, lengths.data() + nlit, ndist, false);
  if (st != InflateStatus::Ok) { return st; }
  return codes(b, lit, dist, out, cap, out_len);
}

InflateStatus blocks(Bits &b, scav_byte *out, uint32_t cap, uint32_t &out_len) {
  out_len = 0;
  uint32_t last{ 0 };
  while (last == 0U) {
    uint32_t type{ 0 };
    if (!take(b, 1, last) || !take(b, 2, type)) { return InflateStatus::Truncated; }
    InflateStatus st{ InflateStatus::BadBlockType };
    if (type == 0U) {
      st = stored(b, out, cap, out_len);
    } else if (type == 1U) {
      st = fixed(b, out, cap, out_len);
    } else if (type == 2U) {
      st = dynamic(b, out, cap, out_len);
    }
    if (st != InflateStatus::Ok) { return st; }
  }
  return InflateStatus::Ok;
}

uint32_t le32(scav_byte const *at) {
  return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8U) |
         (static_cast<uint32_t>(at[2]) << 16U) | (static_cast<uint32_t>(at[3]) << 24U);
}

// Advances `pos` past a zero-terminated header field; false if `end` comes first.
bool skip_string(scav_byte const *in, uint32_t end, uint32_t &pos) {
  while (pos < end) {
    if (in[pos++] == 0U) { return true; }
  }
  return false;
}

}  // namespace

InflateStatus inflate(scav_byte const *in,
                      uint32_t len,
                      scav_byte *out,
                      uint32_t cap,
                      uint32_t &out_len) {
  Bits b{ .in = in, .len = len, .pos = 0, .buf = 0, .count = 0 };
  return blocks(b, out, cap, out_len);
}

InflateStatus gunzip(scav_byte const *in,
                     uint32_t len,
                     scav_byte *out,
                     uint32_t cap,
                     uint32_t &out_len) {
  out_len = 0;
  if (len < 18U) { return InflateStatus::Truncated; }
  if ((in[0] != 0x1FU) || (in[1] != 0x8BU) || (in[2] != 8U) || ((in[3] & 0xE0U) != 0U)) {
    return InflateStatus::BadHeader;
  }
  uint32_t const flags{ in[3] };
  uint32_t const end{ len - 8U };  // the trailer
  uint32_t pos{ 10 };
  if ((flags & 0x04U) != 0U) {  // FEXTRA
    if ((end - pos) < 2U) { return InflateStatus::Truncated; }
    uint32_t const extra{ in[pos] | (static_cast<uint32_t>(in[pos + 1U]) << 8U) };
    if ((end - pos - 2U) < extra) { return InflateStatus::Truncated; }
    pos += 2U + extra;
  }
  if (((flags & 0x08U) != 0U) && !skip_string(in, end, pos)) {  // FNAME
    return InflateStatus::Truncated;
  }
  if (((flags & 0x10U) != 0U) && !skip_string(in, end, pos)) {  // FCOMMENT
    return InflateStatus::Truncated;
  }
  if ((flags & 0x02U) != 0U) {  // FHCRC, the low half of the header's CRC-32
    if ((end - pos) < 2U) { return InflateStatus::Truncated; }
    uint32_t const want{ in[pos] | (static_cast<uint32_t>(in[pos + 1U]) << 8U) };
    if ((crc32(in, pos, 0) & 0xFFFFU) != want) { return InflateStatus::BadHeader; }
    pos += 2U;
  }

  Bits b{ .in = in, .len = end, .pos = pos, .buf = 0, .count = 0 };
  InflateStatus const st{ blocks(b, out, cap, out_len) };
  if (st != InflateStatus::Ok) { return st; }
  if (crc32(out, out_len, 0) != le32(in + end)) { return InflateStatus::BadChecksum; }
  if ((le32(in + end + 4U) != out_len) || ((b.pos - (b.count / 8U)) != end)) {
    return InflateStatus::BadSize;
  }
  return InflateStatus::Ok;
}

bool gzip_size(scav_byte const *in, uint32_t len, uint32_t &size) {
  if (len < 18U) { return false; }
  size = le32(in + len - 4U);
  return true;
}

uint32_t crc32(scav_byte const *bytes, uint32_t len, uint32_t crc) {
  crc = ~crc;
  for (uint32_t i = 0; i < len; ++i) {
    crc = CRC_TABLE[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8U);
  }
  return ~crc;
}

}  // namespace scav
