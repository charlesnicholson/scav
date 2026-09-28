#ifndef SCAV_THREAD_H_INCLUDED
#define SCAV_THREAD_H_INCLUDED

// The threading shim, one backend chosen at build time: pthreads, Win32, or
// null. Shards are the work items, assigned to workers before any of them runs.

#include <cstdint>
#include <type_traits>

namespace scav {

using ShardFn = void (*)(void *ctx, uint32_t shard);

// How many workers this host can run at once, at least 1. The null backend
// says 1, because it is the backend that has no second thread to offer.
uint32_t thread_concurrency();

// Runs fn(ctx, s) once for every s in [0, shards) and returns once the last has
// finished. `threads` of 1 runs them all on the caller in index order, and 0 is
// `thread_concurrency()` -- safe as a default because the count never reaches
// any result (6), so it buys wall clock and nothing else.
void parallel_for(uint32_t shards, uint32_t threads, ShardFn fn, void *ctx);

// The same over a functor, erased to the overload above by a capture-free
// lambda that calls back through `ctx`; the body runs before this returns.
template <typename F>
void parallel_for(uint32_t shards, uint32_t threads, F &&fn) {
  using Fn = std::remove_reference_t<F>;
  parallel_for(
      shards,
      threads,
      [](void *ctx, uint32_t shard) { (*static_cast<Fn *>(ctx))(shard); },
      &fn);
}

// Worker `first` of `workers` takes shards first, first + workers, ...
inline void run_stripe(uint32_t shards,
                       uint32_t workers,
                       uint32_t first,
                       ShardFn fn,
                       void *ctx) {
  // 64-bit, so the index after the last stripe is representable near the top of
  // the domain instead of wrapping back into it.
  for (uint64_t shard{ first }; shard < shards; shard += workers) {
    fn(ctx, static_cast<uint32_t>(shard));
  }
}

}  // namespace scav

#endif  // SCAV_THREAD_H_INCLUDED
