#include "scav_thread.h"

#include <atomic>
#include <cstdint>

namespace scav {

#ifdef SCAV_TESTING
void thread_test_spawn_limit(uint32_t limit);
void thread_test_delay_seed(uint64_t seed);
#endif

// A spin lock: the backend spawns no thread, but its host may call in from several.
Mutex::Mutex() : impl(new std::atomic_flag{}) {}
Mutex::~Mutex() { delete static_cast<std::atomic_flag *>(impl); }
void Mutex::lock() {
  auto *const flag{ static_cast<std::atomic_flag *>(impl) };
  while (flag->test_and_set(std::memory_order_acquire)) {}
}
void Mutex::unlock() {
  static_cast<std::atomic_flag *>(impl)->clear(std::memory_order_release);
}

uint32_t thread_concurrency() { return 1U; }

void parallel_for(uint32_t shards, uint32_t /*threads*/, ShardFn fn, void *ctx) {
  run_stripe(shards, 1U, 0U, fn, ctx);
}

#ifdef SCAV_TESTING
void thread_test_spawn_limit(uint32_t /*limit*/) {}
void thread_test_delay_seed(uint64_t /*seed*/) {}
#endif

}  // namespace scav
