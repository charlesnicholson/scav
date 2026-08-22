#ifndef SCAV_LAYOUT_WIRE_H_INCLUDED
#define SCAV_LAYOUT_WIRE_H_INCLUDED

// Little-endian field appends, shared by the digest and the layout hashes so
// a big-endian host serializes the same bytes.

#include "scav/scav_types.h"

#include <cstdint>
#include <vector>

namespace scav {

inline void append_u32(std::vector<scav_byte> &out, uint32_t v) {
  out.push_back(static_cast<scav_byte>(v & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 8U) & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 16U) & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 24U) & 0xFFU));
}

inline void append_i32(std::vector<scav_byte> &out, int32_t v) {
  append_u32(out, static_cast<uint32_t>(v));
}

}  // namespace scav

#endif  // SCAV_LAYOUT_WIRE_H_INCLUDED
