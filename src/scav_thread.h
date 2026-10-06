#ifndef SCAV_THREAD_H_INCLUDED
#define SCAV_THREAD_H_INCLUDED

// Threading shim with one backend chosen at build time: pthreads, Win32, or null.
// A shard writes only its own slot, so results are the same on any thread.

#include <cstdint>
#include <type_traits>

namespace scav {

using ShardFn = void (*)(void *ctx, uint32_t shard);

// How many workers this host can run at once, at least 1; 1 in the null backend.
uint32_t thread_concurrency();

// Runs fn(ctx, s) once for each s in [0, shards) and returns when all finish.
// `threads` 1 runs them on the caller in index order; others use the process-wide pool.
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

// Held only around lookups and inserts into state that shards share. Not
// recursive; the null backend uses a spin lock.
class Mutex {
 public:
  Mutex();
  ~Mutex();
  Mutex(Mutex const &) = delete;
  Mutex &operator=(Mutex const &) = delete;
  void lock();
  void unlock();

 private:
  friend class ConditionVariable;
  void *impl{ nullptr };  // the backend's lock
};

// Blocks a thread holding a Mutex until another notifies. `wait` may return with no
// notify, so a waiter re-checks its condition; the null backend spins until notified.
class ConditionVariable {
 public:
  ConditionVariable();
  ~ConditionVariable();
  ConditionVariable(ConditionVariable const &) = delete;
  ConditionVariable &operator=(ConditionVariable const &) = delete;
  void wait(Mutex &held);  // releases `held` while blocked and holds it again on return
  void notify_all();

 private:
  void *impl{ nullptr };  // the backend's condition variable
};

// A thread of its own running `fn(ctx)`; the destructor joins it.
class Thread {
 public:
  Thread() = default;
  ~Thread();
  Thread(Thread const &) = delete;
  Thread &operator=(Thread const &) = delete;
  bool start(void (*fn)(void *), void *ctx);  // false when none starts, as under null
  void join();                                // returns once `fn` has; no-op if unstarted

 private:
  void *impl{ nullptr };  // the backend's handle, null until started
};

// Holds a Mutex for the scope it is declared in.
class ScopedLock {
 public:
  explicit ScopedLock(Mutex &m) : held(m) { held.lock(); }
  ~ScopedLock() { held.unlock(); }
  ScopedLock(ScopedLock const &) = delete;
  ScopedLock &operator=(ScopedLock const &) = delete;

 private:
  Mutex &held;
};

// Worker `first` of `workers` takes shards first, first + workers, ...
inline void run_stripe(uint32_t shards,
                       uint32_t workers,
                       uint32_t first,
                       ShardFn fn,
                       void *ctx) {
  // 64-bit: the index after the last stripe may exceed UINT32_MAX.
  for (uint64_t shard{ first }; shard < shards; shard += workers) {
    fn(ctx, static_cast<uint32_t>(shard));
  }
}

}  // namespace scav

#endif  // SCAV_THREAD_H_INCLUDED
