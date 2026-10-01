#ifndef SCAV_INFLATE_H_INCLUDED
#define SCAV_INFLATE_H_INCLUDED

// DEFLATE (RFC 1951) and its gzip wrapper (RFC 1952), decode only. Never reads
// past `len` input bytes or writes past `cap` output bytes; state is on the stack.

#include "scav/scav_types.h"

#include <cstdint>

namespace scav {

enum class InflateStatus : uint32_t {
  Ok,
  Truncated,          // the input ends inside the stream
  OutputFull,         // the output would pass `cap`
  BadBlockType,       // BTYPE 3
  BadStoredLength,    // NLEN is not the complement of LEN
  BadCodeCounts,      // HLIT above 286 or HDIST above 30
  OverSubscribed,     // more codes of some length than the tree has room for
  Incomplete,         // a code with unused leaves, other than one 1-bit code
  BadRepeat,          // code 16 with no previous length, or a repeat past the end
  NoEndOfBlock,       // the literal/length code omits symbol 256
  BadLengthSymbol,    // an unassigned literal/length code, or symbol 286 or 287
  BadDistanceSymbol,  // an unassigned distance code, or symbol 30 or 31
  BadDistance,        // a distance farther back than the output so far
  BadHeader,          // gzip magic, method, reserved flags, or header CRC
  BadChecksum,        // gzip CRC-32 disagrees with the output
  BadSize,            // gzip ISIZE disagrees, or the stream stops short of the trailer
};

// Raw DEFLATE into `out[0, cap)`; `out_len` is the count written, even on error.
InflateStatus inflate(scav_byte const *in,
                      uint32_t len,
                      scav_byte *out,
                      uint32_t cap,
                      uint32_t &out_len);

// One gzip member spanning all of `in`, its CRC-32 and ISIZE checked.
InflateStatus gunzip(scav_byte const *in,
                     uint32_t len,
                     scav_byte *out,
                     uint32_t cap,
                     uint32_t &out_len);

// The trailer's ISIZE, which sizes the buffer `gunzip` fills. False under 18 bytes.
bool gzip_size(scav_byte const *in, uint32_t len, uint32_t &size);

// CRC-32 as gzip and zlib compute it; chain calls by passing the last result.
uint32_t crc32(scav_byte const *bytes, uint32_t len, uint32_t crc);

}  // namespace scav

#endif  // SCAV_INFLATE_H_INCLUDED
