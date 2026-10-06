#ifndef SCAV_LAYOUT_CHUNK_QUEUE_H_INCLUDED
#define SCAV_LAYOUT_CHUNK_QUEUE_H_INCLUDED

// A bounded FIFO of byte chunks that one writer thread drains in push order.

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

  // Queues `chunk` and leaves it empty, holding a written chunk's storage. False once a
  // write has failed; the queue then drops every chunk.
  bool push(std::vector<uint8_t> &chunk);

  // Writes every queued chunk and joins the writer; false when a write failed.
  bool close();

  [[nodiscard]] bool threaded() const { return running; }

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
  Thread writer;
};

}  // namespace scav

#endif  // SCAV_LAYOUT_CHUNK_QUEUE_H_INCLUDED
