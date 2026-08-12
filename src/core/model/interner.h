#ifndef SCAV_CORE_MODEL_INTERNER_H_INCLUDED
#define SCAV_CORE_MODEL_INTERNER_H_INCLUDED

// Builds a StringPool while parsing: identical bytes are stored once, and the
// StrRef a caller gets back is final -- it indexes the pool that ends up in the
// document. Private, because only the parser builds one and a client only reads.
//
// Dedup is all this does. It buys StrRef equality meaning string equality, which
// is worth the twenty lines; ordering the pool canonically is not here, because
// nothing yet depends on the pool's byte layout.

#include "core/model/lookup_map.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

struct Interner {
  StringPool pool;
  std::vector<StrRef> unique;       // pool spans, in first-encounter order
  StringLookupMap<uint32_t> index;  // bytes -> position in `unique`
};

// The empty string is StrRef{0, 0} and never enters the pool, so the pool does
// not depend on whether anything empty was interned.
StrRef intern_bytes(Interner &in, scav_byte const *bytes, size_t len);
StrRef intern_bytes(Interner &in, std::string_view text);

}  // namespace scav

#endif  // SCAV_CORE_MODEL_INTERNER_H_INCLUDED
