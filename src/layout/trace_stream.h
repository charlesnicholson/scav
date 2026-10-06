#ifndef SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED
#define SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED

// The trace's binary encoding: a header naming every kind, its fields and their types,
// and the chart's states; one record per event; an end record.

#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scav {

inline constexpr uint32_t TRACE_FORMAT{ 1 };      // the format version after the magic
inline constexpr uint8_t TRACE_END{ 0xFF };       // the end record's kind byte
inline constexpr size_t TRACE_RECORD_MAX{ 128 };  // bytes in the longest record
inline constexpr size_t TRACE_CHUNK{ size_t{ 256 } << 10U };      // bytes per chunk
inline constexpr size_t TRACE_JSON_FLUSH{ size_t{ 64 } << 10U };  // JSON held per write

// Appends the header: magic, version, the length of the rest, the schema, chart `c`'s
// state names.
void trace_encode_header(Chart const &c, std::vector<uint8_t> &out);

// Appends `e`'s record: its kind byte, then each field its kind carries as a varint.
void trace_encode(TraceEvent const &e, std::vector<uint8_t> &out);

// Appends the end record: `TRACE_END`, then the number of records before it.
void trace_encode_end(uint64_t events, std::vector<uint8_t> &out);

// Receives one decoded event; false fails the decode.
using TraceEventFn = bool (*)(void *ctx, TraceEvent const &e);

// Feeds `data[0..n)` and calls `on`, when set, for each record it completes; false once
// the stream is malformed or `on` fails.
bool trace_decode_events(TraceDecoder &d,
                         uint8_t const *data,
                         size_t n,
                         TraceEventFn on,
                         void *ctx);

// True when the bytes fed are a whole stream: header, records, end record, nothing after.
bool trace_decode_whole(TraceDecoder const &d);

// Starts `t`'s stream with chart `c`'s header.
void trace_begin(LayoutTrace &t, Chart const &c);

// Ends `t`'s stream and hands over the rest; true when `t.sink` took every chunk.
bool trace_end(LayoutTrace &t);

}  // namespace scav

#endif  // SCAV_LAYOUT_TRACE_STREAM_H_INCLUDED
