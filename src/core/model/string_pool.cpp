#include "scav/scav_core.h"

#include "core/model/bytes_view.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scav {

int string_compare_bytes(scav_byte const *a,
                         size_t alen,
                         scav_byte const *b,
                         size_t blen) {
  size_t const n{ (alen < blen) ? alen : blen };
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) { return (a[i] < b[i]) ? -1 : 1; }
  }
  if (alen == blen) { return 0; }
  return (alen < blen) ? -1 : 1;
}

std::string_view string_pool_view(StringPool const &pool, StrRef ref) {
  if (ref.len == 0) { return {}; }
  return bytes_view(pool.bytes.data() + ref.off, ref.len);
}

}  // namespace scav
