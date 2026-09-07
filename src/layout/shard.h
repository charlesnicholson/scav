#ifndef SCAV_LAYOUT_SHARD_H_INCLUDED
#define SCAV_LAYOUT_SHARD_H_INCLUDED

// How many work items a chart splits into, shared by the phases that shard so
// the two cannot disagree about it.

#include "scav/scav_core.h"
#include "scav_shard.h"

#include <cstdint>

namespace scav {

// Every row of the three entity arrays, tombstones included, so the count is a
// function of the model's shape and not of what a phase decided to skip (6).
inline uint32_t layout_shard_count(Chart const &c) {
  return shard_count(static_cast<uint32_t>(c.states.size()) +
                     static_cast<uint32_t>(c.submachines.size()) +
                     static_cast<uint32_t>(c.transitions.size()));
}

}  // namespace scav

#endif  // SCAV_LAYOUT_SHARD_H_INCLUDED
