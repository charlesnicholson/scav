#ifndef SCAV_LAYOUT_WIRE_H_INCLUDED
#define SCAV_LAYOUT_WIRE_H_INCLUDED

// Little-endian field appends for the spaces digest and the layout hashes.

#include "scav/scav_types.h"
#include "scav_vector.h"

#include <cstdint>

namespace scav {

inline void append_u32(Vector<scav_byte> &out, uint32_t v) {
  out.push_back(static_cast<scav_byte>(v & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 8U) & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 16U) & 0xFFU));
  out.push_back(static_cast<scav_byte>((v >> 24U) & 0xFFU));
}

inline void append_i32(Vector<scav_byte> &out, int32_t v) {
  append_u32(out, static_cast<uint32_t>(v));
}

}  // namespace scav

#endif  // SCAV_LAYOUT_WIRE_H_INCLUDED
