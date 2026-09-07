// Each shard runs exactly once, and a reduction merged in index order is one
// value at every thread count. A shard writes only its own slot.

#include "scav_thread.h"

#include "scav/scav_types.h"
#include "scav_xxhash.h"

#include "doctest.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <set>
#include <thread>
#include <vector>

namespace scav {
void thread_test_spawn_limit(uint32_t limit);
void thread_test_delay_seed(uint64_t seed);
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

// Where a body with no captures has to write.
std::vector<uint32_t> stateless_hits;

uint32_t count_of(std::vector<uint32_t> const &hits, uint32_t want) {
  uint32_t n{ 0 };
  for (uint32_t h : hits) { n += (h == want) ? 1U : 0U; }
  return n;
}

// The shard index as four bytes, so a per-shard value has something to hash.
uint32_t shard_value(uint32_t shard) {
  std::array<scav_byte, 4> const key{ static_cast<scav_byte>(shard & 0xFFU),
                                      static_cast<scav_byte>((shard >> 8U) & 0xFFU),
                                      static_cast<scav_byte>((shard >> 16U) & 0xFFU),
                                      static_cast<scav_byte>((shard >> 24U) & 0xFFU) };
  return xxhash32(key.data(), key.size(), 0U);
}

// List append merged in index order: the shape §6 mandates for a reduction, so
// the digest moves if any shard's part lands in the wrong place.
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

TEST_CASE("thread: workers are capped at MAX_WORKERS however many are asked for") {
  uint32_t const shards{ 1000 };
  std::vector<std::thread::id> where(shards);
  auto body = [&where](uint32_t shard) { where[shard] = std::this_thread::get_id(); };
  parallel_for(shards, 1000U, body);
  std::set<std::thread::id> const distinct(where.begin(), where.end());
  CHECK(distinct.size() <= MAX_WORKERS);
  CHECK(count_of(run_hits(shards, 1000U), 1U) == shards);
}

TEST_CASE("thread: asking for more threads than shards still runs every shard") {
  HookGuard const guard;
  for (uint32_t shards : { 2U, 3U, 8U, 16U }) {
    CAPTURE(shards);
    // One fewer spawn than the shim would attempt, so the last stripe has to
    // come back to the caller.
    thread_test_spawn_limit(shards - 1U);
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

  // One spawn allowed of the seven the shim attempts, so six stripes come back
  // to the caller.
  thread_test_spawn_limit(1U);
  CHECK(count_of(run_hits(shards, 8U), 1U) == shards);

  // A limit no call reaches changes nothing, and neither does turning it off.
  thread_test_spawn_limit(64U);
  CHECK(count_of(run_hits(shards, 8U), 1U) == shards);

  thread_test_spawn_limit(0U);
  CHECK(count_of(run_hits(shards, 8U), 1U) == shards);
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
}
