#ifndef SCAV_STRING_POOL_H_INCLUDED
#define SCAV_STRING_POOL_H_INCLUDED

// Everything here is named `string_*`, after the header.
//
// The finalized string pool and the two ways to read one.
//
// A StrRef is an offset and a length into StringPool::bytes. The pool is not
// NUL-terminated and never will be: PRD 16 hands strings out as spans, so a
// caller that wants a C string makes one itself.
//
// How a pool is *built* is not here -- see model/interner.h, which is private
// because interning is a parse-time concern and a client only ever reads.

#include "scav/scav_ids.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

struct StringPool {
  std::vector<scav_byte> bytes;
};

// A zero-length ref reads as the empty string without touching `pool`.
std::string_view string_pool_view(StringPool const &pool, StrRef ref);

// Byte-wise, unsigned, shorter-prefix-first. PRD 6: collation is byte-wise only,
// so Hebrew and Arabic sort in codepoint order and that is an accepted trade.
// Exposed because it is the order the pool is finalized in, and anything
// comparing names has to agree with it.
int string_compare_bytes(scav_byte const *a,
                         uint32_t alen,
                         scav_byte const *b,
                         uint32_t blen);

}  // namespace scav

#endif  // SCAV_STRING_POOL_H_INCLUDED
