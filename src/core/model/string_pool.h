#ifndef SCAV_CORE_MODEL_STRING_POOL_H_INCLUDED
#define SCAV_CORE_MODEL_STRING_POOL_H_INCLUDED

// Two-pass interning (PRD 7).
//
// Pass one dedupes into a staging pool in first-encounter order, which is the
// only order a single forward parse can produce. Pass two re-emits the bytes
// sorted byte-wise and hands back a remap, because canonical ordering is by name
// bytes and never by interning order: two producers building the same model from
// differently-ordered sources must emit the same pool, or the format hash is a
// function of who typed what first.
//
// The remap is a separate step rather than a fixup inside the pool because the
// StrRefs live in the caller's payload arrays, and only the caller knows where
// they all are.

#include "core/model/ids.h"
#include "core/model/lookup_map.h"
#include "scav/types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

struct StringPool {
  std::vector<scav_byte> bytes;
};

struct Interner {
  std::vector<scav_byte> staging;
  std::vector<StrRef> unique;       // staging spans; offsets ascend with the index
  StringLookupMap<uint32_t> index;  // bytes -> position in `unique`
};

// The empty string is StrRef{0, 0} and never enters the pool: a zero length
// already says everything, and reserving an offset for it would make the pool
// depend on whether anything empty was ever interned.
StrRef intern(Interner &in, scav_byte const *bytes, uint32_t len);
StrRef intern(Interner &in, std::string_view text);

// Maps a staging offset to its offset in the finalized pool. Parallel arrays
// rather than a map: `staging` ascends, so this is a binary search and the
// lookup never allocates.
struct StringRemap {
  std::vector<uint32_t> staging;
  std::vector<uint32_t> finalized;
};

// Consumes nothing -- `in` stays valid, so a caller may finalize and still hold
// staging refs while it walks them through `remap`.
void finalize(Interner const &in, StringPool &pool, StringRemap &out);

// A zero-length ref maps to itself; anything else must have been interned.
StrRef remap(StringRemap const &r, StrRef ref);

std::string_view view(StringPool const &pool, StrRef ref);
std::string_view staged_view(Interner const &in, StrRef ref);

// Byte-wise, unsigned, shorter-prefix-first. PRD 6: collation is byte-wise only,
// so Hebrew and Arabic sort in codepoint order and that is an accepted trade.
int compare_bytes(scav_byte const *a, uint32_t alen, scav_byte const *b, uint32_t blen);

}  // namespace scav

#endif  // SCAV_CORE_MODEL_STRING_POOL_H_INCLUDED
