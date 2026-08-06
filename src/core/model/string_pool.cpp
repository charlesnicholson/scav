#include "scav/scav_string_pool.h"

#include "core/model/bytes_view.h"
#include "scav/scav_ids.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>

namespace scav {

int string_compare_bytes(scav_byte const *a,
                         uint32_t alen,
                         scav_byte const *b,
                         uint32_t blen) {
  uint32_t const n{ (alen < blen) ? alen : blen };
  for (uint32_t i = 0; i < n; ++i) {
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
