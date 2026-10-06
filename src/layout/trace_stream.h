#ifndef SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED
#define SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED

// The trace's binary encoding. A stream is a header naming every kind, its fields and
// their types, and the chart's states; then one record per event; then an end record.

#include "layout/trace.h"
#include "scav/scav_core.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scav {

inline constexpr uint32_t TRACE_FORMAT{ 1 };      // the format version after the magic
inline constexpr uint8_t TRACE_END{ 0xFF };       // the end record's kind byte
inline constexpr size_t TRACE_RECORD_MAX{ 128 };  // bytes in the longest record

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

}  // namespace scav

#endif  // SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED
