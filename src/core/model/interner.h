#ifndef SCAV_CORE_MODEL_INTERNER_H_INCLUDED
#define SCAV_CORE_MODEL_INTERNER_H_INCLUDED

// Two-pass interning: pass one dedupes in encounter order, pass two re-emits
// sorted by bytes so two producers of the same model emit the same pool. The
// remap is separate because the StrRefs live in the caller's arrays.

#include "core/model/lookup_map.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

struct Interner {
  std::vector<scav_byte> staging;
  std::vector<StrRef> unique;       // staging spans; offsets ascend with the index
  StringLookupMap<uint32_t> index;  // bytes -> position in `unique`
};

// The empty string is StrRef{0, 0} and never enters the pool, so the pool does
// not depend on whether anything empty was interned.
StrRef intern_bytes(Interner &in, scav_byte const *bytes, size_t len);
StrRef intern_bytes(Interner &in, std::string_view text);

// Staging offset -> finalized offset. Parallel arrays, not a map: `staging`
// ascends, so the lookup is a binary search and never allocates.
struct StringRemap {
  std::vector<uint32_t> staging;
  std::vector<uint32_t> finalized;
};

// `in` stays valid, so a caller may finalize and still walk its staging refs.
void intern_finalize(Interner const &in, StringPool &pool, StringRemap &out);

// A zero-length ref maps to itself; anything else must have been interned.
StrRef intern_remap(StringRemap const &r, StrRef ref);

std::string_view intern_staged_view(Interner const &in, StrRef ref);

}  // namespace scav

#endif  // SCAV_CORE_MODEL_INTERNER_H_INCLUDED
