// Each shard runs exactly once, and a reduction merged in index order is one
// value at every thread count. A shard writes only its own slot.

#include "scav_thread.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"
#include "scav_xxhash.h"

#include "doctest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace scav {
void thread_test_spawn_limit(uint32_t limit);
void thread_test_delay_seed(uint64_t seed);
uint32_t thread_test_participants();  // threads that ran a pooled shard since last asked
}  // namespace scav

namespace {

using namespace scav;

constexpr std::array<uint32_t, 9> THREADS{ 0, 1, 2, 3, 5, 8, 13, 16, 64 };
constexpr std::array<uint32_t, 12> SHARDS{ 1, 2, 3, 5, 7, 8, 13, 16, 64, 255, 256, 1000 };

// Restores both hooks however a case leaves them, failed assertion included.
struct HookGuard {
  HookGuard() = default;
  HookGuard(HookGuard const &) = delete;
  HookGuard &operator=(HookGuard const &) = delete;
  ~HookGuard() {
    thread_test_spawn_limit(0);
    thread_test_delay_seed(0);
  }
};

struct Counters {
  std::vector<uint32_t> hits;
};

void bump(void *ctx, uint32_t shard) { static_cast<Counters *>(ctx)->hits[shard] += 1U; }

void visit(void *ctx, uint32_t shard) {
  static_cast<std::vector<uint32_t> *>(ctx)->push_back(shard);
}

// Output of the capture-free shard body.
std::vector<uint32_t> stateless_hits;

uint32_t count_of(std::vector<uint32_t> const &hits, uint32_t want) {
  uint32_t n{ 0 };
  for (uint32_t h : hits) { n += (h == want) ? 1U : 0U; }
  return n;
}

// xxhash32 of the shard index as four little-endian bytes.
uint32_t shard_value(uint32_t shard) {
  std::array<scav_byte, 4> const key{ static_cast<scav_byte>(shard & 0xFFU),
                                      static_cast<scav_byte>((shard >> 8U) & 0xFFU),
                                      static_cast<scav_byte>((shard >> 16U) & 0xFFU),
                                      static_cast<scav_byte>((shard >> 24U) & 0xFFU) };
  return xxhash32(key.data(), key.size(), 0U);
}

// xxhash32 of the parts as little-endian words in index order.
uint32_t merge_in_index_order(std::vector<uint32_t> const &parts) {
  std::vector<scav_byte> bytes;
  bytes.reserve(parts.size() * 4U);
  for (uint32_t part : parts) {
    bytes.push_back(static_cast<scav_byte>(part & 0xFFU));
    bytes.push_back(static_cast<scav_byte>((part >> 8U) & 0xFFU));
    bytes.push_back(static_cast<scav_byte>((part >> 16U) & 0xFFU));
    bytes.push_back(static_cast<scav_byte>((part >> 24U) & 0xFFU));
  }
  return xxhash32(bytes.data(), bytes.size(), 0U);
}

std::vector<uint32_t> run_hits(uint32_t shards, uint32_t threads) {
  std::vector<uint32_t> hits(shards, 0);
  auto body = [&hits](uint32_t shard) { hits[shard] += 1U; };
  parallel_for(shards, threads, body);
  return hits;
}

}  // namespace

TEST_CASE("thread: no shards calls the body at no thread count") {
  for (uint32_t threads : THREADS) {
    CAPTURE(threads);
    uint32_t calls{ 0 };
    auto body = [&calls](uint32_t /*shard*/) { calls += 1U; };
    parallel_for(0U, threads, body);
    CHECK(calls == 0U);

    Counters counters{ .hits = {} };
    parallel_for(0U, threads, bump, &counters);
    CHECK(counters.hits.empty());
  }
}

TEST_CASE("thread: every shard runs exactly once at every thread count") {
  for (uint32_t threads : THREADS) {
    for (uint32_t shards : SHARDS) {
      CAPTURE(threads);
      CAPTURE(shards);
      std::vector<uint32_t> const hits{ run_hits(shards, threads) };
      CHECK(count_of(hits, 1U) == shards);
    }
  }
}

TEST_CASE("thread: the raw function-pointer overload runs every shard once") {
  for (uint32_t threads : THREADS) {
    CAPTURE(threads);
    Counters counters{ .hits = std::vector<uint32_t>(37U, 0U) };
    parallel_for(37U, threads, bump, &counters);
    CHECK(count_of(counters.hits, 1U) == 37U);
  }
}

TEST_CASE("thread: a capturing functor reaches its captures") {
  uint32_t const shards{ 100 };
  std::vector<uint32_t> seen(shards, 0);
  uint32_t const bias{ 7 };
  auto body = [&seen](uint32_t shard) { seen[shard] = shard + bias; };
  parallel_for(shards, 13U, body);
  bool correct{ true };
  for (uint32_t shard = 0; shard < shards; ++shard) {
    correct = correct && (seen[shard] == (shard + bias));
  }
  CHECK(correct);
}

TEST_CASE("thread: a lambda expression is a body without being named first") {
  uint32_t const shards{ 64 };
  std::vector<uint32_t> hits(shards, 0);

  parallel_for(shards, 8U, [&hits](uint32_t shard) { hits[shard] += 1U; });
  CHECK(count_of(hits, 1U) == shards);

  stateless_hits.assign(shards, 0);
  parallel_for(shards, 8U, [](uint32_t shard) { stateless_hits[shard] += 1U; });
  CHECK(count_of(stateless_hits, 1U) == shards);

  auto named = [&hits](uint32_t shard) { hits[shard] += 1U; };
  parallel_for(shards, 8U, named);
  CHECK(count_of(hits, 2U) == shards);
}

TEST_CASE("thread: no more workers than shards") {
  for (uint32_t shards : { 1U, 2U, 3U, 5U }) {
    CAPTURE(shards);
    std::vector<std::thread::id> where(shards);
    auto body = [&where](uint32_t shard) { where[shard] = std::this_thread::get_id(); };
    parallel_for(shards, 64U, body);
    std::set<std::thread::id> const distinct(where.begin(), where.end());
    CHECK(distinct.size() <= shards);
  }
}

TEST_CASE("thread: asking for more threads than shards still runs every shard") {
  HookGuard const guard;
  for (uint32_t shards : { 3U, 8U, 16U }) {
    CAPTURE(shards);
    // Limits the pool to shards - 2 active workers; every shard still runs once.
    thread_test_spawn_limit(shards - 2U);
    std::vector<uint32_t> const hits{ run_hits(shards, 64U) };
    CHECK(count_of(hits, 1U) == shards);
  }
}

TEST_CASE("thread: per-shard lists concatenated in index order rebuild the sequence") {
  for (uint32_t threads : THREADS) {
    for (uint32_t shards : { 1U, 7U, 64U, 255U }) {
      CAPTURE(threads);
      CAPTURE(shards);
      std::vector<std::vector<uint32_t>> parts(shards);
      auto body = [&parts](uint32_t shard) { parts[shard].push_back(shard); };
      parallel_for(shards, threads, body);

      std::vector<uint32_t> merged;
      merged.reserve(shards);
      for (std::vector<uint32_t> const &part : parts) {
        for (uint32_t v : part) { merged.push_back(v); }
      }

      CHECK(merged.size() == shards);
      bool sequence{ merged.size() == shards };
      for (uint32_t i = 0; sequence && (i < shards); ++i) { sequence = (merged[i] == i); }
      CHECK(sequence);
    }
  }
}

TEST_CASE("thread: an index-ordered reduction is the same value at every thread count") {
  uint32_t const shards{ 256 };
  std::vector<uint32_t> reference(shards, 0);
  for (uint32_t shard = 0; shard < shards; ++shard) {
    reference[shard] = shard_value(shard);
  }
  uint32_t const want{ merge_in_index_order(reference) };

  for (uint32_t threads : { 1U, 2U, 3U, 5U, 8U, 13U, 16U }) {
    CAPTURE(threads);
    std::vector<uint32_t> parts(shards, 0);
    auto body = [&parts](uint32_t shard) { parts[shard] = shard_value(shard); };
    parallel_for(shards, threads, body);
    CHECK(merge_in_index_order(parts) == want);
  }
}

TEST_CASE("thread: a worker that cannot be spawned runs on the caller") {
  HookGuard const guard;
  uint32_t const shards{ 40 };

  // One active worker besides the caller; the two run every shard.
  thread_test_spawn_limit(1U);
  CHECK(count_of(run_hits(shards, 8U), 1U) == shards);

  // A limit no call reaches changes nothing, and neither does turning it off.
  thread_test_spawn_limit(64U);
  CHECK(count_of(run_hits(shards, 8U), 1U) == shards);

  thread_test_spawn_limit(0U);
  CHECK(count_of(run_hits(shards, 8U), 1U) == shards);
}

TEST_CASE("thread: a spawn limit caps the threads that claim shards") {
  HookGuard const guard;
  uint32_t const shards{ 64 };
  for (uint32_t threads : { 2U, 3U, 5U }) {
    CAPTURE(threads);
    thread_test_spawn_limit(threads - 1U);
    uint32_t const want{ std::min(threads, thread_concurrency()) };
    // Each shard waits up to 10 s for `want` threads to arrive, so a thread past the
    // cap has time to join and be counted.
    Mutex m;
    std::set<std::thread::id> seen;
    std::atomic<uint32_t> arrived{ 0 };
    auto body = [&](uint32_t /*shard*/) {
      {
        ScopedLock const held{ m };
        seen.insert(std::this_thread::get_id());
        arrived.store(static_cast<uint32_t>(seen.size()));
      }
      auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds(10) };
      while ((arrived.load() < want) && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::yield();
      }
    };
    thread_test_participants();
    parallel_for(shards, threads, body);
    CHECK(seen.size() == want);
    CHECK(thread_test_participants() == ((want > 1U) ? want : 0U));
  }
}

TEST_CASE("thread: a spawn failure moves work between threads and nothing else") {
  HookGuard const guard;
  uint32_t const shards{ 128 };
  std::vector<uint32_t> reference(shards, 0);
  for (uint32_t shard = 0; shard < shards; ++shard) {
    reference[shard] = shard_value(shard);
  }
  uint32_t const want{ merge_in_index_order(reference) };

  for (uint32_t limit : { 0U, 1U, 2U, 4U, 7U }) {
    CAPTURE(limit);
    thread_test_spawn_limit(limit);
    std::vector<uint32_t> parts(shards, 0);
    auto body = [&parts](uint32_t shard) { parts[shard] = shard_value(shard); };
    parallel_for(shards, 8U, body);
    CHECK(merge_in_index_order(parts) == want);
  }
}

TEST_CASE("thread: the delay injector perturbs the interleaving, not the result") {
  HookGuard const guard;
  uint32_t const shards{ 64 };
  std::vector<uint32_t> reference(shards, 0);
  for (uint32_t shard = 0; shard < shards; ++shard) {
    reference[shard] = shard_value(shard);
  }
  uint32_t const want{ merge_in_index_order(reference) };

  bool concurrent{ false };
  bool perturbed{ false };
  for (uint64_t seed = 1; seed <= 16U; ++seed) {
    thread_test_delay_seed(seed);
    std::vector<uint32_t> parts(shards, 0);
    std::vector<uint32_t> finished(shards, 0);
    std::vector<std::thread::id> where(shards);
    std::atomic<uint32_t> counter{ 0 };
    auto body = [&](uint32_t shard) {
      parts[shard] = shard_value(shard);
      where[shard] = std::this_thread::get_id();
      finished[shard] = counter.fetch_add(1U);
    };
    parallel_for(shards, 8U, body);

    CHECK(merge_in_index_order(parts) == want);
    std::set<std::thread::id> const distinct(where.begin(), where.end());
    concurrent = concurrent || (distinct.size() > 1U);
    for (uint32_t shard = 0; shard < shards; ++shard) {
      perturbed = perturbed || (finished[shard] != shard);
    }
  }

  // The null backend runs every shard on the caller in index order; anything
  // that actually spawned has to have completed out of order at least once.
  if (concurrent) {
    CHECK(perturbed);
  } else {
    CHECK_FALSE(perturbed);
  }
}

TEST_CASE("thread: one thread completes in index order whatever the injector says") {
  HookGuard const guard;
  uint32_t const shards{ 64 };
  for (uint64_t seed : { UINT64_C(0), UINT64_C(1), UINT64_C(0xDEAD'BEEF) }) {
    CAPTURE(seed);
    thread_test_delay_seed(seed);
    std::vector<uint32_t> finished(shards, 0);
    std::atomic<uint32_t> counter{ 0 };
    auto body = [&](uint32_t shard) { finished[shard] = counter.fetch_add(1U); };
    parallel_for(shards, 1U, body);
    bool in_order{ true };
    for (uint32_t shard = 0; shard < shards; ++shard) {
      in_order = in_order && (finished[shard] == shard);
    }
    CHECK(in_order);
  }
}

TEST_CASE("thread: a shard body may run its own parallel_for") {
  uint32_t const outer{ 8 };
  uint32_t const inner{ 11 };
  for (uint32_t threads : THREADS) {
    CAPTURE(threads);
    std::vector<std::vector<uint32_t>> hits(outer, std::vector<uint32_t>(inner, 0));
    auto body = [&hits](uint32_t o) {
      auto nested = [&hits, o](uint32_t i) { hits[o][i] += 1U; };
      parallel_for(inner, 3U, nested);
    };
    parallel_for(outer, threads, body);
    uint32_t ones{ 0 };
    for (std::vector<uint32_t> const &row : hits) { ones += count_of(row, 1U); }
    CHECK(ones == (outer * inner));
  }
}

TEST_CASE("thread: run_stripe walks one worker's shards in index order") {
  std::vector<uint32_t> visited;
  run_stripe(10U, 3U, 1U, visit, &visited);
  CHECK(visited == std::vector<uint32_t>{ 1U, 4U, 7U });

  visited.clear();
  run_stripe(10U, 1U, 0U, visit, &visited);
  CHECK(visited == std::vector<uint32_t>{ 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U });

  visited.clear();
  run_stripe(3U, 4U, 3U, visit, &visited);
  CHECK(visited.empty());

  // The next index after the one shard here is above UINT32_MAX.
  visited.clear();
  run_stripe(UINT32_MAX, UINT32_MAX - 1U, UINT32_MAX - 2U, visit, &visited);
  CHECK(visited == std::vector<uint32_t>{ UINT32_MAX - 2U });
}

TEST_CASE("thread: calls nested three deep finish with one worker to share") {
  // With one worker, each level's waiter runs the level below's shards itself.
  HookGuard const guard;
  thread_test_spawn_limit(1U);
  uint32_t const outer{ 4 };
  uint32_t const middle{ 3 };
  uint32_t const inner{ 5 };
  std::vector<uint32_t> hits(size_t{ outer } * middle * inner, 0);
  auto body = [&](uint32_t o) {
    parallel_for(middle, 0U, [&, o](uint32_t m) {
      parallel_for(inner, 0U, [&, o, m](uint32_t i) {
        hits[(((o * middle) + m) * inner) + i] += 1U;
      });
    });
  };
  parallel_for(outer, 0U, body);
  CHECK(count_of(hits, 1U) == hits.size());
}

TEST_CASE("thread: two callers on threads of their own share the pool") {
  constexpr uint32_t PER_CALL{ 512 };
  std::vector<uint32_t> a(PER_CALL, 0);
  std::vector<uint32_t> b(PER_CALL, 0);
  auto run = [](std::vector<uint32_t> &hits) {
    for (uint32_t round = 0; round < 8; ++round) {
      parallel_for(PER_CALL, 0U, [&hits](uint32_t s) { hits[s] += 1U; });
    }
  };
  std::thread first([&] { run(a); });
  std::thread second([&] { run(b); });
  first.join();
  second.join();
  CHECK(count_of(a, 8U) == PER_CALL);
  CHECK(count_of(b, 8U) == PER_CALL);
}

TEST_CASE("thread: a mutex lets one holder in at a time") {
  // A holder that enters while another is inside finds `inside` nonzero.
  Mutex m;
  std::atomic<uint32_t> inside{ 0 };
  std::atomic<uint32_t> overlaps{ 0 };
  uint64_t total{ 0 };
  constexpr uint32_t HOLDS{ 2000 };
  parallel_for(16U, 0U, [&](uint32_t /*shard*/) {
    for (uint32_t i = 0; i < HOLDS; ++i) {
      ScopedLock const held{ m };
      if (inside.fetch_add(1U) != 0U) { overlaps.fetch_add(1U); }
      uint32_t volatile dwell{ 0 };
      for (uint32_t k = 0; k < 64U; ++k) { dwell = dwell + 1U; }
      ++total;
      inside.fetch_sub(1U);
    }
  });
  CHECK(overlaps.load() == 0U);
  CHECK(total == uint64_t{ 16 } * HOLDS);
}

// Two host threads contend the lock, so the null backend's lock is tested too.
TEST_CASE("thread: a mutex lets one host thread in at a time") {
  Mutex m;
  std::atomic<uint32_t> inside{ 0 };
  std::atomic<uint32_t> overlaps{ 0 };
  uint64_t total{ 0 };
  constexpr uint32_t HOLDS{ 20000 };
  auto const hold = [&] {
    for (uint32_t i = 0; i < HOLDS; ++i) {
      ScopedLock const held{ m };
      if (inside.fetch_add(1U) != 0U) { overlaps.fetch_add(1U); }
      uint32_t volatile dwell{ 0 };
      for (uint32_t k = 0; k < 64U; ++k) { dwell = dwell + 1U; }
      ++total;
      inside.fetch_sub(1U);
    }
  };
  std::thread first(hold);
  std::thread second(hold);
  first.join();
  second.join();
  CHECK(overlaps.load() == 0U);
  CHECK(total == uint64_t{ 2 } * HOLDS);
}

TEST_CASE("thread: a mutex held on one host thread keeps another out until released") {
  Mutex m;
  std::atomic<bool> trying{ false };
  std::atomic<bool> entered{ false };
  m.lock();
  std::thread other([&] {
    trying.store(true);
    ScopedLock const held{ m };
    entered.store(true);
  });
  while (!trying.load()) { std::this_thread::yield(); }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_FALSE(entered.load());
  m.unlock();
  other.join();
  CHECK(entered.load());
}

TEST_CASE("thread: a started thread runs its function once, and join waits for it") {
  struct Run {
    std::atomic<uint32_t> calls{ 0 };
    std::atomic<bool> done{ false };
  };
  Run run;
  Thread t;
  t.join();  // a thread never started joins as a no-op
  bool const started{ t.start(
      [](void *ctx) {
        auto *const r{ static_cast<Run *>(ctx) };
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        r->calls.fetch_add(1U);
        r->done.store(true);
      },
      &run) };
  t.join();
  if (!started) {
    CHECK(thread_concurrency() == 1U);  // only the null backend starts no thread
    CHECK(run.calls.load() == 0U);
    return;
  }
  CHECK(run.done.load());
  CHECK(run.calls.load() == 1U);
  t.join();  // a second join is a no-op
  CHECK(run.calls.load() == 1U);
}

TEST_CASE("thread: a destroyed thread has finished its function") {
  std::atomic<bool> done{ false };
  {
    Thread t;
    if (!t.start(
            [](void *ctx) {
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
              static_cast<std::atomic<bool> *>(ctx)->store(true);
            },
            &done)) {
      return;
    }
  }
  CHECK(done.load());
}

// Two host threads take turns through one condition variable, so the null backend's
// wait, which returns with no notify, is tested too.
TEST_CASE("thread: a condition variable hands a turn between two threads in order") {
  struct Turns {
    Mutex m;
    ConditionVariable changed;
    uint32_t next{ 0 };  // under `m`: the turn number to take next; even is the caller's
    std::vector<uint32_t> taken;  // under `m`
  };
  Turns turns;
  constexpr uint32_t ROUNDS{ 2000 };
  auto const play = [&turns](uint32_t parity) {
    for (uint32_t k = 0; k < ROUNDS; ++k) {
      ScopedLock const held{ turns.m };
      while ((turns.next % 2U) != parity) { turns.changed.wait(turns.m); }
      turns.taken.push_back(turns.next);
      ++turns.next;
      turns.changed.notify_all();
    }
  };
  std::thread other([&] { play(1U); });
  play(0U);
  other.join();
  REQUIRE(turns.taken.size() == 2U * ROUNDS);
  for (uint32_t k = 0; k < turns.taken.size(); ++k) { CHECK(turns.taken[k] == k); }
}

TEST_CASE("thread: a waiter releases its mutex while it waits and holds it on return") {
  struct Gate {
    Mutex m;
    ConditionVariable changed;
    bool open{ false };     // under `m`
    bool waiting{ false };  // under `m`
  };
  Gate gate;
  std::atomic<bool> inside{ false };  // the waiter is between waking and releasing `m`
  std::atomic<bool> finished{ false };
  std::thread waiter([&] {
    ScopedLock const held{ gate.m };
    gate.waiting = true;
    while (!gate.open) { gate.changed.wait(gate.m); }
    inside.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    inside.store(false);
    finished.store(true);
  });
  // Locking `m` while the waiter waits shows the wait released it.
  for (bool opened{ false }; !opened;) {
    ScopedLock const held{ gate.m };
    if (gate.waiting) {
      gate.open = true;
      gate.changed.notify_all();
      opened = true;
    }
  }
  // Every entry after waking finds the waiter outside `m`.
  uint32_t overlaps{ 0 };
  while (!finished.load()) {
    ScopedLock const held{ gate.m };
    overlaps += inside.load() ? 1U : 0U;
  }
  waiter.join();
  CHECK(overlaps == 0U);
}

TEST_CASE("thread: a chart lays out the same however many threads claim its shards" *
          doctest::test_suite("full")) {
  HookGuard const guard;
  constexpr std::array<uint32_t, 4> COUNTS{ 1, 2, 3, 5 };
  constexpr std::array<char const *, 3> CHARTS{ "estop", "led", "brew" };
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  std::array<uint32_t, COUNTS.size()> most{};  // the most participants any chart had
  for (char const *name : CHARTS) {
    std::string const chart{ name };
    CAPTURE(chart);
    std::array<uint32_t, 2> want{};
    for (uint32_t k = 0; k < COUNTS.size(); ++k) {
      uint32_t const threads{ COUNTS[k] };
      CAPTURE(threads);
      thread_test_spawn_limit(threads - 1U);  // workers beside the caller
      std::string const path{ std::string{ SCAV_TEST_DATA_DIR "/charts/" } + name +
                              ".scav" };
      Loader loader;
      Chart c;
      std::vector<Diagnostic> diags;
      std::string failed;
      REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
      scav_layout_opts const o{ .profile = p, .router = 0, .threads = threads };
      std::vector<scav_placed> placed;
      thread_test_participants();
      REQUIRE(layout_run(c, {}, o, placed, diags));
      uint32_t const participants{ thread_test_participants() };
      CHECK(participants <= threads);
      most[k] = std::max(most[k], participants);
      std::array<uint32_t, 2> const got{ layout_structural_hash(c),
                                         layout_coordinate_hash(c) };
      if (k == 0) {
        want = got;
      } else {
        CHECK(got == want);
      }
    }
  }
  std::string seen;
  for (uint32_t const n : most) { seen += ' ' + std::to_string(n); }
  MESSAGE("most participants at 1, 2, 3, 5 threads:", seen);
  CHECK(most[0] == 0U);
  if (thread_concurrency() >= 2U) { CHECK(most[1] == 2U); }
  if (thread_concurrency() >= 3U) { CHECK(most[3] >= 3U); }
}
