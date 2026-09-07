#include "scav_thread.h"

#include <cstdint>

namespace scav {

#ifdef SCAV_TESTING
void thread_test_spawn_limit(uint32_t limit);
void thread_test_delay_seed(uint64_t seed);
#endif

void parallel_for(uint32_t shards, uint32_t /*threads*/, ShardFn fn, void *ctx) {
  run_stripe(shards, 1U, 0U, fn, ctx);
}

#ifdef SCAV_TESTING
void thread_test_spawn_limit(uint32_t /*limit*/) {}
void thread_test_delay_seed(uint64_t /*seed*/) {}
#endif

}  // namespace scav
