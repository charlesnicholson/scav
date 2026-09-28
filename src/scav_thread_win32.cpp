#include "scav_thread.h"

#ifdef SCAV_TESTING
#  include "scav_rnd.h"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifdef SCAV_TESTING
#  include <atomic>
#endif

#include <cstdint>
#include <vector>

namespace scav {

#ifdef SCAV_TESTING
void thread_test_spawn_limit(uint32_t limit);
void thread_test_delay_seed(uint64_t seed);
#endif

namespace {

#ifdef SCAV_TESTING
// Atomic because a parked worker reads them while a test changes them between
// calls; relaxed, since neither orders anything but itself.
std::atomic<uint32_t> test_spawn_limit{ 0 };
std::atomic<uint64_t> test_delay_seed{ 0 };

// Both counts come from `rnd` at the shard's position, so one shard waits the
// same amount every run and only the interleaving of the workers moves.
void delay(uint32_t shard) {
  uint64_t const seed{ test_delay_seed.load(std::memory_order_relaxed) };
  if (seed == 0) { return; }
  uint64_t const yields{ rnd(seed, 0, shard, 0) % 64U };
  for (uint64_t i{ 0 }; i < yields; ++i) { SwitchToThread(); }
  uint64_t const spins{ rnd(seed, 1, shard, 0) % 4096U };
  uint32_t volatile sink{ 0 };
  for (uint64_t i{ 0 }; i < spins; ++i) { sink = sink + 1U; }
}
#endif

// One call's shards, on the stack of the thread that called: it returns only
// once `done` reaches `shards`, so no thread holds a job past its lifetime.
struct Job {
  ShardFn fn;
  void *ctx;
  uint32_t shards;
  uint32_t depth;      // one past the depth of the shard that pushed it
  uint32_t next{ 0 };  // the lowest shard nobody has claimed; under `mu`
  uint32_t done{ 0 };  // under `mu`
};

// One pool for the process, started on the first call that has work for it.
// Every thread, worker or caller, takes the next unclaimed shard of an open
// job, so a slow shard holds up only the thread running it.
struct Pool {
  SRWLOCK mu;
  CONDITION_VARIABLE cv;    // a job was pushed, or a job's last shard finished
  std::vector<Job *> open;  // jobs with a shard still unclaimed, oldest first
  uint32_t workers{ 0 };
};

Pool *g_pool{ nullptr };
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

// The depth of the job whose shard this thread is running, 0 outside one.
thread_local uint32_t t_depth{ 0 };

// The deepest open job at `floor` or deeper, the oldest of equals, so work a
// job has started finishes before a shallower job starts more of its own.
Job *pick(Pool &p, uint32_t floor) {
  Job *best{ nullptr };
  for (Job *const j : p.open) {
    if ((j->depth >= floor) && ((best == nullptr) || (j->depth > best->depth))) {
      best = j;
    }
  }
  return best;
}

// Claims `j`'s next shard and runs it with the lock released. The lock is
// held on entry and on return.
void run_next(Pool &p, Job &j) {
  uint32_t const shard{ j.next++ };
  if (j.next == j.shards) {
    for (uint32_t k = 0; k < p.open.size(); ++k) {
      if (p.open[k] == &j) {
        p.open.erase(p.open.begin() + k);
        break;
      }
    }
  }
  ReleaseSRWLockExclusive(&p.mu);
  uint32_t const was{ t_depth };
  t_depth = j.depth;
#ifdef SCAV_TESTING
  delay(shard);
#endif
  j.fn(j.ctx, shard);
  t_depth = was;
  AcquireSRWLockExclusive(&p.mu);
  if (++j.done == j.shards) { WakeAllConditionVariable(&p.cv); }
}

struct Seat {
  Pool *pool;
  uint32_t index;
};

DWORD WINAPI worker_main(LPVOID arg) {
  Seat const seat{ *static_cast<Seat *>(arg) };
  delete static_cast<Seat *>(arg);
  Pool &p{ *seat.pool };
  AcquireSRWLockExclusive(&p.mu);
  for (;;) {
    Job *const j{ pick(p, 0) };
#ifdef SCAV_TESTING
    uint32_t const limit{ test_spawn_limit.load(std::memory_order_relaxed) };
    bool const benched{ (limit != 0) && (seat.index >= limit) };
#else
    bool const benched{ false };
#endif
    if ((j == nullptr) || benched) {
      SleepConditionVariableSRW(&p.cv, &p.mu, INFINITE, 0);
      continue;
    }
    run_next(p, *j);
  }
}

// Never destroyed: a worker parks in `SleepConditionVariableSRW` for the life
// of the process, and a static destructor would tear the lock out from under
// it. A worker that will not start leaves the pool smaller, and a pool of none
// runs everything on the caller, so a spawn failure degrades and never fails.
BOOL CALLBACK start_pool(PINIT_ONCE /*once*/, PVOID /*param*/, PVOID * /*context*/) {
  Pool *const p{ new Pool };
  InitializeSRWLock(&p->mu);
  InitializeConditionVariable(&p->cv);
  uint32_t const want{ thread_concurrency() - 1U };
  for (uint32_t w{ 0 }; w < want; ++w) {
    auto *const seat{ new Seat{ .pool = p, .index = p->workers } };
    HANDLE const handle{ CreateThread(nullptr, 0, worker_main, seat, 0, nullptr) };
    if (handle != nullptr) {
      CloseHandle(handle);
      ++p->workers;
    } else {
      delete seat;
    }
  }
  g_pool = p;
  return TRUE;
}

}  // namespace

Mutex::Mutex() : impl(new SRWLOCK) { InitializeSRWLock(static_cast<SRWLOCK *>(impl)); }
Mutex::~Mutex() { delete static_cast<SRWLOCK *>(impl); }
void Mutex::lock() { AcquireSRWLockExclusive(static_cast<SRWLOCK *>(impl)); }
void Mutex::unlock() { ReleaseSRWLockExclusive(static_cast<SRWLOCK *>(impl)); }

uint32_t thread_concurrency() {
  DWORD const active{ GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) };
  return (active > 1) ? static_cast<uint32_t>(active) : 1U;
}

void parallel_for(uint32_t shards, uint32_t threads, ShardFn fn, void *ctx) {
  if ((shards <= 1U) || (threads == 1U)) {
    run_stripe(shards, 1U, 0U, fn, ctx);
    return;
  }
  InitOnceExecuteOnce(&g_once, start_pool, nullptr, nullptr);
  Pool &p{ *g_pool };
  if (p.workers == 0U) {
    run_stripe(shards, 1U, 0U, fn, ctx);
    return;
  }

  // The caller works its own job first, and then any job as deep as its own,
  // which is the work its job is waiting on or work no larger than it. A job
  // shallower than its own could be a whole search the caller would have to
  // finish before it noticed its own job was done.
  Job j{ .fn = fn, .ctx = ctx, .shards = shards, .depth = t_depth + 1U };
  AcquireSRWLockExclusive(&p.mu);
  p.open.push_back(&j);
  WakeAllConditionVariable(&p.cv);
  while (j.done != j.shards) {
    Job *const k{ (j.next < j.shards) ? &j : pick(p, j.depth) };
    if (k == nullptr) {
      SleepConditionVariableSRW(&p.cv, &p.mu, INFINITE, 0);
      continue;
    }
    run_next(p, *k);
  }
  ReleaseSRWLockExclusive(&p.mu);
}

#ifdef SCAV_TESTING
void thread_test_spawn_limit(uint32_t limit) {
  test_spawn_limit.store(limit, std::memory_order_relaxed);
}
void thread_test_delay_seed(uint64_t seed) {
  test_delay_seed.store(seed, std::memory_order_relaxed);
}
#endif

}  // namespace scav
