// The trace's encoding, decoding and header; a run's chunks; the JSON a decoder writes;
// and traced runs, whose sink takes the stream their sinks record.

#include "layout/trace_stream.h"

#include "layout/tests/trace_record.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace scav;

// Field values by set; field `j` of an event made at set `s` takes set `s + j`.
struct Values {
  uint32_t u;
  int32_t i;
  int64_t l;
  uint16_t h;
};
constexpr std::array<Values, 7> VALUES{ {
    { .u = 0, .i = 0, .l = 0, .h = 0 },
    { .u = INVALID, .i = -1, .l = -1, .h = 0xFFFF },
    { .u = 1, .i = -2, .l = -3, .h = 1 },
    { .u = INVALID - 1U,
      .i = std::numeric_limits<int32_t>::min(),
      .l = std::numeric_limits<int64_t>::min(),
      .h = 0x8000 },
    { .u = 0x7FFFFFFFU,
      .i = std::numeric_limits<int32_t>::max(),
      .l = std::numeric_limits<int64_t>::max(),
      .h = 0x7FFF },
    { .u = 127, .i = 64, .l = int64_t{ 4294967296 }, .h = 128 },
    { .u = 128, .i = -65, .l = -int64_t{ 4294967297 }, .h = 127 },
} };

// Calls `f` on `pass`, `frame`, then each field of the union member `e.kind` carries.
template <typename F>
void each_field(TraceEvent &e, F &&f) {
  f(e.pass);
  f(e.frame);
  switch (e.kind) {
    case TraceKind::None: break;
    case TraceKind::RankAssigned:
    case TraceKind::RankPinned:
    case TraceKind::PseudostateSeated:
      f(e.rank.state);
      f(e.rank.rank);
      break;
    case TraceKind::EdgeReversed:
    case TraceKind::RouteDegraded:
    case TraceKind::RouteWalled:
    case TraceKind::LabelCentred:
    case TraceKind::RouteReseated:
    case TraceKind::RouteCrossed: f(e.seg.seg); break;
    case TraceKind::EdgeChained:
      f(e.chain.seg);
      f(e.chain.rank);
      f(e.chain.index);
      f(e.chain.count);
      break;
    case TraceKind::NodePlaced:
      f(e.place.state);
      f(e.place.seg);
      f(e.place.x);
      f(e.place.y);
      break;
    case TraceKind::SpacingInflated:
      f(e.inflate.node_sep);
      f(e.inflate.rank_sep);
      break;
    case TraceKind::NetPlanned:
      f(e.net.seg);
      f(e.net.trans);
      f(e.net.waypoints);
      f(e.net.sx);
      f(e.net.sy);
      f(e.net.dx);
      f(e.net.dy);
      break;
    case TraceKind::NetWaypoint:
      f(e.point.x);
      f(e.point.y);
      break;
    case TraceKind::SeatMoved:
      f(e.seat.net);
      f(e.seat.end);
      f(e.seat.from_x);
      f(e.seat.from_y);
      f(e.seat.to_x);
      f(e.seat.to_y);
      break;
    case TraceKind::LaneAssigned:
      f(e.lane.net);
      f(e.lane.lane);
      f(e.lane.at);
      break;
    case TraceKind::LaneFound:
      f(e.found.horizontal);
      f(e.found.at);
      f(e.found.members);
      f(e.found.bundles);
      f(e.found.merged);
      f(e.found.reordered);
      f(e.found.spread);
      break;
    case TraceKind::BundleRefused:
      f(e.bundle.net);
      f(e.bundle.lane);
      f(e.bundle.members);
      f(e.bundle.to);
      break;
    case TraceKind::CandidateScored:
      f(e.score.row);
      f(e.score.state);
      f(e.score.rank);
      f(e.score.trans);
      f(e.score.leg);
      f(e.score.move);
      f(e.score.end);
      f(e.score.face);
      f(e.score.t0);
      f(e.score.t2);
      break;
    case TraceKind::CandidateTerms:
      for (int32_t &share : e.terms.share) { f(share); }
      break;
    case TraceKind::FoldCut:
    case TraceKind::FoldPinned:
      f(e.fold.rank);
      f(e.fold.refused);
      f(e.fold.carried);
      break;
    case TraceKind::PortTurned:
    case TraceKind::PortWalled:
    case TraceKind::LoopFaced:
      f(e.port.seg);
      f(e.port.trans);
      f(e.port.leg);
      f(e.port.side);
      break;
    case TraceKind::PortAttached:
    case TraceKind::ColumnCentred:
      f(e.shift.state);
      f(e.shift.seg);
      f(e.shift.by);
      break;
    case TraceKind::PiecePacked:
      f(e.piece.rank);
      f(e.piece.x);
      f(e.piece.y);
      f(e.piece.w);
      f(e.piece.h);
      f(e.piece.carried);
      break;
    case TraceKind::GapCharged:
      f(e.gap.boundary);
      f(e.gap.seg);
      f(e.gap.width);
      break;
    case TraceKind::BoundaryCarried:
      f(e.carry.seg);
      f(e.carry.rank);
      break;
    case TraceKind::RowSearched:
    case TraceKind::RowRepeated:
    case TraceKind::KickScored:
    case TraceKind::KickTaken:
      f(e.search.row);
      f(e.search.of);
      f(e.search.move);
      f(e.search.trans);
      f(e.search.leg);
      f(e.search.state);
      f(e.search.seats_w);
      f(e.search.seats_h);
      f(e.search.t0);
      f(e.search.framed_t0);
      f(e.search.t2);
      f(e.search.framed);
      break;
    case TraceKind::StateGrown:
      f(e.grow.state);
      f(e.grow.ends);
      f(e.grow.seats_w);
      f(e.grow.seats_h);
      f(e.grow.from_w);
      f(e.grow.from_h);
      f(e.grow.to_w);
      f(e.grow.to_h);
      break;
  }
}

// Kind `k` with every field from `VALUES`, starting at set `set`.
TraceEvent make(uint32_t k, uint32_t set) {
  TraceEvent e{};
  e.kind = static_cast<TraceKind>(k);
  uint32_t next{ set };
  each_field(e, [&next](auto &field) {
    Values const &v{ VALUES[next % VALUES.size()] };
    ++next;
    using T = std::remove_reference_t<decltype(field)>;
    if constexpr (std::is_same_v<T, uint16_t>) { field = v.h; }
    if constexpr (std::is_same_v<T, uint32_t>) { field = v.u; }
    if constexpr (std::is_same_v<T, int32_t>) { field = v.i; }
    if constexpr (std::is_same_v<T, int64_t>) { field = v.l; }
  });
  return e;
}

// The kind, then every field in `each_field` order, as 64-bit values.
std::vector<int64_t> values(TraceEvent e) {
  std::vector<int64_t> out{ static_cast<int64_t>(e.kind) };
  each_field(e, [&out](auto &field) { out.push_back(int64_t{ field }); });
  return out;
}

// Every kind at every value set, in order.
std::vector<TraceEvent> every_event() {
  std::vector<TraceEvent> out;
  for (uint32_t set = 0; set < VALUES.size(); ++set) {
    for (uint32_t k = 0; k < TRACE_KINDS; ++k) { out.push_back(make(k, set)); }
  }
  return out;
}

// A chart with a named state, a nameless one and a third.
Chart named_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "Alpha", StateKind::Normal, {});
  build_state(c, root, "", StateKind::Initial, {});
  build_state(c, root, "Gamma", StateKind::Normal, {});
  return c;
}

// The whole stream for `events` on chart `c`.
std::vector<uint8_t> stream_of(Chart const &c, std::vector<TraceEvent> const &events) {
  std::vector<uint8_t> out;
  trace_encode_header(c, out);
  for (TraceEvent const &e : events) { trace_encode(e, out); }
  trace_encode_end(events.size(), out);
  return out;
}

bool collect(void *ctx, TraceEvent const &e) {
  static_cast<std::vector<TraceEvent> *>(ctx)->push_back(e);
  return true;
}

// Decodes `bytes` fed `piece` bytes at a time; false when the decoder refuses them or the
// stream is not whole at the end.
bool decode_in_pieces(std::vector<uint8_t> const &bytes,
                      size_t piece,
                      std::vector<TraceEvent> &out,
                      TraceDecoder &d) {
  for (size_t at = 0; at < bytes.size(); at += piece) {
    size_t const n{ (bytes.size() - at < piece) ? (bytes.size() - at) : piece };
    if (!trace_decode_events(d, bytes.data() + at, n, collect, &out)) { return false; }
  }
  return trace_decode_whole(d);
}

bool decodes(std::vector<uint8_t> const &bytes) {
  TraceDecoder d;
  std::vector<TraceEvent> out;
  return decode_in_pieces(bytes, bytes.size() + 1U, out, d);
}

}  // namespace

TEST_CASE("trace stream: TRACE_KINDS is one past the last kind") {
  CHECK(std::string_view{ trace_kind_name(static_cast<TraceKind>(TRACE_KINDS - 1U)) } !=
        "none");
  CHECK(std::string_view{ trace_kind_name(static_cast<TraceKind>(TRACE_KINDS)) } ==
        "none");
}

TEST_CASE("trace stream: every kind round-trips at zero, INVALID, negative and extremes") {
  Chart const c{ named_chart() };
  for (TraceEvent const &want : every_event()) {
    std::string const kind{ trace_kind_name(want.kind) };
    CAPTURE(kind);
    std::vector<TraceEvent> got;
    TraceDecoder d;
    REQUIRE(decode_in_pieces(stream_of(c, { want }), 1U << 20U, got, d));
    REQUIRE(got.size() == 1U);
    CHECK(values(got[0]) == values(want));
  }
}

TEST_CASE("trace stream: no record outgrows TRACE_RECORD_MAX") {
  for (TraceEvent const &e : every_event()) {
    std::vector<uint8_t> out;
    trace_encode(e, out);
    CHECK(out.size() <= TRACE_RECORD_MAX);
  }
}

TEST_CASE("trace stream: a stream fed in pieces of any size decodes the same events") {
  Chart const c{ named_chart() };
  std::vector<TraceEvent> const want{ every_event() };
  std::vector<uint8_t> const bytes{ stream_of(c, want) };
  for (size_t const piece : { size_t{ 1 },
                              size_t{ 2 },
                              size_t{ 7 },
                              size_t{ 64 },
                              size_t{ 4093 },
                              bytes.size() }) {
    CAPTURE(piece);
    std::vector<TraceEvent> got;
    TraceDecoder d;
    REQUIRE(decode_in_pieces(bytes, piece, got, d));
    REQUIRE(got.size() == want.size());
    for (size_t k = 0; k < want.size(); ++k) { CHECK(values(got[k]) == values(want[k])); }
    CHECK(d.events == want.size());
  }
}

TEST_CASE("trace stream: the header carries every state's name, nameless ones empty") {
  Chart const c{ named_chart() };
  TraceDecoder d;
  std::vector<TraceEvent> got;
  REQUIRE(decode_in_pieces(stream_of(c, {}), 3U, got, d));
  CHECK(got.empty());
  CHECK(std::string_view{ d.names.data(), d.names.size() } == "AlphaGamma");
  CHECK(d.name_end == std::vector<uint32_t>{ 5, 5, 10 });
}

TEST_CASE("trace stream: a header another build wrote is told apart from a broken one") {
  Chart const c{ named_chart() };
  std::vector<uint8_t> const good{ stream_of(c, every_event()) };
  std::string_view const text{ reinterpret_cast<char const *>(good.data()), good.size() };
  size_t const schema{ text.find("none") };
  REQUIRE(schema != std::string_view::npos);
  struct Case {
    char const *what;
    size_t at;
    uint8_t flip;
    size_t keep;
    bool foreign;
  };
  size_t const all{ good.size() };
  for (Case const &k :
       { Case{ .what = "version", .at = 8, .flip = 1, .keep = all, .foreign = true },
         Case{ .what = "schema",
               .at = schema,
               .flip = 0x20,
               .keep = all,
               .foreign = true },
         Case{ .what = "magic", .at = 0, .flip = 1, .keep = all, .foreign = false },
         Case{ .what = "cut", .at = 0, .flip = 0, .keep = all - 1U, .foreign = false } }) {
    std::string const what{ k.what };
    CAPTURE(what);
    std::vector<uint8_t> bad{ good.begin(),
                              good.begin() + static_cast<ptrdiff_t>(k.keep) };
    bad[k.at] = static_cast<uint8_t>(bad[k.at] ^ k.flip);
    TraceDecoder d;
    bool const whole{ trace_decode(d, bad.data(), bad.size()) && trace_decode_end(d) };
    CHECK_FALSE(whole);
    CHECK(d.foreign == k.foreign);
  }
}

TEST_CASE("trace stream: a malformed or partial stream is refused") {
  Chart const c{ named_chart() };
  std::vector<uint8_t> const good{ stream_of(c, every_event()) };
  REQUIRE(decodes(good));

  std::vector<uint8_t> header;
  trace_encode_header(c, header);
  std::vector<uint8_t> record;
  trace_encode(make(static_cast<uint32_t>(TraceKind::NetPlanned), 2), record);

  SUBCASE("a wrong magic") {
    std::vector<uint8_t> bad{ good };
    bad[0] = static_cast<uint8_t>(bad[0] ^ 1U);
    CHECK_FALSE(decodes(bad));
  }
  SUBCASE("another format version") {
    std::vector<uint8_t> bad{ good };
    bad[8] = static_cast<uint8_t>(TRACE_FORMAT + 1U);
    CHECK_FALSE(decodes(bad));
  }
  SUBCASE("a schema that differs from this build's") {
    // The first kind's name, `none`, starts after the magic, version, length and count.
    std::vector<uint8_t> bad{ good };
    size_t const name{
      std::string_view{ reinterpret_cast<char const *>(bad.data()), bad.size() }.find(
          "none")
    };
    REQUIRE(name != std::string_view::npos);
    bad[name] = 'N';
    CHECK_FALSE(decodes(bad));
  }
  SUBCASE("a stream cut anywhere") {
    for (size_t n = 0; n < good.size(); n += 97U) {
      CHECK_FALSE(decodes({ good.begin(), good.begin() + static_cast<ptrdiff_t>(n) }));
    }
    CHECK_FALSE(decodes({ good.begin(), good.end() - 1 }));
  }
  SUBCASE("a kind byte past the last kind") {
    std::vector<uint8_t> bad{ header };
    bad.push_back(static_cast<uint8_t>(TRACE_KINDS));
    trace_encode_end(1, bad);
    CHECK_FALSE(decodes(bad));
  }
  SUBCASE("a varint past its field's range") {
    // A `pass` of 2^16 + 1 in place of a valid one.
    std::vector<uint8_t> bad{ header };
    bad.insert(
        bad.end(),
        { static_cast<uint8_t>(TraceKind::EdgeReversed), 0x81, 0x80, 0x04, 0x00, 0x00 });
    trace_encode_end(1, bad);
    CHECK_FALSE(decodes(bad));
  }
  SUBCASE("a varint longer than ten bytes") {
    std::vector<uint8_t> bad{ header };
    bad.push_back(static_cast<uint8_t>(TraceKind::SpacingInflated));
    bad.insert(bad.end(), { 0x00, 0x00 });  // pass, frame
    bad.insert(bad.end(), 10U, 0x80);       // node_sep runs on
    bad.insert(bad.end(), { 0x00, 0x00 });
    trace_encode_end(1, bad);
    CHECK_FALSE(decodes(bad));
  }
  SUBCASE("an end record that miscounts") {
    std::vector<uint8_t> bad{ header };
    bad.insert(bad.end(), record.begin(), record.end());
    trace_encode_end(2, bad);
    CHECK_FALSE(decodes(bad));
  }
  SUBCASE("bytes after the end record") {
    std::vector<uint8_t> bad{ good };
    bad.push_back(0);
    CHECK_FALSE(decodes(bad));
  }
  SUBCASE("no end record") {
    std::vector<uint8_t> bad{ header };
    bad.insert(bad.end(), record.begin(), record.end());
    CHECK_FALSE(decodes(bad));
  }
}

namespace {

// A reader written from the format alone: the header names each kind's fields and types,
// and those decode each record into named values.
struct Generic {
  struct Field {
    std::string name;
    uint8_t type{ 0 };
    uint64_t count{ 0 };
  };
  struct Kind {
    std::string name;
    std::vector<Field> fields;
  };
  struct Record {
    std::string kind;
    std::vector<std::string> names;  // a field of count n repeats its name n times
    std::vector<int64_t> values;
  };
  std::vector<Kind> kinds;
  std::vector<std::string> states;
  std::vector<Record> records;
  uint64_t ended_at{ 0 };  // the end record's count

  std::vector<uint8_t> const &in;
  size_t at{ 0 };

  uint64_t varint() {
    uint64_t v{ 0 };
    for (uint32_t shift = 0;; shift += 7U) {
      uint8_t const b{ in.at(at++) };
      v |= uint64_t{ b & 0x7FU } << shift;
      if ((b & 0x80U) == 0) { return v; }
    }
  }
  std::string text() {
    uint64_t const n{ varint() };
    std::string out{ reinterpret_cast<char const *>(in.data() + at), n };
    at += n;
    return out;
  }
  // Type 1 is a plain varint, 2 a varint of the value plus one, 3 and 4 zigzag varints.
  int64_t value(uint8_t type) {
    uint64_t const raw{ varint() };
    if (type == 2) { return static_cast<int64_t>(static_cast<uint32_t>(raw - 1U)); }
    if ((type == 3) || (type == 4)) {
      return static_cast<int64_t>(raw >> 1U) ^ -static_cast<int64_t>(raw & 1U);
    }
    return static_cast<int64_t>(raw);
  }

  explicit Generic(std::vector<uint8_t> const &bytes) : in(bytes) {
    at = 8;  // the magic
    REQUIRE(varint() == TRACE_FORMAT);
    uint64_t const length{ varint() };
    size_t const records_at{ at + length };
    uint64_t const n_kinds{ varint() };
    for (uint64_t k = 0; k < n_kinds; ++k) {
      Kind kind{ .name = text(), .fields = {} };
      uint64_t const n_fields{ varint() };
      for (uint64_t f = 0; f < n_fields; ++f) {
        Field field{ .name = text(), .type = in.at(at++), .count = 0 };
        field.count = varint();
        kind.fields.push_back(field);
      }
      kinds.push_back(kind);
    }
    uint64_t const n_states{ varint() };
    for (uint64_t s = 0; s < n_states; ++s) { states.push_back(text()); }
    REQUIRE(at == records_at);
    for (;;) {
      uint8_t const k{ in.at(at++) };
      if (k == 0xFF) {
        ended_at = varint();
        break;
      }
      Record r{ .kind = kinds.at(k).name, .names = {}, .values = { k } };
      for (Field const &f : kinds.at(k).fields) {
        for (uint64_t n = 0; n < f.count; ++n) {
          r.names.push_back(f.name);
          r.values.push_back(value(f.type));
        }
      }
      records.push_back(r);
    }
    CHECK(at == in.size());
  }
};

}  // namespace

TEST_CASE("trace stream: the header alone decodes every record") {
  Chart const c{ named_chart() };
  std::vector<TraceEvent> const events{ every_event() };
  std::vector<uint8_t> const bytes{ stream_of(c, events) };
  Generic const g{ bytes };
  CHECK(g.kinds.size() == TRACE_KINDS);
  CHECK(g.states == std::vector<std::string>{ "Alpha", "", "Gamma" });
  CHECK(g.ended_at == events.size());
  REQUIRE(g.records.size() == events.size());
  for (size_t k = 0; k < events.size(); ++k) {
    CHECK(g.records[k].kind == trace_kind_name(events[k].kind));
    CHECK(g.records[k].values == values(events[k]));
  }
  // Fields are named as the JSON names them.
  TraceEvent const scored{ make(static_cast<uint32_t>(TraceKind::CandidateScored), 0) };
  for (size_t k = 0; k < events.size(); ++k) {
    if (events[k].kind != scored.kind) { continue; }
    CHECK(g.records[k].names == std::vector<std::string>{ "pass",
                                                          "frame",
                                                          "row",
                                                          "state",
                                                          "rank",
                                                          "trans",
                                                          "leg",
                                                          "move",
                                                          "end",
                                                          "face",
                                                          "t0",
                                                          "t2" });
    break;
  }
}

namespace {

// Every kind at every value set, repeated until its records pass `bytes`.
std::vector<TraceEvent> events_past(size_t bytes) {
  std::vector<TraceEvent> const one{ every_event() };
  size_t const each{ stream_of(Chart{}, one).size() - stream_of(Chart{}, {}).size() };
  std::vector<TraceEvent> out;
  for (size_t k = 0; k * each < bytes; ++k) {
    out.insert(out.end(), one.begin(), one.end());
  }
  return out;
}

// Past two chunks of stream.
std::vector<TraceEvent> many_events() { return events_past((5U * TRACE_CHUNK) / 2U); }

// The chunks a sink took, and the one it refuses; 0 refuses none.
struct Chunks {
  std::vector<std::vector<uint8_t>> taken;
  uint32_t refuse{ 0 };
};

bool take_chunk(void *ctx, uint8_t const *data, size_t n) {
  auto &c{ *static_cast<Chunks *>(ctx) };
  if (c.taken.size() + 1U == c.refuse) { return false; }
  c.taken.emplace_back(data, data + n);
  return true;
}

// Everything `layout_trace`'s stream would carry for `events` on chart `c`, in chunks.
Chunks streamed(Chart const &c, std::vector<TraceEvent> const &events, uint32_t refuse) {
  Chunks out{ .taken = {}, .refuse = refuse };
  LayoutTrace t;
  t.sink = take_chunk;
  t.ctx = &out;
  trace_begin(t, c);
  for (TraceEvent const &e : events) { trace_put(t, e); }
  CHECK(trace_end(t) == (refuse == 0));
  return out;
}

// The JSON a decoder writes for `bytes` fed `piece` bytes at a time, and each write's
// size; empty when the decode fails.
std::string json_in_pieces(std::vector<uint8_t> const &bytes,
                           size_t piece,
                           std::vector<size_t> *writes = nullptr) {
  struct Out {
    std::string text;
    std::vector<size_t> *writes;
  };
  Out out{ .text = {}, .writes = writes };
  TraceDecoder d;
  d.write = [](void *ctx, char const *text, size_t n) {
    auto &o{ *static_cast<Out *>(ctx) };
    o.text.append(text, n);
    if (o.writes != nullptr) { o.writes->push_back(n); }
    return true;
  };
  d.ctx = &out;
  for (size_t at = 0; at < bytes.size(); at += piece) {
    size_t const n{ (bytes.size() - at < piece) ? (bytes.size() - at) : piece };
    if (!trace_decode(d, bytes.data() + at, n)) { return {}; }
  }
  return trace_decode_end(d) ? out.text : std::string{};
}

}  // namespace

TEST_CASE(
    "trace stream: a run's stream reaches its sink whole, in chunks of TRACE_CHUNK") {
  Chart const c{ named_chart() };
  std::vector<TraceEvent> const events{ many_events() };
  Chunks const got{ streamed(c, events, 0) };
  REQUIRE(got.taken.size() >= 3U);
  std::vector<uint8_t> joined;
  for (size_t k = 0; k < got.taken.size(); ++k) {
    CAPTURE(k);
    if ((k + 1U) < got.taken.size()) {
      CHECK(got.taken[k].size() >= TRACE_CHUNK);
      CHECK(got.taken[k].size() < TRACE_CHUNK + TRACE_RECORD_MAX);
    }
    joined.insert(joined.end(), got.taken[k].begin(), got.taken[k].end());
  }
  CHECK(joined == stream_of(c, events));
}

TEST_CASE("trace stream: a sink that refuses a chunk is handed no more") {
  Chart const c{ named_chart() };
  Chunks const got{ streamed(c, many_events(), 2) };
  CHECK(got.taken.size() == 1U);
}

TEST_CASE("trace stream: a decoder writes one JSON array, whatever the pieces it is fed") {
  Chart const c{ named_chart() };
  std::vector<uint8_t> const bytes{ stream_of(c, every_event()) };
  std::string const whole{ json_in_pieces(bytes, bytes.size()) };
  REQUIRE(whole.starts_with("[\n  {\"i\":0,\"kind\":\"none\""));
  CHECK(whole.ends_with("}\n]\n"));
  CHECK(whole.find("\"state\":\"Alpha\"") != std::string::npos);
  CHECK(whole.find("\"state\":\"#1\"") != std::string::npos);
  for (size_t const piece : { size_t{ 1 }, size_t{ 7 }, size_t{ 4093 } }) {
    CAPTURE(piece);
    CHECK(json_in_pieces(bytes, piece) == whole);
  }
  CHECK(json_in_pieces(stream_of(c, {}), 3) == "[\n]\n");
}

TEST_CASE("trace stream: a decoder writes in pieces of about TRACE_JSON_FLUSH") {
  Chart const c{ named_chart() };
  std::vector<uint8_t> const bytes{ stream_of(c, events_past(TRACE_JSON_FLUSH / 2U)) };
  std::vector<size_t> writes;
  std::string const json{ json_in_pieces(bytes, TRACE_CHUNK, &writes) };
  REQUIRE(writes.size() >= 3U);
  for (size_t k = 0; (k + 1U) < writes.size(); ++k) {
    CAPTURE(k);
    CHECK(writes[k] >= TRACE_JSON_FLUSH);
    CHECK(writes[k] < TRACE_JSON_FLUSH + 4096U);
  }
  CHECK(json == json_in_pieces(bytes, bytes.size()));
}

TEST_CASE("trace stream: a decoder with no write checks the stream and writes nothing") {
  Chart const c{ named_chart() };
  std::vector<uint8_t> const good{ stream_of(c, every_event()) };
  for (size_t const n : { good.size(), good.size() - 1U, good.size() / 2U }) {
    CAPTURE(n);
    TraceDecoder d;
    bool const fed{ trace_decode(d, good.data(), n) };
    CHECK(fed);
    CHECK(trace_decode_end(d) == (n == good.size()));
    CHECK(d.text.empty());
  }
}

TEST_CASE("trace stream: a decoder whose write fails fails the decode") {
  Chart const c{ named_chart() };
  std::vector<uint8_t> const bytes{ stream_of(c, events_past(TRACE_JSON_FLUSH / 2U)) };
  TraceDecoder d;
  d.write = [](void * /*ctx*/, char const * /*text*/, size_t /*n*/) { return false; };
  CHECK_FALSE(trace_decode(d, bytes.data(), bytes.size()));
  CHECK_FALSE(trace_decode_end(d));
}

namespace {

Chart gauntlet(char const *name) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/gauntlet/" };
  path += name;
  Loader loader;
  Chart c;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
  return c;
}

// The stream `layout_trace` hands its sink for `scope`, recorded with the sinks set
// directly.
std::vector<uint8_t> recorded(Chart &c, scav_layout_opts const &o, TraceScope scope) {
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_layout_opts serial{ o };
  serial.threads = 1;
  TraceRecord t{ c };
  if (scope == TraceScope::Shipped) {
    uint32_t won{ 0 };
    SearchPins taken;
    REQUIRE(layout_run(c, {}, o, placed, diags, nullptr, &won, INVALID, nullptr, &taken));
    serial.profile.portfolio_k = 0;
    trace_sink_set(&t);
    CHECK(layout_run(c,
                     {},
                     serial,
                     placed,
                     diags,
                     nullptr,
                     nullptr,
                     won,
                     nullptr,
                     nullptr,
                     &taken));
  } else {
    if (scope == TraceScope::Outline) {
      trace_outline_set(&t);
    } else {
      trace_sink_set(&t);
    }
    CHECK(layout_run(c, {}, serial, placed, diags, nullptr, nullptr, INVALID));
  }
  trace_outline_set(nullptr);
  trace_sink_set(nullptr);
  t.end();
  return t.bytes;
}

}  // namespace

TEST_CASE("trace stream: a traced run hands its sink the stream its sinks record") {
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  for (TraceScope const scope :
       { TraceScope::Shipped, TraceScope::Search, TraceScope::Outline }) {
    CAPTURE(static_cast<uint32_t>(scope));
    Chart streamed_chart{ gauntlet("loop.scav") };
    Chart recorded_chart{ gauntlet("loop.scav") };
    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    Chunks got;
    bool streamed{ false };
    CHECK(layout_trace(streamed_chart,
                       {},
                       opts,
                       placed,
                       diags,
                       take_chunk,
                       &got,
                       streamed,
                       INVALID,
                       scope));
    CHECK(streamed);
    CHECK(trace_sink() == nullptr);
    CHECK(trace_outline() == nullptr);
    std::vector<uint8_t> joined;
    for (std::vector<uint8_t> const &chunk : got.taken) {
      joined.insert(joined.end(), chunk.begin(), chunk.end());
    }
    std::vector<uint8_t> const want{ recorded(recorded_chart, opts, scope) };
    CHECK(json_in_pieces(want, want.size()).size() > 4U);
    CHECK(joined == want);
    CHECK(layout_coordinate_hash(streamed_chart) ==
          layout_coordinate_hash(recorded_chart));
  }
}

TEST_CASE("trace stream: a null sink runs nothing") {
  Chart c{ gauntlet("loop.scav") };
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  bool streamed{ true };
  CHECK_FALSE(layout_trace(c, {}, opts, placed, diags, nullptr, nullptr, streamed));
  CHECK_FALSE(streamed);
  CHECK(placed.empty());
}
