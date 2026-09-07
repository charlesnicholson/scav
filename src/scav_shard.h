#ifndef SCAV_SHARD_H_INCLUDED
#define SCAV_SHARD_H_INCLUDED

// Sharding derived from the model alone: how many work items a chart splits
// into, and which indices each one owns.

#include "scav/scav_types.h"
#include "scav_int.h"

#include <bit>
#include <cstdint>

namespace scav {

inline constexpr uint32_t ENTITIES_PER_SHARD{ 64 };
inline constexpr uint32_t MAX_SHARDS{ 256 };

constexpr uint32_t shard_count(uint32_t entity_count) {
  return imin(std::bit_ceil(ceil_div(entity_count, ENTITIES_PER_SHARD)), MAX_SHARDS);
}

// The first `items % shards` ranges take the extra index, so offsets stay
// contiguous and no two lengths differ by more than one. `shards` is nonzero.
constexpr scav_span shard_range(uint32_t shard, uint32_t shards, uint32_t items) {
  uint32_t const base{ items / shards };
  uint32_t const extra{ items % shards };
  return { .off = (base * shard) + imin(shard, extra),
           .len = base + ((shard < extra) ? 1U : 0U) };
}

}  // namespace scav

#endif  // SCAV_SHARD_H_INCLUDED
