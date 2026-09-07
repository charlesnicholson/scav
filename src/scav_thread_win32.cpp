#include "scav_thread.h"

#include "scav_int.h"

#ifdef SCAV_TESTING
#  include "scav_rnd.h"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <vector>

namespace scav {

#ifdef SCAV_TESTING
void thread_test_spawn_limit(uint32_t limit);
void thread_test_delay_seed(uint64_t seed);
#endif

namespace {

#ifdef SCAV_TESTING
uint32_t test_spawn_limit{ 0 };
uint64_t test_delay_seed{ 0 };

// Both counts come from `rnd` at the shard's position, so one shard waits the
// same amount every run and only the interleaving of the workers moves.
void delay(uint32_t shard) {
  if (test_delay_seed == 0) { return; }
  uint64_t const yields{ rnd(test_delay_seed, 0, shard, 0) % 64U };
  for (uint64_t i{ 0 }; i < yields; ++i) { SwitchToThread(); }
  uint64_t const spins{ rnd(test_delay_seed, 1, shard, 0) % 4096U };
  uint32_t volatile sink{ 0 };
  for (uint64_t i{ 0 }; i < spins; ++i) { sink = sink + 1U; }
}
#endif

struct Worker {
  ShardFn fn;
  void *ctx;
  uint32_t shards;
  uint32_t workers;
  uint32_t first;
};

void run_worker(Worker const &worker) {
#ifdef SCAV_TESTING
  for (uint64_t shard{ worker.first }; shard < worker.shards; shard += worker.workers) {
    delay(static_cast<uint32_t>(shard));
    worker.fn(worker.ctx, static_cast<uint32_t>(shard));
  }
#else
  run_stripe(worker.shards, worker.workers, worker.first, worker.fn, worker.ctx);
#endif
}

DWORD WINAPI worker_main(LPVOID arg) {
  run_worker(*static_cast<Worker const *>(arg));
  return 0;
}

}  // namespace

void parallel_for(uint32_t shards, uint32_t threads, ShardFn fn, void *ctx) {
  uint32_t const requested{ (threads == 0U) ? 1U : threads };
  uint32_t const workers{ imin(requested, shards) };
  if (workers <= 1U) {
    run_stripe(shards, 1U, 0U, fn, ctx);
    return;
  }

  std::vector<Worker> plan(workers);
  for (uint32_t w{ 0 }; w < workers; ++w) {
    plan[w] =
        Worker{ .fn = fn, .ctx = ctx, .shards = shards, .workers = workers, .first = w };
  }

  // A spawned worker reads its own `plan` entry and nothing else; both vectors
  // below stay on the calling thread.
  std::vector<HANDLE> handles(workers, nullptr);
  std::vector<uint8_t> spawned(workers, 0);
  for (uint32_t w{ 1 }; w < workers; ++w) {
#ifdef SCAV_TESTING
    if ((test_spawn_limit != 0) && (w > test_spawn_limit)) { continue; }
#endif
    handles[w] = CreateThread(nullptr, 0, worker_main, &plan[w], 0, nullptr);
    if (handles[w] != nullptr) { spawned[w] = 1; }
  }

  run_worker(plan[0]);
  // A worker that failed to spawn runs on the caller, over the stripe it was
  // already assigned.
  for (uint32_t w{ 1 }; w < workers; ++w) {
    if (spawned[w] == 0) { run_worker(plan[w]); }
  }
  for (uint32_t w{ 1 }; w < workers; ++w) {
    if (spawned[w] != 0) {
      WaitForSingleObject(handles[w], INFINITE);
      CloseHandle(handles[w]);
    }
  }
}

#ifdef SCAV_TESTING
void thread_test_spawn_limit(uint32_t limit) { test_spawn_limit = limit; }
void thread_test_delay_seed(uint64_t seed) { test_delay_seed = seed; }
#endif

}  // namespace scav
