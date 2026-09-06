#ifndef SCAV_THREAD_H_INCLUDED
#define SCAV_THREAD_H_INCLUDED

// The threading shim, one backend chosen at build time: pthreads, Win32, or
// null. Shards are the work items, assigned to workers before any of them runs.

#include <cstdint>
#include <type_traits>

namespace scav {

using ShardFn = void (*)(void *ctx, uint32_t shard);

// Runs fn(ctx, s) once for every s in [0, shards) and returns once the last has
// finished. `threads` <= 1 runs them all on the caller in index order.
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
  for (uint32_t shard{ first }; shard < shards; shard += workers) { fn(ctx, shard); }
}

}  // namespace scav

#endif  // SCAV_THREAD_H_INCLUDED
