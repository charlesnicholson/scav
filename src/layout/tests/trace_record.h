#ifndef SCAV_LAYOUT_TESTS_TRACE_RECORD_H_INCLUDED
#define SCAV_LAYOUT_TESTS_TRACE_RECORD_H_INCLUDED

// A trace streamed into memory, for tests.

#include "layout/trace.h"
#include "layout/trace_stream.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scav {

// A sink that streams as a traced run does, headed with chart `c`'s state names;
// `events()` and `json()` end the stream and read it back.
struct TraceRecord : LayoutTrace {
  std::vector<uint8_t> bytes;  // the stream so far
  bool ended{ false };

  explicit TraceRecord(Chart const &c = Chart{}) {
    sink = [](void *to, uint8_t const *data, size_t n) {
      auto &out{ *static_cast<std::vector<uint8_t> *>(to) };
      out.insert(out.end(), data, data + n);
      return true;
    };
    ctx = &bytes;
    trace_begin(*this, c);
  }
  TraceRecord(TraceRecord const &) = delete;
  TraceRecord &operator=(TraceRecord const &) = delete;

  void end() {
    if (!ended) { trace_end(*this); }
    ended = true;
  }

  // Every event streamed, decoded.
  std::vector<TraceEvent> events() {
    end();
    std::vector<TraceEvent> out;
    TraceDecoder d;
    trace_decode_events(
        d,
        bytes.data(),
        bytes.size(),
        [](void *to, TraceEvent const &e) {
          static_cast<std::vector<TraceEvent> *>(to)->push_back(e);
          return true;
        },
        &out);
    return out;
  }

  // The JSON `scav dump --trace` prints for the stream; empty when it does not decode
  // whole.
  std::vector<char> json() {
    end();
    std::vector<char> out;
    TraceDecoder d;
    d.write = [](void *to, char const *text, size_t n) {
      auto &json{ *static_cast<std::vector<char> *>(to) };
      json.insert(json.end(), text, text + n);
      return true;
    };
    d.ctx = &out;
    if (!trace_decode(d, bytes.data(), bytes.size()) || !trace_decode_end(d)) {
      out.clear();
    }
    return out;
  }
};

}  // namespace scav

#endif  // SCAV_LAYOUT_TESTS_TRACE_RECORD_H_INCLUDED
