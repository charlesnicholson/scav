#include "scav_thread.h"

#ifdef SCAV_TESTING
#  include "scav_rnd.h"
#endif

#include <pthread.h>
#include <unistd.h>
#ifdef SCAV_TESTING
#  include <sched.h>

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
  for (uint64_t i{ 0 }; i < yields; ++i) { sched_yield(); }
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
  pthread_mutex_t mu;
  pthread_cond_t cv;        // a job was pushed, or a job's last shard finished
  std::vector<Job *> open;  // jobs with a shard still unclaimed, oldest first
  uint32_t workers{ 0 };
};

Pool *g_pool{ nullptr };
pthread_once_t g_once = PTHREAD_ONCE_INIT;

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
  pthread_mutex_unlock(&p.mu);
  uint32_t const was{ t_depth };
  t_depth = j.depth;
#ifdef SCAV_TESTING
  delay(shard);
#endif
  j.fn(j.ctx, shard);
  t_depth = was;
  pthread_mutex_lock(&p.mu);
  if (++j.done == j.shards) { pthread_cond_broadcast(&p.cv); }
}

struct Seat {
  Pool *pool;
  uint32_t index;
};

void *worker_main(void *arg) {
  Seat const seat{ *static_cast<Seat *>(arg) };
  delete static_cast<Seat *>(arg);
  Pool &p{ *seat.pool };
  pthread_mutex_lock(&p.mu);
  for (;;) {
    Job *const j{ pick(p, 0) };
#ifdef SCAV_TESTING
    uint32_t const limit{ test_spawn_limit.load(std::memory_order_relaxed) };
    bool const benched{ (limit != 0) && (seat.index >= limit) };
#else
    bool const benched{ false };
#endif
    if ((j == nullptr) || benched) {
      pthread_cond_wait(&p.cv, &p.mu);
      continue;
    }
    run_next(p, *j);
  }
}

// Never destroyed: a worker parks in `pthread_cond_wait` for the life of the
// process, and a static destructor would tear the condition out from under it.
// A worker that will not start leaves the pool smaller, and a pool of none
// runs everything on the caller, so a spawn failure degrades and never fails.
void start_pool() {
  Pool *const p{ new Pool };
  pthread_mutex_init(&p->mu, nullptr);
  pthread_cond_init(&p->cv, nullptr);
  uint32_t const want{ thread_concurrency() - 1U };
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  for (uint32_t w{ 0 }; w < want; ++w) {
    auto *const seat{ new Seat{ .pool = p, .index = p->workers } };
    pthread_t handle{};
    if (pthread_create(&handle, &attr, worker_main, seat) == 0) {
      ++p->workers;
    } else {
      delete seat;
    }
  }
  pthread_attr_destroy(&attr);
  g_pool = p;
}

}  // namespace

uint32_t thread_concurrency() {
  int64_t const online{ sysconf(_SC_NPROCESSORS_ONLN) };
  return (online > 1) ? static_cast<uint32_t>(online) : 1U;
}

void parallel_for(uint32_t shards, uint32_t threads, ShardFn fn, void *ctx) {
  if ((shards <= 1U) || (threads == 1U)) {
    run_stripe(shards, 1U, 0U, fn, ctx);
    return;
  }
  pthread_once(&g_once, start_pool);
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
  pthread_mutex_lock(&p.mu);
  p.open.push_back(&j);
  pthread_cond_broadcast(&p.cv);
  while (j.done != j.shards) {
    Job *const k{ (j.next < j.shards) ? &j : pick(p, j.depth) };
    if (k == nullptr) {
      pthread_cond_wait(&p.cv, &p.mu);
      continue;
    }
    run_next(p, *k);
  }
  pthread_mutex_unlock(&p.mu);
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
