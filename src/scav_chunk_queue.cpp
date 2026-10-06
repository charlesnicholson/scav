#include "scav_chunk_queue.h"

#include "scav_thread.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scav {

ChunkQueue::ChunkQueue(ChunkWrite write_fn, void *write_ctx, uint32_t depth)
    : write(write_fn), ctx(write_ctx), slots((depth > 0) ? depth : 1U) {
  running = writer.start(drain, this);
}

ChunkQueue::~ChunkQueue() { close(); }

bool ChunkQueue::push(uint8_t const *data, size_t n) {
  if (!running) {
    failed = failed || !write(ctx, data, n);
    return !failed;
  }
  ScopedLock const held{ lock };
#ifdef SCAV_TESTING
  blocked += ((count == slots.size()) && !failed) ? 1U : 0U;
#endif
  while ((count == slots.size()) && !failed) { changed.wait(lock); }
  if (failed) { return false; }
  slots[(head + count) % slots.size()].assign(data, data + n);
  ++count;
  changed.notify_all();
  return true;
}

bool ChunkQueue::close() {
  if (running) {
    {
      ScopedLock const held{ lock };
      closing = true;
      changed.notify_all();
    }
    writer.join();
    running = false;
  }
  return !failed;
}

#ifdef SCAV_TESTING
uint32_t ChunkQueue::test_blocked() {
  ScopedLock const held{ lock };
  return blocked;
}
#endif

// Writes the chunk at `head` with the lock released; the producer fills only free slots.
void ChunkQueue::drain(void *self) {
  ChunkQueue &q{ *static_cast<ChunkQueue *>(self) };
  q.lock.lock();
  for (;;) {
    while ((q.count == 0) && !q.closing) { q.changed.wait(q.lock); }
    if (q.count == 0) { break; }
    std::vector<uint8_t> &chunk{ q.slots[q.head] };
    bool const skip{ q.failed };
    q.lock.unlock();
    bool const wrote{ skip || q.write(q.ctx, chunk.data(), chunk.size()) };
    q.lock.lock();
    q.head = (q.head + 1U) % static_cast<uint32_t>(q.slots.size());
    --q.count;
    q.failed = q.failed || !wrote;
    q.changed.notify_all();
  }
  q.lock.unlock();
}

}  // namespace scav
