#include "core/model/interner.h"

#include "core/model/bytes_view.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace scav {

StrRef intern_bytes(Interner &in, scav_byte const *bytes, size_t len) {
  if (len == 0) { return {}; }

  std::string_view const probe{ bytes_view(bytes, len) };
  if (uint32_t const *const found = in.index.find(probe); found != nullptr) {
    return in.unique[*found];
  }

  // The parser rejected anything a span cannot address, so both round-trip.
  StrRef const ref{ str_ref(narrow_clamp<uint32_t>(in.pool.bytes.size()),
                            narrow_clamp<uint32_t>(len)) };
  in.pool.bytes.insert(in.pool.bytes.end(), bytes, bytes + len);
  in.index.insert(std::string{ probe }, narrow_clamp<uint32_t>(in.unique.size()));
  in.unique.push_back(ref);
  return ref;
}

StrRef intern_bytes(Interner &in, std::string_view text) {
  return intern_bytes(in, reinterpret_cast<scav_byte const *>(text.data()), text.size());
}

}  // namespace scav
