// The chunk queue's order, bound, blocking and failure.

#include "layout/chunk_queue.h"

#include "doctest.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace scav;

using Clock = std::chrono::steady_clock;

// Chunk `k` of a run: `bytes` bytes, each a function of its chunk and position.
std::vector<uint8_t> chunk_bytes(uint32_t k, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  for (size_t i = 0; i < bytes; ++i) { out[i] = static_cast<uint8_t>((k * 131U) + i); }
  return out;
}

// What the writer saw, read by the test only after `close` joins the writer.
struct Seen {
  std::vector<uint8_t> bytes;
  std::set<uint8_t const *> buffers;  // distinct chunk storage the writer was handed
  uint32_t calls{ 0 };
};

bool record(void *ctx, uint8_t const *data, size_t n) {
  auto *const s{ static_cast<Seen *>(ctx) };
  s->bytes.insert(s->bytes.end(), data, data + n);
  s->buffers.insert(data);
  ++s->calls;
  return true;
}

}  // namespace

TEST_CASE("chunk queue: every byte arrives in push order") {
  Seen seen;
  std::vector<uint8_t> want;
  {
    ChunkQueue q{ record, &seen, 3 };
    std::vector<uint8_t> chunk;
    for (uint32_t k = 0; k < 200; ++k) {
      std::vector<uint8_t> const part{ chunk_bytes(k, 1U + ((k * 977U) % 5000U)) };
      want.insert(want.end(), part.begin(), part.end());
      chunk.assign(part.begin(), part.end());
      REQUIRE(q.push(chunk));
      CHECK(chunk.empty());
    }
    CHECK(q.close());
  }
  CHECK(seen.calls == 200U);
  CHECK(seen.bytes == want);
}

TEST_CASE("chunk queue: closing with nothing pushed writes nothing") {
  Seen seen;
  ChunkQueue q{ record, &seen, 4 };
  CHECK(q.close());
  CHECK(q.close());  // a second close is a no-op
  CHECK(seen.calls == 0U);
}

// The writer sleeps 2 ms per chunk.
TEST_CASE("chunk queue: a slow writer blocks the producer and bounds the chunks alive") {
  constexpr uint32_t DEPTH{ 3 };
  constexpr uint32_t CHUNKS{ 40 };
  constexpr size_t BYTES{ 4096 };
  struct Slow {
    Seen seen;
    std::atomic<uint32_t> attempted{ 0 };  // pushes begun by the producer
    uint32_t written{ 0 };                 // writes finished; the writer's own
    uint32_t most{ 0 };                    // the most chunks alive at a write
  };
  Slow slow;
  ChunkWrite const write = [](void *ctx, uint8_t const *data, size_t n) {
    auto *const s{ static_cast<Slow *>(ctx) };
    // Alive: the chunks pushed and not yet written, and the one the producer holds.
    uint32_t const alive{ s->attempted.load() - s->written };
    s->most = (alive > s->most) ? alive : s->most;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    record(&s->seen, data, n);
    ++s->written;
    return true;
  };
  std::vector<uint8_t> want;
  uint32_t waited{ 0 };  // pushes that blocked for a writer's sleep or longer
  bool threaded{ false };
  {
    ChunkQueue q{ write, &slow, DEPTH };
    threaded = q.threaded();
    std::vector<uint8_t> chunk;
    for (uint32_t k = 0; k < CHUNKS; ++k) {
      std::vector<uint8_t> const part{ chunk_bytes(k, BYTES) };
      want.insert(want.end(), part.begin(), part.end());
      chunk.assign(part.begin(), part.end());
      slow.attempted.fetch_add(1U);
      Clock::time_point const began{ Clock::now() };
      REQUIRE(q.push(chunk));
      waited += (Clock::now() - began >= std::chrono::milliseconds(1)) ? 1U : 0U;
    }
    CHECK(q.close());
  }
  CHECK(slow.seen.bytes == want);
  CHECK(slow.most <= DEPTH + 1U);
  // Storage is recycled: the writer sees at most the slots and the producer's chunk.
  CHECK(slow.seen.buffers.size() <= DEPTH + 1U);
  if (threaded) { CHECK(waited >= CHUNKS - DEPTH - 1U); }
}

// The writer holds its first chunk until the test opens a gate.
TEST_CASE("chunk queue: a producer stops at depth chunks until the writer frees one") {
  constexpr uint32_t DEPTH{ 3 };
  struct Gated {
    Seen seen;
    std::atomic<bool> open{ false };
    std::atomic<uint32_t> entered{ 0 };
  };
  Gated gated;
  ChunkWrite const write = [](void *ctx, uint8_t const *data, size_t n) {
    auto *const g{ static_cast<Gated *>(ctx) };
    g->entered.fetch_add(1U);
    while (!g->open.load()) { std::this_thread::yield(); }
    return record(&g->seen, data, n);
  };
  ChunkQueue q{ write, &gated, DEPTH };
  if (!q.threaded()) {
    gated.open.store(true);
    CHECK(q.close());
    return;
  }
  std::atomic<uint32_t> pushed{ 0 };
  std::vector<uint8_t> want;
  for (uint32_t k = 0; k < 10; ++k) {
    std::vector<uint8_t> const part{ chunk_bytes(k, 100) };
    want.insert(want.end(), part.begin(), part.end());
  }
  std::thread producer([&] {
    std::vector<uint8_t> chunk;
    for (uint32_t k = 0; k < 10; ++k) {
      chunk = chunk_bytes(k, 100);
      if (!q.push(chunk)) { return; }
      pushed.fetch_add(1U);
    }
  });
  Clock::time_point const began{ Clock::now() };
  while (((pushed.load() < DEPTH) || (gated.entered.load() < 1U)) &&
         (Clock::now() - began < std::chrono::seconds(10))) {
    std::this_thread::yield();
  }
  REQUIRE(pushed.load() == DEPTH);
  // The first chunk is in the writer and two wait; a fourth push has no slot.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(pushed.load() == DEPTH);
  CHECK(gated.entered.load() == 1U);
  gated.open.store(true);
  producer.join();
  CHECK(q.close());
  CHECK(pushed.load() == 10U);
  CHECK(gated.seen.bytes == want);
}

TEST_CASE(
    "chunk queue: a failed write fails later pushes and the close, writing no more") {
  struct Failing {
    uint32_t calls{ 0 };
  };
  Failing failing;
  ChunkWrite const write = [](void *ctx, uint8_t const * /*data*/, size_t /*n*/) {
    auto *const f{ static_cast<Failing *>(ctx) };
    ++f->calls;
    return f->calls < 3U;  // the third chunk fails
  };
  ChunkQueue q{ write, &failing, 3 };
  std::vector<bool> took;
  std::vector<uint8_t> chunk;
  for (uint32_t k = 0; k < 12; ++k) {
    chunk = chunk_bytes(k, 64);
    took.push_back(q.push(chunk));
    CHECK(chunk.empty());
  }
  CHECK_FALSE(q.close());
  CHECK(failing.calls == 3U);
  // The first two were written; from the seventh on, no slot frees before the failure.
  CHECK(took[0]);
  CHECK(took[1]);
  for (uint32_t k = 6; k < took.size(); ++k) { CHECK_FALSE(took[k]); }
}
