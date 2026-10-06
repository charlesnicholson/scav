#ifndef SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED
#define SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED

// The trace's binary encoding and its streaming. A stream is a header naming every kind,
// its fields and their types, and the chart's states; one record per event; an end record.

#include "layout/chunk_queue.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace scav {

inline constexpr uint32_t TRACE_FORMAT{ 1 };      // the format version after the magic
inline constexpr uint8_t TRACE_END{ 0xFF };       // the end record's kind byte
inline constexpr size_t TRACE_RECORD_MAX{ 128 };  // bytes in the longest record
inline constexpr size_t TRACE_CHUNK{ size_t{ 256 } << 10U };      // bytes per queued chunk
inline constexpr uint32_t TRACE_QUEUE_DEPTH{ 4 };                 // chunks a queue holds
inline constexpr size_t TRACE_JSON_FLUSH{ size_t{ 64 } << 10U };  // JSON held per write

// Appends the header: magic, version, the length of the rest, the schema, chart `c`'s
// state names.
void trace_encode_header(Chart const &c, std::vector<uint8_t> &out);

// Appends `e`'s record: its kind byte, then each field its kind carries as a varint.
void trace_encode(TraceEvent const &e, std::vector<uint8_t> &out);

// Appends the end record: `TRACE_END`, then the number of records before it.
void trace_encode_end(uint64_t events, std::vector<uint8_t> &out);

// Decodes a stream fed in pieces of any size.
struct TraceDecoder {
  std::vector<uint8_t> pending;     // fed bytes short of a whole record
  std::vector<std::string> states;  // the header's state names
  uint64_t events{ 0 };             // records decoded
  bool header{ false };
  bool ended{ false };
  bool failed{ false };
};

// Receives one decoded event; false fails the decode.
using TraceEventFn = bool (*)(void *ctx, TraceEvent const &e);

// Feeds `data[0..n)` and calls `on` for each record it completes; false once the stream
// is malformed or `on` fails.
bool trace_decode(TraceDecoder &d,
                  uint8_t const *data,
                  size_t n,
                  TraceEventFn on,
                  void *ctx);

// True when the bytes fed are a whole stream: header, records, end record, nothing after.
bool trace_decode_whole(TraceDecoder const &d);

// Encodes a run's events into chunks of about TRACE_CHUNK bytes for `queue`.
struct TraceWriter {
  ChunkQueue *queue{ nullptr };
  std::vector<uint8_t> chunk;
  uint64_t events{ 0 };
  bool ok{ true };  // every chunk was queued
};

// A file written as `<path>.tmp` and renamed onto `path` when committed.
struct AtomicFile {
  std::FILE *file{ nullptr };
  std::string path;
  std::string temp;
};

// Decodes a stream and writes each event's JSON line through `write`.
struct JsonOut {
  TraceDecoder decoder;
  std::vector<char> text;  // JSON not yet written
  TraceWrite write{ nullptr };
  void *ctx{ nullptr };
};

// A run's trace streamed through a writer thread: into the file `to.path`, or decoded
// to JSON through `to.write`.
class TraceStream {
 public:
  TraceStream(Chart const &c, TraceTo const &to);  // queues the header
  ~TraceStream();                                  // unfinished, it removes its temp file
  TraceStream(TraceStream const &) = delete;
  TraceStream &operator=(TraceStream const &) = delete;

  [[nodiscard]] bool opened() const { return open; }  // false when `to` cannot be written
  LayoutTrace &sink() { return trace; }               // streams each event put to it
  bool finish();  // ends the stream; true when the whole trace reached `to`

 private:
  AtomicFile file;
  JsonOut json;
  bool const to_file;
  bool const open;
  bool finished{ false };
  ChunkQueue queue;
  TraceWriter writer;
  LayoutTrace trace;
};

}  // namespace scav

#endif  // SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED
