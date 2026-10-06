// The chunk queue's order, bound, blocking and failure.

#include "scav_chunk_queue.h"

#include "doctest.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace scav;

// Chunk `k` of a run: `bytes` bytes, each a function of its chunk and position.
std::vector<uint8_t> chunk_bytes(uint32_t k, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  for (size_t i = 0; i < bytes; ++i) {
    out[i] = static_cast<uint8_t>((size_t{ k } * 131U) + i);
  }
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

// Spins until `done()` holds; false past a ten-second deadline.
template <typename F>
bool wait_until(F &&done) {
  auto const began{ std::chrono::steady_clock::now() };
  while (!done()) {
    if (std::chrono::steady_clock::now() - began > std::chrono::seconds(10)) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

}  // namespace

TEST_CASE("chunk queue: every byte arrives in push order") {
  Seen seen;
  std::vector<uint8_t> want;
  {
    ChunkQueue q{ record, &seen, 3 };
    for (uint32_t k = 0; k < 200; ++k) {
      std::vector<uint8_t> const part{ chunk_bytes(k, 1U + ((k * 977U) % 5000U)) };
      want.insert(want.end(), part.begin(), part.end());
      REQUIRE(q.push(part.data(), part.size()));
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

// The writer holds each chunk until the test grants it a write.
TEST_CASE("chunk queue: past depth chunks, each push blocks until a write frees a slot") {
  constexpr uint32_t DEPTH{ 3 };
  constexpr uint32_t CHUNKS{ 12 };
  struct Gated {
    Seen seen;
    std::atomic<uint32_t> granted{ 0 };  // writes the test allows
    std::atomic<uint32_t> written{ 0 };  // writes finished
  };
  Gated gated;
  ChunkWrite const write = [](void *ctx, uint8_t const *data, size_t n) {
    auto *const g{ static_cast<Gated *>(ctx) };
    while (g->written.load() == g->granted.load()) { std::this_thread::yield(); }
    record(&g->seen, data, n);
    g->written.fetch_add(1U);
    return true;
  };
  ChunkQueue q{ write, &gated, DEPTH };
  if (!q.threaded()) {
    gated.granted.store(CHUNKS);
    CHECK(q.close());
    return;
  }
  std::vector<uint8_t> want;
  for (uint32_t k = 0; k < CHUNKS; ++k) {
    std::vector<uint8_t> const part{ chunk_bytes(k, 64) };
    want.insert(want.end(), part.begin(), part.end());
  }
  std::atomic<uint32_t> pushed{ 0 };
  std::thread producer([&] {
    for (uint32_t k = 0; k < CHUNKS; ++k) {
      std::vector<uint8_t> const part{ chunk_bytes(k, 64) };
      if (!q.push(part.data(), part.size())) { return; }
      pushed.fetch_add(1U);
    }
  });
  for (uint32_t k = DEPTH; k < CHUNKS; ++k) {
    CAPTURE(k);
    // Push `k` found every slot taken: one chunk in the writer, the rest waiting.
    REQUIRE(wait_until([&] { return q.test_blocked() == k - DEPTH + 1U; }));
    CHECK(pushed.load() == k);
    CHECK(gated.written.load() == k - DEPTH);
    gated.granted.fetch_add(1U);
  }
  gated.granted.store(CHUNKS);
  producer.join();
  CHECK(q.close());
  CHECK(pushed.load() == CHUNKS);
  CHECK(q.test_blocked() == CHUNKS - DEPTH);
  CHECK(gated.seen.bytes == want);
  CHECK(gated.seen.buffers.size() <= DEPTH);  // each slot reuses its storage
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
  std::array<bool, 12> took{};
  for (uint32_t k = 0; k < took.size(); ++k) {
    std::vector<uint8_t> const chunk{ chunk_bytes(k, 64) };
    took[k] = q.push(chunk.data(), chunk.size());
  }
  CHECK_FALSE(q.close());
  CHECK(failing.calls == 3U);
  // The first two were written; from the seventh on, no slot frees before the failure.
  CHECK(took[0]);
  CHECK(took[1]);
  for (uint32_t k = 6; k < took.size(); ++k) { CHECK_FALSE(took[k]); }
}
