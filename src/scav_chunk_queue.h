#ifndef SCAV_CHUNK_QUEUE_H_INCLUDED
#define SCAV_CHUNK_QUEUE_H_INCLUDED

// A bounded FIFO of byte chunks from one producer, drained in push order by one writer
// thread.

#include "scav_thread.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scav {

// Receives one chunk; false fails the queue.
using ChunkWrite = bool (*)(void *ctx, uint8_t const *data, size_t n);

// `push` blocks while `depth` chunks wait or are being written. With no writer thread,
// `push` calls `write` itself.
class ChunkQueue {
 public:
  ChunkQueue(ChunkWrite write, void *ctx, uint32_t depth);
  ~ChunkQueue();  // closes
  ChunkQueue(ChunkQueue const &) = delete;
  ChunkQueue &operator=(ChunkQueue const &) = delete;

  // Copies `data[0..n)` into a free slot. False once a write has failed; the queue then
  // drops every chunk.
  bool push(uint8_t const *data, size_t n);

  // Writes every queued chunk and joins the writer; false when a write failed.
  bool close();

  [[nodiscard]] bool threaded() const { return running; }

#ifdef SCAV_TESTING
  uint32_t test_blocked();  // pushes that found every slot taken
#endif

 private:
  static void drain(void *self);  // the writer thread's loop

  ChunkWrite const write;
  void *const ctx;
  Mutex lock;
  ConditionVariable changed;  // a chunk was queued or written, or the queue is closing
  // A ring: `count` chunks from `head` are queued; the others hold written storage.
  std::vector<std::vector<uint8_t>> slots;
  uint32_t head{ 0 };     // under `lock`
  uint32_t count{ 0 };    // under `lock`
  bool closing{ false };  // under `lock`
  bool failed{ false };   // under `lock`
  bool running{ false };  // the producer's
#ifdef SCAV_TESTING
  uint32_t blocked{ 0 };  // under `lock`
#endif
  Thread writer;
};

}  // namespace scav

#endif  // SCAV_CHUNK_QUEUE_H_INCLUDED
