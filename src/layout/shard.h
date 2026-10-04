#ifndef SCAV_LAYOUT_SHARD_H_INCLUDED
#define SCAV_LAYOUT_SHARD_H_INCLUDED

// The work-item count of a chart, shared by every layout phase that shards.

#include "scav/scav_core.h"
#include "scav_shard.h"

#include <cstdint>

namespace scav {

// Rows in the state, submachine and transition arrays, tombstones included.
inline uint32_t layout_entity_count(Chart const &c) {
  return static_cast<uint32_t>(c.states.size()) +
         static_cast<uint32_t>(c.submachines.size()) +
         static_cast<uint32_t>(c.transitions.size());
}

inline uint32_t layout_shard_count(Chart const &c) {
  return shard_count(layout_entity_count(c));
}

}  // namespace scav

#endif  // SCAV_LAYOUT_SHARD_H_INCLUDED
