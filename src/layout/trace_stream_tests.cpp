// The trace's encoding, decoding and header; then streams: the file's replacement, failed
// writes, and traced runs whose streamed JSON is their in-memory trace's.

#include "layout/trace_stream.h"

#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace scav { void trace_test_fail_write(uint32_t nth); }  // namespace scav

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
      f(e.search.t0);
      f(e.search.framed_t0);
      f(e.search.t2);
      f(e.search.framed);
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
    if (!trace_decode(d, bytes.data() + at, n, collect, &out)) { return false; }
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
  REQUIRE(d.states.size() == 3U);
  CHECK(d.states[0] == "Alpha");
  CHECK(d.states[1].empty());
  CHECK(d.states[2] == "Gamma");
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

std::string scratch(std::string_view name) {
  return std::string{ SCAV_TEST_OUT_DIR } + "/trace_stream_" + std::string{ name };
}

bool exists(std::string const &path) {
  std::FILE *const f{ std::fopen(path.c_str(), "rb") };
  if (f == nullptr) { return false; }
  std::ignore = std::fclose(f);
  return true;
}

std::string contents(std::string const &path) {
  std::vector<scav_byte> bytes;
  if (!read_file(path.c_str(), bytes)) { return "<unreadable>"; }
  return { reinterpret_cast<char const *>(bytes.data()), bytes.size() };
}

void put(std::string const &path, std::string_view text) {
  REQUIRE(write_file(path.c_str(),
                     reinterpret_cast<scav_byte const *>(text.data()),
                     text.size()));
}

// Every kind at every value set, repeated past several chunks of stream.
std::vector<TraceEvent> many_events() {
  std::vector<TraceEvent> const one{ every_event() };
  std::vector<TraceEvent> out;
  for (uint32_t k = 0; k < 400; ++k) { out.insert(out.end(), one.begin(), one.end()); }
  return out;
}

bool append_text(void *ctx, char const *text, size_t n) {
  static_cast<std::string *>(ctx)->append(text, n);
  return true;
}

std::string json_of(LayoutTrace const &t, Chart const &c) {
  std::vector<char> out;
  trace_to_json(t, c, out);
  return { out.begin(), out.end() };
}

// The events of the trace file at `path`; empty when it is not a whole stream.
std::vector<TraceEvent> events_in(std::string const &path) {
  std::vector<scav_byte> bytes;
  std::vector<TraceEvent> out;
  TraceDecoder d;
  if (!read_file(path.c_str(), bytes) ||
      !trace_decode(d, bytes.data(), bytes.size(), collect, &out) ||
      !trace_decode_whole(d)) {
    out.clear();
  }
  return out;
}

bool same_events(std::vector<TraceEvent> const &a, std::vector<TraceEvent> const &b) {
  if (a.size() != b.size()) { return false; }
  for (size_t k = 0; k < a.size(); ++k) {
    if (values(a[k]) != values(b[k])) { return false; }
  }
  return true;
}

// Fails the `nth` file write for its scope.
struct FailGuard {
  explicit FailGuard(uint32_t nth) { trace_test_fail_write(nth); }
  ~FailGuard() { trace_test_fail_write(0); }
  FailGuard(FailGuard const &) = delete;
  FailGuard &operator=(FailGuard const &) = delete;
};

}  // namespace

TEST_CASE("trace stream: a file appears only once its stream finishes whole") {
  Chart const c{ named_chart() };
  std::string const path{ scratch("whole.trace") };
  std::string const temp{ path + ".tmp" };
  std::ignore = std::remove(path.c_str());
  std::ignore = std::remove(temp.c_str());
  std::vector<TraceEvent> const events{ many_events() };
  LayoutTrace want;
  {
    TraceStream s{ c, { .path = path.c_str(), .write = nullptr, .ctx = nullptr } };
    REQUIRE(s.opened());
    for (TraceEvent const &e : events) {
      trace_put(s.sink(), e);
      want.events.push_back(e);
    }
    CHECK_FALSE(exists(path));
    CHECK(exists(temp));
    CHECK(s.finish());
  }
  CHECK_FALSE(exists(temp));
  CHECK(same_events(events_in(path), events));
  std::string json;
  REQUIRE(trace_file_json(path.c_str(), append_text, &json));
  CHECK(json == json_of(want, c));
}

TEST_CASE("trace stream: finishing replaces a file; an unfinished stream leaves it") {
  Chart const c{ named_chart() };
  std::string const path{ scratch("replaced.trace") };
  std::string const temp{ path + ".tmp" };
  put(path, "previous");
  std::vector<TraceEvent> const events{ many_events() };
  {
    TraceStream s{ c, { .path = path.c_str(), .write = nullptr, .ctx = nullptr } };
    REQUIRE(s.opened());
    for (TraceEvent const &e : events) { trace_put(s.sink(), e); }
    CHECK(contents(path) == "previous");
  }
  CHECK(contents(path) == "previous");
  CHECK_FALSE(exists(temp));
  {
    TraceStream s{ c, { .path = path.c_str(), .write = nullptr, .ctx = nullptr } };
    REQUIRE(s.opened());
    for (TraceEvent const &e : events) { trace_put(s.sink(), e); }
    CHECK(contents(path) == "previous");
    CHECK(s.finish());
  }
  CHECK_FALSE(exists(temp));
  CHECK(same_events(events_in(path), events));
}

TEST_CASE("trace stream: a failed write leaves no temp and the file as it was") {
  Chart const c{ named_chart() };
  std::string const path{ scratch("failed.trace") };
  std::string const temp{ path + ".tmp" };
  std::vector<TraceEvent> const events{ many_events() };
  for (bool const had : { false, true }) {
    CAPTURE(had);
    if (had) {
      put(path, "previous");
    } else {
      std::ignore = std::remove(path.c_str());
    }
    {
      FailGuard const failing{ 2 };
      TraceStream s{ c, { .path = path.c_str(), .write = nullptr, .ctx = nullptr } };
      REQUIRE(s.opened());
      for (TraceEvent const &e : events) { trace_put(s.sink(), e); }
      CHECK_FALSE(s.finish());
    }
    CHECK_FALSE(exists(temp));
    if (had) {
      CHECK(contents(path) == "previous");
    } else {
      CHECK_FALSE(exists(path));
    }
  }
}

TEST_CASE("trace stream: a file that cannot be created takes no events") {
  Chart const c{ named_chart() };
  std::string const path{ scratch("no_such_dir/x.trace") };
  TraceStream s{ c, { .path = path.c_str(), .write = nullptr, .ctx = nullptr } };
  CHECK_FALSE(s.opened());
  trace_put(s.sink(), make(static_cast<uint32_t>(TraceKind::NetPlanned), 1));
  CHECK_FALSE(s.finish());
  CHECK_FALSE(exists(path));
  TraceStream neither{ c, { .path = nullptr, .write = nullptr, .ctx = nullptr } };
  CHECK_FALSE(neither.opened());
  CHECK_FALSE(neither.finish());
}

TEST_CASE("trace stream: JSON whose write fails fails the stream") {
  Chart const c{ named_chart() };
  TraceWrite const refuse = [](void * /*ctx*/, char const * /*text*/, size_t /*n*/) {
    return false;
  };
  TraceStream s{ c, { .path = nullptr, .write = refuse, .ctx = nullptr } };
  REQUIRE(s.opened());
  for (TraceEvent const &e : many_events()) { trace_put(s.sink(), e); }
  CHECK_FALSE(s.finish());
}

TEST_CASE("trace stream: a JSON stream writes what the in-memory trace writes") {
  Chart const c{ named_chart() };
  for (bool const empty : { true, false }) {
    CAPTURE(empty);
    std::vector<TraceEvent> const events{ empty ? std::vector<TraceEvent>{}
                                                : many_events() };
    LayoutTrace want;
    std::string json;
    {
      TraceStream s{ c, { .path = nullptr, .write = append_text, .ctx = &json } };
      REQUIRE(s.opened());
      for (TraceEvent const &e : events) {
        trace_put(s.sink(), e);
        want.events.push_back(e);
      }
      CHECK(s.finish());
    }
    CHECK(json == json_of(want, c));
  }
}

TEST_CASE("trace stream: a file that is not a whole trace is refused") {
  Chart const c{ named_chart() };
  std::string const path{ scratch("cut.trace") };
  std::string json;
  CHECK_FALSE(trace_file_json(scratch("absent.trace").c_str(), append_text, &json));
  std::vector<uint8_t> const bytes{ stream_of(c, every_event()) };
  REQUIRE(write_file(path.c_str(), bytes.data(), bytes.size() - 1U));
  CHECK_FALSE(trace_file_json(path.c_str(), append_text, &json));
  REQUIRE(write_file(path.c_str(), bytes.data(), bytes.size()));
  json.clear();
  CHECK(trace_file_json(path.c_str(), append_text, &json));
  CHECK(json.starts_with("[\n  {\"i\":0,\"kind\":\"none\""));
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

// The trace `layout_trace` streams for `scope`, recorded in memory with the sinks set
// directly, as JSON.
std::string in_memory(Chart &c, scav_layout_opts const &o, TraceScope scope) {
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_layout_opts serial{ o };
  serial.threads = 1;
  LayoutTrace t;
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
  return json_of(t, c);
}

}  // namespace

TEST_CASE("trace stream: a traced run streams the JSON its in-memory trace makes") {
  scav_layout_opts opts{};
  REQUIRE(profile_named("readable", opts.profile));
  for (char const *name : { "folded.scav", "loop.scav" }) {
    for (TraceScope const scope :
         { TraceScope::Shipped, TraceScope::Search, TraceScope::Outline }) {
      std::string const chart{ name };
      CAPTURE(chart);
      CAPTURE(static_cast<uint32_t>(scope));
      Chart streamed_chart{ gauntlet(name) };
      Chart memory_chart{ gauntlet(name) };
      std::vector<scav_placed> placed;
      std::vector<Diagnostic> diags;
      std::string json;
      bool streamed{ false };
      CHECK(layout_trace(streamed_chart,
                         {},
                         opts,
                         placed,
                         diags,
                         { .path = nullptr, .write = append_text, .ctx = &json },
                         streamed,
                         INVALID,
                         scope));
      CHECK(streamed);
      CHECK(trace_sink() == nullptr);
      CHECK(trace_outline() == nullptr);
      std::string const want{ in_memory(memory_chart, opts, scope) };
      CHECK(want.size() > 4U);
      CHECK(json == want);
      CHECK(layout_coordinate_hash(streamed_chart) ==
            layout_coordinate_hash(memory_chart));
    }
  }
}
