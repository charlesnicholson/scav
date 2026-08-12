#include "scav/scav_core.h"

#include "core/model/bytes_view.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scav {

std::string_view string_pool_view(StringPool const &pool, StrRef ref) {
  if (ref.len == 0) { return {}; }
  return bytes_view(pool.bytes.data() + ref.off, ref.len);
}

}  // namespace scav
