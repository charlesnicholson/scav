#ifndef SCAV_CORE_MODEL_BYTES_VIEW_H_INCLUDED
#define SCAV_CORE_MODEL_BYTES_VIEW_H_INCLUDED

// One cast, in one place. char may alias any object representation, which is
// what this relies on; the other direction would not be safe.

#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>

namespace scav {

inline std::string_view bytes_view(scav_byte const *bytes, uint32_t len) {
  return { reinterpret_cast<char const *>(bytes), len };
}

}  // namespace scav

#endif  // SCAV_CORE_MODEL_BYTES_VIEW_H_INCLUDED
