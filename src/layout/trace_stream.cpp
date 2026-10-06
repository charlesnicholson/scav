// The trace's binary encoding: one per-kind field table drives the schema, the encoder
// and the decoder.

#include "layout/trace_stream.h"

#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_vec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace scav {

namespace {

constexpr std::array<uint8_t, 8> MAGIC{ 's', 'c', 'a', 'v', '-', 't', 'r', 'c' };

// A field's encoding: `U16` a varint; `U32` a varint of the value plus one modulo 2^32,
// so INVALID is 0; `I32` and `I64` zigzag varints.
enum class TraceType : uint8_t { U16 = 1, U32 = 2, I32 = 3, I64 = 4 };

// Calls `f(name, field)` for each field `e.kind` carries, `pass` and `frame` first.
template <typename Event, typename F>
void each_field(Event &e, F &&f) {
  f("pass", e.pass);
  f("frame", e.frame);
  switch (e.kind) {
    case TraceKind::None: break;
    case TraceKind::RankAssigned:
    case TraceKind::RankPinned:
    case TraceKind::PseudostateSeated:
      f("state", e.rank.state);
      f("rank", e.rank.rank);
      break;
    case TraceKind::EdgeReversed:
    case TraceKind::RouteDegraded:
    case TraceKind::RouteWalled:
    case TraceKind::LabelCentred:
    case TraceKind::RouteReseated:
    case TraceKind::RouteCrossed: f("seg", e.seg.seg); break;
    case TraceKind::EdgeChained:
      f("seg", e.chain.seg);
      f("rank", e.chain.rank);
      f("index", e.chain.index);
      f("count", e.chain.count);
      break;
    case TraceKind::NodePlaced:
      f("state", e.place.state);
      f("seg", e.place.seg);
      f("x", e.place.x);
      f("y", e.place.y);
      break;
    case TraceKind::SpacingInflated:
      f("node_sep", e.inflate.node_sep);
      f("rank_sep", e.inflate.rank_sep);
      break;
    case TraceKind::NetPlanned:
      f("seg", e.net.seg);
      f("trans", e.net.trans);
      f("waypoints", e.net.waypoints);
      f("sx", e.net.sx);
      f("sy", e.net.sy);
      f("dx", e.net.dx);
      f("dy", e.net.dy);
      break;
    case TraceKind::NetWaypoint:
      f("x", e.point.x);
      f("y", e.point.y);
      break;
    case TraceKind::SeatMoved:
      f("net", e.seat.net);
      f("end", e.seat.end);
      f("from_x", e.seat.from_x);
      f("from_y", e.seat.from_y);
      f("to_x", e.seat.to_x);
      f("to_y", e.seat.to_y);
      break;
    case TraceKind::LaneAssigned:
      f("net", e.lane.net);
      f("lane", e.lane.lane);
      f("at", e.lane.at);
      break;
    case TraceKind::LaneFound:
      f("horizontal", e.found.horizontal);
      f("at", e.found.at);
      f("members", e.found.members);
      f("bundles", e.found.bundles);
      f("merged", e.found.merged);
      f("reordered", e.found.reordered);
      f("spread", e.found.spread);
      break;
    case TraceKind::BundleRefused:
      f("net", e.bundle.net);
      f("lane", e.bundle.lane);
      f("members", e.bundle.members);
      f("to", e.bundle.to);
      break;
    case TraceKind::CandidateScored:
      f("row", e.score.row);
      f("state", e.score.state);
      f("rank", e.score.rank);
      f("trans", e.score.trans);
      f("leg", e.score.leg);
      f("move", e.score.move);
      f("end", e.score.end);
      f("face", e.score.face);
      f("t0", e.score.t0);
      f("t2", e.score.t2);
      break;
    case TraceKind::CandidateTerms: f("share", e.terms.share); break;
    case TraceKind::FoldCut:
    case TraceKind::FoldPinned:
      f("rank", e.fold.rank);
      f("refused", e.fold.refused);
      f("carried", e.fold.carried);
      break;
    case TraceKind::PortTurned:
    case TraceKind::PortWalled:
    case TraceKind::LoopFaced:
      f("seg", e.port.seg);
      f("trans", e.port.trans);
      f("leg", e.port.leg);
      f("side", e.port.side);
      break;
    case TraceKind::PortAttached:
    case TraceKind::ColumnCentred:
      f("state", e.shift.state);
      f("seg", e.shift.seg);
      f("by", e.shift.by);
      break;
    case TraceKind::PiecePacked:
      f("rank", e.piece.rank);
      f("x", e.piece.x);
      f("y", e.piece.y);
      f("w", e.piece.w);
      f("h", e.piece.h);
      f("carried", e.piece.carried);
      break;
    case TraceKind::GapCharged:
      f("boundary", e.gap.boundary);
      f("seg", e.gap.seg);
      f("width", e.gap.width);
      break;
    case TraceKind::BoundaryCarried:
      f("seg", e.carry.seg);
      f("rank", e.carry.rank);
      break;
    case TraceKind::RowSearched:
    case TraceKind::RowRepeated:
    case TraceKind::KickScored:
    case TraceKind::KickTaken:
      f("row", e.search.row);
      f("of", e.search.of);
      f("move", e.search.move);
      f("trans", e.search.trans);
      f("leg", e.search.leg);
      f("t0", e.search.t0);
      f("framed_t0", e.search.framed_t0);
      f("t2", e.search.t2);
      f("framed", e.search.framed);
      break;
  }
}

void put_varint(std::vector<uint8_t> &out, uint64_t v) {
  while (v >= 0x80U) {
    vec_push_back(out, static_cast<uint8_t>(v | 0x80U));
    v >>= 7U;
  }
  vec_push_back(out, static_cast<uint8_t>(v));
}

uint64_t zigzag(int64_t v) {
  return (static_cast<uint64_t>(v) << 1U) ^ static_cast<uint64_t>(v >> 63);
}

int64_t unzigzag(uint64_t u) {
  return static_cast<int64_t>(u >> 1U) ^ -static_cast<int64_t>(u & 1U);
}

void put_text(std::vector<uint8_t> &out, char const *s, size_t n) {
  put_varint(out, n);
  vec_insert(out, out.end(), s, s + n);
}

// Appends each field's value.
struct Encode {
  std::vector<uint8_t> &out;
  void operator()(char const * /*name*/, uint16_t v) const { put_varint(out, v); }
  void operator()(char const * /*name*/, uint32_t v) const { put_varint(out, v + 1U); }
  void operator()(char const * /*name*/, int32_t v) const { put_varint(out, zigzag(v)); }
  void operator()(char const * /*name*/, int64_t v) const { put_varint(out, zigzag(v)); }
  template <size_t N>
  void operator()(char const *name, std::array<int32_t, N> const &a) const {
    for (int32_t const v : a) { (*this)(name, v); }
  }
};

// Appends each field's name, type and count, and counts the fields.
struct Describe {
  std::vector<uint8_t> &out;
  uint64_t fields{ 0 };
  void field(char const *name, TraceType type, uint64_t count) {
    put_text(out, name, std::strlen(name));
    vec_push_back(out, static_cast<uint8_t>(type));
    put_varint(out, count);
    ++fields;
  }
  void operator()(char const *name, uint16_t /*v*/) { field(name, TraceType::U16, 1); }
  void operator()(char const *name, uint32_t /*v*/) { field(name, TraceType::U32, 1); }
  void operator()(char const *name, int32_t /*v*/) { field(name, TraceType::I32, 1); }
  void operator()(char const *name, int64_t /*v*/) { field(name, TraceType::I64, 1); }
  template <size_t N>
  void operator()(char const *name, std::array<int32_t, N> const & /*a*/) {
    field(name, TraceType::I32, N);
  }
};

// The schema: the kind count, then per kind its name, field count and fields.
std::vector<uint8_t> const &schema() {
  static std::vector<uint8_t> const SCHEMA{ [] {
    std::vector<uint8_t> out;
    put_varint(out, TRACE_KINDS);
    std::vector<uint8_t> fields;
    for (uint32_t k = 0; k < TRACE_KINDS; ++k) {
      TraceEvent e{};
      e.kind = static_cast<TraceKind>(k);
      fields.clear();
      Describe d{ .out = fields, .fields = 0 };
      each_field(e, d);
      char const *const name{ trace_kind_name(e.kind) };
      put_text(out, name, std::strlen(name));
      put_varint(out, d.fields);
      vec_insert(out, out.end(), fields.begin(), fields.end());
    }
    return out;
  }() };
  return SCHEMA;
}

// Reads varints from `[at, end)`. `short_read` is set when the bytes run out and `bad`
// when a varint is malformed or past its field's range.
struct Reader {
  uint8_t const *at;
  uint8_t const *end;
  bool short_read{ false };
  bool bad{ false };

  bool varint(uint64_t &v) {
    v = 0;
    for (uint32_t shift = 0; shift < 64U; shift += 7U) {
      if (at == end) {
        short_read = true;
        return false;
      }
      uint8_t const b{ *at++ };
      if ((shift == 63U) && (b > 1U)) { break; }
      v |= uint64_t{ b & 0x7FU } << shift;
      if ((b & 0x80U) == 0) { return true; }
    }
    bad = true;
    return false;
  }

  // A varint no greater than `most`.
  bool bounded(uint64_t &v, uint64_t most) {
    if (!varint(v)) { return false; }
    bad = bad || (v > most);
    return !bad;
  }
};

// Reads each field's value.
struct Decode {
  Reader &in;
  void operator()(char const * /*name*/, uint16_t &v) const {
    uint64_t raw{ 0 };
    if (in.bounded(raw, UINT16_MAX)) { v = static_cast<uint16_t>(raw); }
  }
  void operator()(char const * /*name*/, uint32_t &v) const {
    uint64_t raw{ 0 };
    if (in.bounded(raw, UINT32_MAX)) { v = static_cast<uint32_t>(raw) - 1U; }
  }
  void operator()(char const * /*name*/, int32_t &v) const {
    uint64_t raw{ 0 };
    if (in.bounded(raw, UINT32_MAX)) { v = static_cast<int32_t>(unzigzag(raw)); }
  }
  void operator()(char const * /*name*/, int64_t &v) const {
    uint64_t raw{ 0 };
    if (in.varint(raw)) { v = unzigzag(raw); }
  }
  template <size_t N>
  void operator()(char const *name, std::array<int32_t, N> &a) const {
    for (int32_t &v : a) { (*this)(name, v); }
  }
};

// Parses the header at the front of `d.pending`; returns the bytes it took, 0 while the
// header is incomplete. Sets `d.failed` on a malformed one.
size_t decode_header(TraceDecoder &d) {
  Reader r{ .at = d.pending.data(), .end = d.pending.data() + d.pending.size() };
  if (d.pending.size() < MAGIC.size()) { return 0; }
  if (std::memcmp(d.pending.data(), MAGIC.data(), MAGIC.size()) != 0) {
    d.failed = true;
    return 0;
  }
  r.at += MAGIC.size();
  uint64_t version{ 0 };
  uint64_t length{ 0 };
  if (!r.varint(version) || !r.varint(length)) {
    d.failed = r.bad;
    return 0;
  }
  if ((version != TRACE_FORMAT) || (length > UINT32_MAX)) {
    d.failed = true;
    d.foreign = (version != TRACE_FORMAT);
    return 0;
  }
  if (std::cmp_greater(length, r.end - r.at)) { return 0; }
  Reader body{ .at = r.at, .end = r.at + length };
  std::vector<uint8_t> const &want{ schema() };
  if ((length < want.size()) || (std::memcmp(body.at, want.data(), want.size()) != 0)) {
    d.failed = true;
    d.foreign = true;
    return 0;
  }
  body.at += want.size();
  uint64_t states{ 0 };
  bool ok{ body.varint(states) };
  for (uint64_t s = 0; ok && (s < states); ++s) {
    uint64_t n{ 0 };
    ok = body.varint(n) && std::cmp_less_equal(n, body.end - body.at);
    if (ok) {
      char const *const name{ reinterpret_cast<char const *>(body.at) };
      vec_insert(d.names, d.names.end(), name, name + n);
      vec_push_back(d.name_end, static_cast<uint32_t>(d.names.size()));
      body.at += n;
    }
  }
  if (!ok || (body.at != body.end)) {
    d.failed = true;
    return 0;
  }
  d.header = true;
  return static_cast<size_t>(body.end - d.pending.data());
}

}  // namespace

void trace_encode_header(Chart const &c, std::vector<uint8_t> &out) {
  std::vector<uint8_t> body{ schema() };
  put_varint(body, c.states.size());
  for (State const &st : c.states) {
    auto const name{ chart_string(c, st.name) };
    put_text(body, name.data(), name.size());
  }
  vec_insert(out, out.end(), MAGIC.begin(), MAGIC.end());
  put_varint(out, TRACE_FORMAT);
  put_varint(out, body.size());
  vec_insert(out, out.end(), body.begin(), body.end());
}

void trace_encode(TraceEvent const &e, std::vector<uint8_t> &out) {
  vec_push_back(out, static_cast<uint8_t>(e.kind));
  each_field(e, Encode{ out });
}

void trace_encode_end(uint64_t events, std::vector<uint8_t> &out) {
  vec_push_back(out, TRACE_END);
  put_varint(out, events);
}

bool trace_decode_events(TraceDecoder &d,
                         uint8_t const *data,
                         size_t n,
                         TraceEventFn on,
                         void *ctx) {
  if (d.failed) { return false; }
  vec_insert(d.pending, d.pending.end(), data, data + n);
  size_t used{ d.header ? 0U : decode_header(d) };
  uint8_t const *const end{ d.pending.data() + d.pending.size() };
  while (d.header && !d.failed && (used < d.pending.size())) {
    if (d.ended) {
      d.failed = true;  // bytes after the end record
      break;
    }
    Reader r{ .at = d.pending.data() + used, .end = end };
    uint8_t const kind{ *r.at++ };
    if (kind == TRACE_END) {
      uint64_t count{ 0 };
      if (!r.varint(count)) {
        d.failed = r.bad;
        break;
      }
      d.failed = (count != d.events);
      d.ended = true;
    } else if (kind < TRACE_KINDS) {
      TraceEvent e{};
      e.kind = static_cast<TraceKind>(kind);
      each_field(e, Decode{ r });
      if (r.bad || r.short_read) {
        d.failed = r.bad;
        break;
      }
      ++d.events;
      d.failed = (on != nullptr) && !on(ctx, e);
    } else {
      d.failed = true;
    }
    used = static_cast<size_t>(r.at - d.pending.data());
  }
  d.pending.erase(d.pending.begin(), d.pending.begin() + static_cast<ptrdiff_t>(used));
  return !d.failed;
}

bool trace_decode_whole(TraceDecoder const &d) {
  return d.header && d.ended && !d.failed && d.pending.empty();
}

namespace {

// Writes the JSON held and empties it.
bool json_flush(TraceDecoder &d) {
  bool const wrote{ d.write(d.ctx, d.text.data(), d.text.size()) };
  d.text.clear();
  return wrote;
}

// A `TraceEventFn` appending the event's line after `[` or a separator, and writing once
// `TRACE_JSON_FLUSH` bytes are held.
bool json_event(void *ctx, TraceEvent const &e) {
  auto &d{ *static_cast<TraceDecoder *>(ctx) };
  uint64_t const i{ d.events - 1U };
  vec_insert(d.text, d.text.end(), { (i == 0) ? '[' : ',', '\n' });
  trace_event_json(e, i, d.names, d.name_end, d.text);
  return (d.text.size() < TRACE_JSON_FLUSH) || json_flush(d);
}

// Hands the chunk to the sink and empties it.
void trace_flush(LayoutTrace &t) {
  if (t.ok && !t.chunk.empty()) { t.ok = t.sink(t.ctx, t.chunk.data(), t.chunk.size()); }
  t.chunk.clear();
}

}  // namespace

bool trace_decode(TraceDecoder &d, uint8_t const *data, size_t n) {
  return trace_decode_events(d, data, n, (d.write != nullptr) ? json_event : nullptr, &d);
}

bool trace_decode_end(TraceDecoder &d) {
  if (!trace_decode_whole(d)) { return false; }
  if (d.write == nullptr) { return true; }
  char const *const tail{ (d.events == 0) ? "[\n]\n" : "\n]\n" };
  vec_insert(d.text, d.text.end(), tail, tail + std::strlen(tail));
  return json_flush(d);
}

void trace_begin(LayoutTrace &t, Chart const &c) {
  vec_reserve(t.chunk, TRACE_CHUNK + TRACE_RECORD_MAX);
  trace_encode_header(c, t.chunk);
  if (t.chunk.size() >= TRACE_CHUNK) { trace_flush(t); }
}

bool trace_end(LayoutTrace &t) {
  trace_encode_end(t.count, t.chunk);
  trace_flush(t);
  return t.ok;
}

void trace_put(LayoutTrace &t, TraceEvent const &e) {
  if (!t.ok) { return; }
  trace_encode(e, t.chunk);
  ++t.count;
  if (t.chunk.size() >= TRACE_CHUNK) { trace_flush(t); }
}

}  // namespace scav
