// Hand-transcribed charts. Synthetic input has uniform branching and no
// accidental structure, so validating on it alone is a trap (PRD 17) -- these
// are the other half of P0's corpus.
//
// Each is a real state machine written the way someone would actually write it:
// mixed aliases and long spellings, comments where a reader wants them,
// concurrent submachines, cross-document endpoints, attributes from a plugin
// namespace. They live as inline literals rather than as files under test_data
// because parsing takes bytes and acquiring bytes is a different system.

#include "core/test_support.h"
#include "scav/scav_diagnostics.h"
#include "scav/scav_ids.h"
#include "scav/scav_parser.h"
#include "scav/scav_syntax_tree.h"

#include "doctest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

// PRD 15's own worked example, transcribed verbatim including its comment-free
// shape, so the document the format is specified by is a document that parses.
constexpr std::string_view EG91{ R"(
chart eg91 "EG91 modem driver" {
  include "wifi.scav" as wifi,

  state Off "modem powered down",
  state Booting,
  state PreConfig choice,

  trans * -> Off,
  trans Off -> Booting "EG91_POWER_ON",

  state On {
    @doc = "Enter: publishes MODEM_EVT_POWERED_ON",
    @libhsm { submachine_handler, legacy = "false" },

    submachine main {
      state Idle { @libhsm:handler = "false" },
      state Ready,
      trans * -> Idle,
      trans internal Ready -> Ready "FI_EG91_AT_RESPONSE_ERROR",
      trans Ready -> wifi/On/Connected "handoff",
    },
    submachine strays "consumes stray AT errors" {
      state Idle,
      trans * -> Idle,
    },
  },
}
)" };

// A TCP connection. Real-world shape: many peer states, a couple of
// pseudostates, one deep-history resume, and transitions that skip levels.
constexpr std::string_view TCP{ R"(
// RFC 793 connection states, as a chart rather than as a table.
chart tcp "TCP connection" {
  @doc = "Passive and active opens share the established substate.",

  state Closed,
  state Listen,
  state SynSent,
  state SynReceived,

  trans * -> Closed,
  trans Closed -> Listen "passive open",
  trans Closed -> SynSent "active open",
  trans Listen -> SynReceived "recv SYN",
  trans SynSent -> SynReceived "recv SYN",

  state Established "data may flow both ways" {
    // Both halves close independently, which is why this is concurrent.
    submachine inbound {
      state Open,
      state HalfClosed,
      trans * -> Open,
      trans Open -> HalfClosed "recv FIN",
    },
    submachine outbound {
      state Open,
      state FinSent,
      trans * -> Open,
      trans Open -> FinSent "send FIN", // no more data from us
    },
  },

  trans SynReceived -> Established "recv ACK",
  trans SynSent -> Established "recv SYN+ACK",

  state Closing choice,
  state TimeWait,

  // A transition out of a nested state to a top-level one: the long
  // hierarchical edge the whole project exists for.
  trans Established:inbound/HalfClosed -> Closing,
  trans Established:outbound/FinSent -> Closing,
  trans Closing -> TimeWait "both halves closed",
  trans TimeWait -> Closed "2MSL elapsed",
}
)" };

// Firmware over-the-air update. Written in the terse aliases a person drafting
// reaches for, with a fork/join pair and a raw-string label.
constexpr std::string_view OTA{ R"(
chart ota "firmware OTA" {
  @vendor:component = "bootloader",
  @tags = ["firmware", "safety-critical"],

  s Idle,
  t * -> Idle,

  s Downloading {
    @doc = """
      Chunks arrive out of order and are written straight to the
      inactive slot. The manifest is verified only once every chunk
      has landed.
      """,
    m main {
      s Fetching,
      s Writing,
      t * -> Fetching,
      t Fetching -> Writing "chunk ready",
      t internal Writing -> Writing "flash busy",
      t Writing -> Fetching "chunk written",
    },
  },

  s Verify fork,
  s CheckSignature,
  s CheckVersion,
  s Verified join,
  s Failed,

  t Idle -> Downloading "update offered",
  t Downloading -> Verify "all chunks received",
  t Verify -> CheckSignature,
  t Verify -> CheckVersion,
  t CheckSignature -> Verified,
  t CheckVersion -> Verified,
  t Verified -> Idle "swap slots and reboot",

  // Anything can fail at any time, and failure is terminal for this attempt.
  t CheckSignature -> Failed "bad signature",
  t CheckVersion -> Failed "downgrade refused",
  t Downloading -> Failed "timeout",
  t Failed -> *,
}
)" };

struct Chart {
  char const *name;
  std::string_view text;
};

std::vector<Chart> corpus() {
  return { { "eg91", EG91 }, { "tcp", TCP }, { "ota", OTA } };
}

uint32_t count_of(ParsedDocument const &pd, ElemKind kind) {
  return static_cast<uint32_t>(stmts_of(pd, kind).size());
}

}  // namespace

TEST_CASE("corpus: every chart parses with no diagnostics") {
  // P0's exit gate, stated as a test rather than as a claim.
  for (Chart const &c : corpus()) {
    Parsed const r{ parse(c.text, std::string{ c.name } + ".scav") };
    CHECK_MESSAGE(r.ok, c.name << ": " << diag_message(first_code(r.diags)));
    CHECK_MESSAGE(r.diags.empty(), c.name);
  }
}

TEST_CASE("corpus: every statement's span lands inside the document") {
  for (Chart const &c : corpus()) {
    Parsed const r{ parse(c.text) };
    REQUIRE(r.ok);
    uint32_t const len{ static_cast<uint32_t>(r.pd.src_bytes.size()) };
    for (Statement const &s : r.pd.stmts) {
      CHECK_MESSAGE(s.src.len > 0, c.name);
      CHECK_MESSAGE(static_cast<size_t>(s.src.off) + s.src.len <= len, c.name);
    }
    // PRD 10 will check this structurally in P1; asserting it here means the
    // parser never hands over a span the validator would reject.
    CHECK(r.pd.doc.text == make_span(0, len));
  }
}

TEST_CASE("corpus: every span index is in range for the array it names") {
  for (Chart const &c : corpus()) {
    Parsed const r{ parse(c.text) };
    REQUIRE(r.ok);
    for (uint32_t i = 0; i < r.pd.stmts.size(); ++i) {
      Span const kids{ r.pd.stmt_children[i] };
      CHECK(static_cast<size_t>(kids.off) + kids.len <= r.pd.stmt_ids.size());
      for (uint32_t k = 0; k < kids.len; ++k) {
        CHECK(r.pd.stmt_ids[kids.off + k].v < r.pd.stmts.size());
      }
      Span const trivia{ r.pd.stmts[i].comments };
      CHECK(static_cast<size_t>(trivia.off) + trivia.len <= r.pd.comments.size());
      CHECK(r.pd.stmt_payload[i] != INVALID);
    }
    for (TransStmt const &t : r.pd.transitions) {
      CHECK(static_cast<size_t>(t.src.segs.off) + t.src.segs.len <= r.pd.path_segs.size());
      CHECK(static_cast<size_t>(t.dst.segs.off) + t.dst.segs.len <= r.pd.path_segs.size());
    }
    for (AttrStmt const &a : r.pd.attrs) {
      CHECK(static_cast<size_t>(a.entries.off) + a.entries.len <=
            r.pd.attr_entries.size());
    }
    for (AttrEntry const &e : r.pd.attr_entries) {
      CHECK(static_cast<size_t>(e.values.off) + e.values.len <= r.pd.attr_values.size());
    }
  }
}

TEST_CASE("corpus: every StrRef points inside the finalized pool") {
  for (Chart const &c : corpus()) {
    Parsed const r{ parse(c.text) };
    REQUIRE(r.ok);
    size_t const n{ r.pd.strings.bytes.size() };
    auto const in_pool = [n](StrRef ref) {
      return (ref.len == 0) ? (ref.off == 0)
                            : (static_cast<size_t>(ref.off) + ref.len <= n);
    };
    CHECK(in_pool(r.pd.doc.path));
    for (ChartStmt const &s : r.pd.charts) {
      CHECK(in_pool(s.name));
      CHECK(in_pool(s.label));
    }
    for (StateStmt const &s : r.pd.states) {
      CHECK(in_pool(s.name));
      CHECK(in_pool(s.label));
    }
    for (SubmachineStmt const &s : r.pd.submachines) {
      CHECK(in_pool(s.name));
      CHECK(in_pool(s.label));
    }
    for (TransStmt const &s : r.pd.transitions) { CHECK(in_pool(s.label)); }
    for (IncludeStmt const &s : r.pd.includes) {
      CHECK(in_pool(s.path));
      CHECK(in_pool(s.alias));
    }
    for (AttrStmt const &s : r.pd.attrs) { CHECK(in_pool(s.ns)); }
    for (AttrEntry const &e : r.pd.attr_entries) { CHECK(in_pool(e.key)); }
    for (StrRef const v : r.pd.attr_values) { CHECK(in_pool(v)); }
    for (PathSeg const &s : r.pd.path_segs) {
      CHECK(in_pool(s.name));
      CHECK(in_pool(s.qualifier));
    }
  }
}

TEST_CASE("corpus: eg91 is the chart PRD 15 specifies") {
  Parsed const r{ parse(EG91, "eg91.scav") };
  REQUIRE(r.ok);
  CHECK(str(r.pd, r.pd.charts[0].name) == "eg91");
  CHECK(str(r.pd, r.pd.charts[0].label) == "EG91 modem driver");
  CHECK(count_of(r.pd, ElemKind::Include) == 1);
  // Off, Booting, PreConfig, On, main/Idle, main/Ready, strays/Idle.
  CHECK(count_of(r.pd, ElemKind::State) == 7);
  CHECK(count_of(r.pd, ElemKind::Submachine) == 2);
  CHECK(count_of(r.pd, ElemKind::Trans) == 6);
  CHECK(count_of(r.pd, ElemKind::Attr) == 3);

  // The cross-document endpoint is an ordinary path, because an include alias
  // is an ordinary state name (PRD 9).
  bool found_handoff{ false };
  for (uint32_t const stmt : stmts_of(r.pd, ElemKind::Trans)) {
    if (str(r.pd, trans_at(r.pd, stmt).label) != "handoff") { continue; }
    found_handoff = true;
    CHECK(path_text(r.pd, trans_at(r.pd, stmt).dst) == "wifi/On/Connected");
  }
  CHECK(found_handoff);
}

TEST_CASE("corpus: eg91's two submachines make On concurrent") {
  Parsed const r{ parse(EG91) };
  REQUIRE(r.ok);
  uint32_t on{ INVALID };
  for (uint32_t const stmt : stmts_of(r.pd, ElemKind::State)) {
    if (str(r.pd, state_at(r.pd, stmt).name) == "On") { on = stmt; }
  }
  REQUIRE(on != INVALID);

  uint32_t submachines{ 0 };
  Span const kids{ r.pd.stmt_children[on] };
  for (uint32_t i = 0; i < kids.len; ++i) {
    if (r.pd.stmts[r.pd.stmt_ids[kids.off + i].v].kind == ElemKind::Submachine) {
      ++submachines;
    }
  }
  CHECK(submachines == 2);
}

TEST_CASE("corpus: tcp keeps its long hierarchical edges as written") {
  Parsed const r{ parse(TCP, "tcp.scav") };
  REQUIRE(r.ok);
  std::vector<std::string> paths;
  for (uint32_t const stmt : stmts_of(r.pd, ElemKind::Trans)) {
    paths.push_back(path_text(r.pd, trans_at(r.pd, stmt).src));
  }
  bool found_inbound{ false };
  bool found_outbound{ false };
  for (std::string const &p : paths) {
    found_inbound = found_inbound || (p == "Established:inbound/HalfClosed");
    found_outbound = found_outbound || (p == "Established:outbound/FinSent");
  }
  CHECK(found_inbound);
  CHECK(found_outbound);
}

TEST_CASE("corpus: tcp's duplicate names in different submachines are distinct rows") {
  // `Open` exists in both halves. Addressing is by path, so there is no
  // display-name-versus-identifier split to invent (PRD 17).
  Parsed const r{ parse(TCP) };
  REQUIRE(r.ok);
  uint32_t opens{ 0 };
  for (uint32_t const stmt : stmts_of(r.pd, ElemKind::State)) {
    if (str(r.pd, state_at(r.pd, stmt).name) == "Open") { ++opens; }
  }
  CHECK(opens == 2);
}

TEST_CASE("corpus: ota's aliases produce the same rows the long spelling would") {
  Parsed const r{ parse(OTA, "ota.scav") };
  REQUIRE(r.ok);
  CHECK(count_of(r.pd, ElemKind::Submachine) == 1);
  CHECK(count_of(r.pd, ElemKind::State) == 9);

  std::vector<StateKind> kinds;
  for (uint32_t const stmt : stmts_of(r.pd, ElemKind::State)) {
    kinds.push_back(state_at(r.pd, stmt).kind);
  }
  bool has_fork{ false };
  bool has_join{ false };
  for (StateKind const k : kinds) {
    has_fork = has_fork || (k == StateKind::Fork);
    has_join = has_join || (k == StateKind::Join);
  }
  CHECK(has_fork);
  CHECK(has_join);
}

TEST_CASE("corpus: ota's raw-string label is dedented to its closing delimiter") {
  Parsed const r{ parse(OTA) };
  REQUIRE(r.ok);
  std::string doc;
  for (uint32_t const stmt : stmts_of(r.pd, ElemKind::Attr)) {
    AttrStmt const &a{ attr_at(r.pd, stmt) };
    AttrEntry const &e{ r.pd.attr_entries[a.entries.off] };
    if (str(r.pd, e.key) != "doc") { continue; }
    doc = str(r.pd, r.pd.attr_values[e.values.off]);
  }
  CHECK(doc ==
        "Chunks arrive out of order and are written straight to the\n"
        "inactive slot. The manifest is verified only once every chunk\n"
        "has landed.");
}

TEST_CASE("corpus: ota's list attribute keeps its order") {
  Parsed const r{ parse(OTA) };
  REQUIRE(r.ok);
  for (uint32_t const stmt : stmts_of(r.pd, ElemKind::Attr)) {
    AttrStmt const &a{ attr_at(r.pd, stmt) };
    AttrEntry const &e{ r.pd.attr_entries[a.entries.off] };
    if (str(r.pd, e.key) != "tags") { continue; }
    REQUIRE(e.values.len == 2);
    CHECK(str(r.pd, r.pd.attr_values[e.values.off]) == "firmware");
    CHECK(str(r.pd, r.pd.attr_values[e.values.off + 1]) == "safety-critical");
  }
}

TEST_CASE("corpus: reformatting a chart onto one line changes nothing but the spans") {
  // Newlines carry nothing (PRD 15), which is what makes byte-identical output
  // the printer's job rather than the format's.
  for (Chart const &c : corpus()) {
    Parsed const spread{ parse(c.text) };
    REQUIRE(spread.ok);

    // Strip comments and collapse runs of whitespace outside strings. Crude on
    // purpose: a general reformatter is P3's, and this only has to be
    // whitespace-equivalent.
    std::string flat;
    bool in_string{ false };
    for (uint32_t i = 0; i < c.text.size(); ++i) {
      char const ch{ c.text[i] };
      if (!in_string && (ch == '/') && (i + 1 < c.text.size()) && (c.text[i + 1] == '/')) {
        while ((i < c.text.size()) && (c.text[i] != '\n')) { ++i; }
        continue;
      }
      if (ch == '"') { in_string = !in_string; }
      if (!in_string && ((ch == '\n') || (ch == '\t'))) {
        flat.push_back(' ');
        continue;
      }
      flat.push_back(ch);
    }

    Parsed const collapsed{ parse(flat) };
    REQUIRE_MESSAGE(collapsed.ok,
                    c.name << ": " << diag_message(first_code(collapsed.diags)));
    CHECK_MESSAGE(collapsed.pd.stmts.size() == spread.pd.stmts.size(), c.name);
    CHECK_MESSAGE(collapsed.pd.strings.bytes == spread.pd.strings.bytes, c.name);
    for (uint32_t i = 0; i < spread.pd.stmts.size(); ++i) {
      CHECK(collapsed.pd.stmts[i].kind == spread.pd.stmts[i].kind);
      CHECK(collapsed.pd.stmt_children[i].len == spread.pd.stmt_children[i].len);
    }
  }
}

TEST_CASE("corpus: parsing the same bytes twice yields identical structure") {
  // Determinism discipline is in force from the first commit (PRD 17), and a
  // parser that depends on nothing but its input is where that starts.
  for (Chart const &c : corpus()) {
    Parsed const first{ parse(c.text) };
    Parsed const second{ parse(c.text) };
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    CHECK(first.pd.strings.bytes == second.pd.strings.bytes);
    CHECK(first.pd.src_bytes == second.pd.src_bytes);
    REQUIRE(first.pd.stmts.size() == second.pd.stmts.size());
    for (uint32_t i = 0; i < first.pd.stmts.size(); ++i) {
      CHECK(first.pd.stmts[i].kind == second.pd.stmts[i].kind);
      CHECK(first.pd.stmts[i].src == second.pd.stmts[i].src);
      CHECK(first.pd.stmts[i].comments == second.pd.stmts[i].comments);
      CHECK(first.pd.stmt_payload[i] == second.pd.stmt_payload[i]);
      CHECK(first.pd.stmt_children[i] == second.pd.stmt_children[i]);
    }
  }
}

TEST_CASE("corpus: comments land where a reader put them") {
  Parsed const r{ parse(TCP) };
  REQUIRE(r.ok);
  uint32_t leading{ 0 };
  uint32_t trailing{ 0 };
  uint32_t own_line{ 0 };
  for (Trivia const &t : r.pd.comments) {
    leading += (t.pos == CommentPos::Leading) ? 1U : 0U;
    trailing += (t.pos == CommentPos::Trailing) ? 1U : 0U;
    own_line += (t.pos == CommentPos::OwnLine) ? 1U : 0U;
  }
  CHECK(r.pd.comments.size() == 5);
  CHECK(trailing == 1);  // the one after `send FIN`
  CHECK(leading + own_line == 4);
}
