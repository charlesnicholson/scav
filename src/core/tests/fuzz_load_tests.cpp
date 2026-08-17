// A deterministic sweep over malformed networks. The loader terminates, every
// diagnostic lands inside its document, and any chart returned is resolved.

#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

// splitmix64's finalizer, position-addressed: reproducible from the seed alone.
uint64_t rnd(uint64_t seed, uint64_t index) {
  uint64_t x{ seed + (index * 0x9E37'79B9'7F4A'7C15ULL) };
  x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31U);
}

struct Doc {
  std::string name;
  std::string text;
};

// Every loader diagnostic is document-local: it names a document the loader
// holds, and its span lands inside that document's bytes.
void check_loader_diags(Loader const &s, std::vector<Diagnostic> const &diags) {
  for (Diagnostic const &d : diags) {
    CHECK(d.code != DiagCode::Ok);
    if (d.src.len == 0) { continue; }  // a finding with no statement to point at
    scav_byte const *bytes{ nullptr };
    uint32_t len{ 0 };
    if (load_document_bytes(s, d.doc, &bytes, &len)) {
      CHECK((static_cast<size_t>(d.src.off) + d.src.len) <= len);
    }
  }
}

// Past a successful attach every finding is an ordinary chart diagnostic, so
// its span indexes the chart's pool instead.
void check_chart_diags(Chart const &c, std::vector<Diagnostic> const &diags) {
  for (Diagnostic const &d : diags) {
    CHECK((static_cast<size_t>(d.src.off) + d.src.len) <= c.src_bytes.size());
    if (d.subject.kind == ElemKind::State) { CHECK(d.subject.ordinal < c.states.size()); }
  }
}

// Drives one network to completion. `order` permutes each pending batch, so a
// run covers arrival orders the application is entitled to produce.
struct Run {
  Loader loader;
  Chart chart;
  std::vector<Diagnostic> diags;
  bool ok;
};

Run drive(std::vector<Doc> const &corpus, uint64_t order) {
  Run run;
  if (corpus.empty()) {
    run.ok = load_finish(run.loader, run.chart, run.diags);
    return run;
  }
  if (!load_add(run.loader, raw(corpus[0].text), corpus[0].text.size(), corpus[0].name)) {
    run.ok = load_finish(run.loader, run.chart, run.diags);
    return run;
  }

  std::vector<std::string> wanted;
  // A cap rather than a while(true), so a loader that never drains pending
  // fails. One round per level, clearing the deepest chain built below.
  constexpr uint32_t MAX_ROUNDS{ 1024 };
  for (uint32_t round = 0; round < MAX_ROUNDS; ++round) {
    wanted.clear();
    for (Pending const &p : load_pending(run.loader)) {
      wanted.emplace_back(load_pending_path(run.loader, p));
    }
    if (wanted.empty()) { break; }

    // A deterministic rotation of the batch, derived from the seed.
    if (wanted.size() > 1) {
      size_t const by{ static_cast<size_t>(rnd(order, round) % wanted.size()) };
      std::vector<std::string> rotated;
      rotated.reserve(wanted.size());
      for (size_t i = 0; i < wanted.size(); ++i) {
        rotated.push_back(wanted[(i + by) % wanted.size()]);
      }
      wanted.swap(rotated);
    }

    bool advanced{ false };
    for (std::string const &want : wanted) {
      std::string_view body;
      for (Doc const &d : corpus) {
        if (d.name == want) { body = d.text; }
      }
      if (body.empty()) { continue; }
      advanced = true;
      if (!load_add(run.loader, raw(body), body.size(), want)) {
        run.ok = load_finish(run.loader, run.chart, run.diags);
        return run;
      }
    }
    if (!advanced) { break; }
  }
  run.ok = load_finish(run.loader, run.chart, run.diags);
  return run;
}

// Paths chosen to hit every branch of the resolver and of discovery: fine,
// escaping, self-naming, absolute, scheme-carrying, and outright rejected.
constexpr std::array<std::string_view, 18> PATHS{
  "b.scav",        "./b.scav",    "../b.scav",  "sub/b.scav",
  "sub/../b.scav", "a.scav",      "c.scav",     "",
  "sub/",          "..",          ".",          "/abs.scav",
  "x://h/y",       "b.scav/",     "././b.scav", "../../../../b.scav",
  "b.scav\\c",     "sub//b.scav",
};

std::string include_line(uint64_t key, uint32_t n) {
  std::string out;
  out += "include \"";
  out += PATHS[rnd(key, n) % PATHS.size()];
  out += "\" as i";
  out += std::to_string(n);
  out += ",\n";
  return out;
}

// A deterministic mix of includes, states and endpoints, some of them reaching
// through an alias.
std::string synth_doc(std::string_view chart_name, uint64_t key) {
  std::string out{ "chart " };
  out += chart_name;
  out += " {\n";
  uint32_t const includes{ static_cast<uint32_t>(rnd(key, 1) % 3U) };
  for (uint32_t i = 0; i < includes; ++i) { out += include_line(key, i + 2U); }
  out += "state S0,\nstate S1,\ntrans * -> S0,\ntrans S0 -> S1,\n";
  if (includes != 0) { out += "trans S1 -> i0/S0,\n"; }
  if ((rnd(key, 9) % 4U) == 0U) { out += "trans S1 -> i9/Missing,\n"; }
  out += "}\n";
  return out;
}

}  // namespace

TEST_CASE("fuzz: any network the loader accepts is complete, or it built nothing") {
  constexpr uint64_t SEED{ 0x5CA1'AB1E'0000'0202ULL };
  for (uint32_t i = 0; i < 600; ++i) {
    uint64_t const key{ SEED + i };
    CAPTURE(i);

    std::vector<Doc> corpus{
      { "a.scav", synth_doc("a", key) },
      { "b.scav", synth_doc("b", key ^ 0x11U) },
      { "c.scav", synth_doc("c", key ^ 0x22U) },
    };
    // Sometimes the escaping path is real, sometimes it is a hole the
    // application can never fill.
    if ((rnd(key, 20) % 3U) == 0U) {
      corpus.push_back({ "../b.scav", synth_doc("up", key ^ 0x33U) });
    }

    Run run{ drive(corpus, key) };

    // Whether the chart got built decides which pool a finding's span indexes,
    // so the sweep branches on that rather than trying both.
    if (run.chart.documents.empty()) {
      // A fatal load. It must have said why, and left nothing half-built.
      CHECK_FALSE(run.ok);
      CHECK_FALSE(run.diags.empty());
      CHECK(run.chart.states.empty());
      CHECK(run.chart.includes.empty());
      check_loader_diags(run.loader, run.diags);
      continue;
    }

    check_chart_diags(run.chart, run.diags);

    // A chart that exists is a complete network: every include names a real
    // document and every alias host gained its submachine.
    for (Include const &inc : run.chart.includes) {
      CHECK(inc.target.v < run.chart.documents.size());
      REQUIRE(inc.host.v < run.chart.states.size());
      CHECK(run.chart.states[inc.host.v].submachines.len == 1);
    }

    // Validation must agree, and the structural checks must survive whatever
    // the network was.
    std::vector<Diagnostic> validate_diags;
    bool const clean{ validate_chart(run.chart, validate_diags) };
    for (Diagnostic const &d : validate_diags) {
      if (d.subject.kind == ElemKind::State) {
        CHECK(d.subject.ordinal < run.chart.states.size());
      }
    }

    // Every live address round-trips on a clean chart, across documents.
    if (!clean) { continue; }
    for (uint32_t s = 0; s < run.chart.states.size(); ++s) {
      if (run.chart.states[s].live == 0) { continue; }
      std::string const address{ path(run.chart, StateId{ s }) };
      CHECK_FALSE(address.empty());
      StateId back{ INVALID };
      CAPTURE(address);
      CHECK(resolve_path(run.chart, run.chart.root_submachine, address, back) ==
            ResolveStatus::Ok);
      CHECK(back == StateId{ s });
    }
  }
}

TEST_CASE("fuzz: arrival order never changes the model") {
  // Swept rather than asserted once: one corpus resolved in different orders
  // gives the same bytes.
  constexpr uint64_t SEED{ 0x5CA1'AB1E'0000'0303ULL };
  for (uint32_t i = 0; i < 200; ++i) {
    uint64_t const key{ SEED + i };
    CAPTURE(i);
    std::vector<Doc> const corpus{
      { "a.scav", synth_doc("a", key) },
      { "b.scav", synth_doc("b", key ^ 0x11U) },
      { "c.scav", synth_doc("c", key ^ 0x22U) },
      { "../b.scav", synth_doc("up", key ^ 0x33U) },
    };

    Run const first{ drive(corpus, key) };
    Run const second{ drive(corpus, key ^ 0xFFFF'FFFFULL) };
    CHECK(first.ok == second.ok);
    CHECK(chart_structural_hash(first.chart) == chart_structural_hash(second.chart));

    std::vector<scav_byte> a;
    std::vector<scav_byte> b;
    chart_digest_bytes(first.chart, a);
    chart_digest_bytes(second.chart, b);
    CHECK(a == b);
  }
}

TEST_CASE("fuzz: a cycle at any length is caught rather than followed") {
  for (uint32_t n = 1; n <= 12; ++n) {
    CAPTURE(n);
    std::vector<Doc> corpus;
    corpus.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      std::string text{ "chart d" };
      text += std::to_string(i);
      text += " {\ninclude \"d";
      text += std::to_string((i + 1) % n);  // the last one closes the ring
      text += ".scav\" as nxt,\nstate S,\n}\n";
      corpus.push_back({ "d" + std::to_string(i) + ".scav", text });
    }
    Run const run{ drive(corpus, n) };
    CHECK_FALSE(run.ok);
    CHECK(has_code(run.diags, DiagCode::IncludeCycle));
    CHECK(run.chart.documents.empty());
  }
}

TEST_CASE("fuzz: a deep include chain resolves without recursing") {
  // The walks are explicit stacks because chain depth is attacker-controlled.
  // 400 documents is well past any real chart and well past a default stack.
  constexpr uint32_t DEPTH{ 400 };
  std::vector<Doc> corpus;
  corpus.reserve(DEPTH);
  for (uint32_t i = 0; i < DEPTH; ++i) {
    std::string text{ "chart d" };
    text += std::to_string(i);
    text += " {\n";
    if (i + 1 < DEPTH) {
      text += "include \"d" + std::to_string(i + 1) + ".scav\" as nxt,\n";
    }
    text += "state S,\ntrans * -> S,\n}\n";
    corpus.push_back({ "d" + std::to_string(i) + ".scav", text });
  }

  Run const run{ drive(corpus, 0) };
  REQUIRE_MESSAGE(run.ok, diag_message(first_code(run.diags)));
  CHECK(run.chart.documents.size() == DEPTH);
  CHECK(run.chart.includes.size() == DEPTH - 1);

  // And the address of the deepest state is 399 segments that still resolve.
  StateId deepest{ INVALID };
  for (uint32_t s = 0; s < run.chart.states.size(); ++s) {
    if (run.chart.states[s].inst.v == (DEPTH - 2)) {
      if (chart_string(run.chart, run.chart.states[s].name) == "S") { deepest = { s }; }
    }
  }
  REQUIRE(deepest.v != INVALID);
  StateId back{ INVALID };
  CHECK(
      resolve_path(run.chart, run.chart.root_submachine, path(run.chart, deepest), back) ==
      ResolveStatus::Ok);
  CHECK(back == deepest);
}

TEST_CASE("fuzz: an exponential DAG is capped rather than expanded") {
  // Not a cycle, so the cycle check passes it: 40 documents each including the
  // next twice reaches 2^39 instantiations, and the walk stops at its cap.
  constexpr uint32_t DEPTH{ 40 };
  std::vector<Doc> corpus;
  corpus.reserve(DEPTH);
  for (uint32_t i = 0; i < DEPTH; ++i) {
    std::string text{ "chart d" };
    text += std::to_string(i);
    text += " {\n";
    if (i + 1 < DEPTH) {
      std::string const nxt{ std::to_string(i + 1) };
      text += "include \"d" + nxt + ".scav\" as l,\n";
      text += "include \"d" + nxt + ".scav\" as r,\n";
    }
    text += "state S,\n}\n";
    corpus.push_back({ "d" + std::to_string(i) + ".scav", text });
  }

  Run const run{ drive(corpus, 0) };
  CHECK_FALSE(run.ok);
  CHECK(has_code(run.diags, DiagCode::IncludeExpansionTooLarge));
  // And nothing half-built: a chart handed back is always a complete network,
  // so stopping mid-expansion has to leave none at all.
  CHECK(run.chart.documents.empty());
  CHECK(run.chart.states.empty());
}
