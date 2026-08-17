// The loader, driven the way an application drives it: add the root, read
// pending, add each, repeat, finish. Nothing here touches a filesystem.

#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

struct Doc {
  std::string_view name;
  std::string_view text;
};

struct Loaded {
  Loader loader;
  Chart chart;
  std::vector<Diagnostic> diags;
  std::vector<std::string> fetched;  // arrival order, for the ordering tests
  bool ok;
};

std::string_view body_of(std::vector<Doc> const &corpus, std::string_view name) {
  for (Doc const &d : corpus) {
    if (d.name == name) { return d.text; }
  }
  return {};
}

// `reverse` resolves each pending batch back to front, so a model that varies
// with arrival order shows up as a mismatch rather than passing quietly.
Loaded load_network(std::vector<Doc> const &corpus,
                    std::string_view root,
                    bool reverse = false) {
  Loaded out;
  std::string_view const root_text{ body_of(corpus, root) };
  out.fetched.emplace_back(root);
  if (!load_add(out.loader, raw(root_text), root_text.size(), root)) {
    out.ok = load_finish(out.loader, out.chart, out.diags);
    return out;
  }

  std::vector<std::string> wanted;
  for (;;) {
    wanted.clear();
    for (Pending const &p : load_pending(out.loader)) {
      wanted.emplace_back(load_pending_path(out.loader, p));
    }
    if (wanted.empty()) { break; }
    if (reverse) {
      for (size_t i = wanted.size() / 2; i-- > 0;) {
        std::swap(wanted[i], wanted[wanted.size() - 1 - i]);
      }
    }
    bool advanced{ false };
    for (std::string const &want : wanted) {
      std::string_view const text{ body_of(corpus, want) };
      if (text.empty()) { continue; }  // a document the corpus does not have
      out.fetched.push_back(want);
      advanced = true;
      if (!load_add(out.loader, raw(text), text.size(), want)) {
        out.ok = load_finish(out.loader, out.chart, out.diags);
        return out;
      }
    }
    if (!advanced) { break; }  // nothing left that can be supplied
  }
  out.ok = load_finish(out.loader, out.chart, out.diags);
  return out;
}

std::vector<std::string> document_names(Chart const &c) {
  std::vector<std::string> out;
  out.reserve(c.documents.size());
  for (Document const &d : c.documents) { out.emplace_back(chart_string(c, d.path)); }
  return out;
}

// A diamond with a repeat: `root` includes `mid` and `leaf`, `mid` includes
// `leaf` too, so leaf is one document and two instantiations.
std::vector<Doc> diamond() {
  return {
    { "root.scav",
      R"(chart root {
           include "mid.scav" as mid,
           include "leaf.scav" as leaf,
           state A,
           trans * -> A,
           trans A -> mid/M,
         })" },
    { "mid.scav",
      R"(chart mid {
           include "leaf.scav" as inner,
           state M,
           trans * -> M,
           trans M -> inner/L,
         })" },
    { "leaf.scav", R"(chart leaf { state L, trans * -> L, })" },
  };
}

uint32_t count_live_named(Chart const &c, std::string_view name) {
  uint32_t n{ 0 };
  for (State const &s : c.states) {
    if ((s.live != 0) && (chart_string(c, s.name) == name)) { ++n; }
  }
  return n;
}

}  // namespace

TEST_CASE("load: one document with no includes matches lower_document") {
  constexpr std::string_view TEXT{ "chart c { state A, state B, trans A -> B, }" };
  Loaded r{ load_network({ { "c.scav", TEXT } }, "c.scav") };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));

  Chart direct;
  std::vector<Diagnostic> diags;
  Parsed const p{ parse(TEXT, "c.scav") };
  REQUIRE(p.ok);
  REQUIRE(lower_document(direct, p.pd, diags));

  // The loader is the same three steps with N == 1, so it had better agree.
  CHECK(chart_structural_hash(r.chart) == chart_structural_hash(direct));
}

TEST_CASE("load: a two-document network resolves its alias and its endpoints") {
  Loaded r{ load_network(
      { { "a.scav",
          R"(chart a { include "b.scav" as b, state A, trans * -> A, trans A -> b/B, })" },
        { "b.scav", R"(chart b { state B, trans * -> B, })" } },
      "a.scav") };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));

  std::vector<Diagnostic> diags;
  REQUIRE(validate_chart(r.chart, diags));
  REQUIRE(r.chart.includes.size() == 1);

  Include const &inc{ r.chart.includes[0] };
  CHECK(chart_string(r.chart, inc.alias) == "b");
  CHECK(chart_string(r.chart, inc.path) == "b.scav");
  CHECK(inc.target == DocId{ 1 });

  // Resolution links, it does not flatten: the included root is a submachine
  // of the alias state, and the containment tree spans both documents.
  State const &host{ r.chart.states[inc.host.v] };
  REQUIRE(host.submachines.len == 1);
  SubmachineId const attached{ r.chart.submachine_ids[host.submachines.off] };
  CHECK(r.chart.submachines[attached.v].owner == inc.host);

  StateId found{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "b/B", found) ==
          ResolveStatus::Ok);
  CHECK(path(r.chart, found) == "b/B");
}

TEST_CASE("load: an included file is parsed once and instantiated per include") {
  Loaded r{ load_network(diamond(), "root.scav") };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));

  // Three files, so three documents and three statement runs, however many
  // times any of them is instantiated.
  CHECK(r.chart.documents.size() == 3);
  CHECK(document_names(r.chart) ==
        std::vector<std::string>{ "root.scav", "mid.scav", "leaf.scav" });

  // leaf is included twice, so `L` exists twice with disjoint rows.
  CHECK(count_live_named(r.chart, "L") == 2);
  CHECK(r.chart.includes.size() == 3);
}

TEST_CASE("load: DocId comes from the include graph, never from arrival order") {
  // What lets a host resolve a pending batch concurrently.
  Loaded const forward{ load_network(diamond(), "root.scav", false) };
  Loaded const backward{ load_network(diamond(), "root.scav", true) };
  REQUIRE(forward.ok);
  REQUIRE(backward.ok);

  CHECK(forward.fetched != backward.fetched);  // the orders really did differ
  CHECK(document_names(forward.chart) == document_names(backward.chart));
  CHECK(chart_structural_hash(forward.chart) == chart_structural_hash(backward.chart));

  std::vector<scav_byte> a;
  std::vector<scav_byte> b;
  chart_digest_bytes(forward.chart, a);
  chart_digest_bytes(backward.chart, b);
  CHECK(a == b);
}

TEST_CASE("load: pending names the path, the requester, and its statement") {
  Loader s;
  constexpr std::string_view ROOT{ R"(chart a { include "sub/b.scav" as b, state A, })" };
  REQUIRE(load_add(s, raw(ROOT), ROOT.size(), "top/a.scav"));

  std::vector<Pending> const &pending{ load_pending(s) };
  REQUIRE(pending.size() == 1);
  // Resolved against the requesting document's directory, so the application
  // is told what to fetch rather than what was written.
  CHECK(load_pending_path(s, pending[0]) == "top/sub/b.scav");
  CHECK(pending[0].from == DocId{ 0 });
  CHECK(s.parsed[0].stmts[pending[0].stmt_row].kind == StmtKind::Include);
}

TEST_CASE("load: pending empties as documents arrive") {
  Loader s;
  constexpr std::string_view ROOT{ R"(chart a { include "b.scav" as b, state A, })" };
  constexpr std::string_view SUB{ R"(chart b { state B, })" };
  REQUIRE(load_add(s, raw(ROOT), ROOT.size(), "a.scav"));
  CHECK(load_pending(s).size() == 1);
  REQUIRE(load_add(s, raw(SUB), SUB.size(), "b.scav"));
  CHECK(load_pending(s).empty());
}

TEST_CASE("load: two spellings of one path claim one document") {
  Loaded r{ load_network(
      { { "a.scav",
          R"(chart a { include "./b.scav" as x, include "sub/../b.scav" as y,
                       state A, })" },
        { "b.scav", R"(chart b { state B, })" } },
      "a.scav") };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));
  CHECK(r.chart.documents.size() == 2);
  CHECK(r.chart.includes.size() == 2);
  CHECK(r.chart.includes[0].target == r.chart.includes[1].target);
  // The authored text survives verbatim even though the key is shared: the
  // printer reprints what was written, not what it resolved to.
  CHECK(chart_string(r.chart, r.chart.includes[0].path) == "./b.scav");
  CHECK(chart_string(r.chart, r.chart.includes[1].path) == "sub/../b.scav");
}

TEST_CASE("load: entities carry the instantiation they belong to") {
  Loaded r{ load_network(diamond(), "root.scav") };
  REQUIRE(r.ok);

  for (State const &s : r.chart.states) {
    std::string_view const name{ chart_string(r.chart, s.name) };
    if (name == "A") { CHECK(s.inst.v == INVALID); }     // the root document
    if (name == "mid") { CHECK(s.inst.v == INVALID); }   // an alias host is the
    if (name == "leaf") { CHECK(s.inst.v == INVALID); }  // requester's own state
    if (name == "M") { CHECK(s.inst.v != INVALID); }
  }

  // The two `L` rows belong to different instantiations, which is the only
  // thing distinguishing them.
  std::vector<uint32_t> insts;
  for (State const &s : r.chart.states) {
    if (chart_string(r.chart, s.name) == "L") { insts.push_back(s.inst.v); }
  }
  REQUIRE(insts.size() == 2);
  CHECK(insts[0] != insts[1]);
  for (uint32_t const inst : insts) { CHECK(inst < r.chart.includes.size()); }
}

TEST_CASE("load: an included root submachine is unnamed and keeps its label") {
  // Naming it after the sub-chart would make `Host:leaf/L` a legal address
  // spelled from a word nobody wrote as a submachine name.
  Loaded r{ load_network({ { "a.scav", R"(chart a { include "b.scav" as b, state A, })" },
                           { "b.scav", R"(chart b "the label" { state B, })" } },
                         "a.scav") };
  REQUIRE(r.ok);

  StateId const host{ r.chart.includes[0].host };
  SubmachineId const root{
    r.chart.submachine_ids[r.chart.states[host.v].submachines.off]
  };
  Submachine const &m{ r.chart.submachines[root.v] };
  CHECK(m.name.len == 0);
  CHECK(chart_string(r.chart, m.label) == "the label");
  CHECK(m.stmt.v != INVALID);
  // The sub-chart's name is one `stmt` hop away, which is the whole point of
  // carrying provenance.
  CHECK(r.chart.stmts[m.stmt.v].kind == StmtKind::Chart);
  // And the model still has exactly one chart name -- the root's.
  CHECK(chart_string(r.chart, r.chart.name) == "a");
}

TEST_CASE("load: a sub-document's chart attrs land on its root submachine") {
  // A model holds exactly one Chart entity and it belongs to the root
  // document, so an included chart's attrs cannot attach there.
  Loaded r{ load_network(
      { { "a.scav", R"(chart a { @top = "1", include "b.scav" as b, })" },
        { "b.scav", R"(chart b { @sub = "2", state B, })" } },
      "a.scav") };
  REQUIRE(r.ok);

  CHECK(chart_attr_find(r.chart, chart_ref(), "top") != INVALID);
  CHECK(chart_attr_find(r.chart, chart_ref(), "sub") == INVALID);

  StateId const host{ r.chart.includes[0].host };
  SubmachineId const root{
    r.chart.submachine_ids[r.chart.states[host.v].submachines.off]
  };
  CHECK(chart_attr_find(r.chart, ref(root), "sub") != INVALID);
}

TEST_CASE("load: a path descends through an include once it is attached") {
  Loaded r{ load_network(diamond(), "root.scav") };
  REQUIRE(r.ok);

  StateId found{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "mid/M", found) ==
          ResolveStatus::Ok);
  CHECK(path(r.chart, found) == "mid/M");

  // Two levels down, through two documents.
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "mid/inner/L", found) ==
          ResolveStatus::Ok);
  CHECK(path(r.chart, found) == "mid/inner/L");

  // And the other instantiation is a different row at a different address.
  StateId other{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "leaf/L", other) ==
          ResolveStatus::Ok);
  CHECK(other != found);
}

TEST_CASE("load: every live state's address round-trips across the network") {
  Loaded r{ load_network(diamond(), "root.scav") };
  REQUIRE(r.ok);
  std::vector<Diagnostic> diags;
  REQUIRE(validate_chart(r.chart, diags));

  for (uint32_t i = 0; i < r.chart.states.size(); ++i) {
    if (r.chart.states[i].live == 0) { continue; }
    std::string const address{ path(r.chart, StateId{ i }) };
    CAPTURE(address);
    StateId back{ INVALID };
    CHECK(resolve_path(r.chart, r.chart.root_submachine, address, back) ==
          ResolveStatus::Ok);
    CHECK(back == StateId{ i });
  }
}

TEST_CASE("load: an include cycle is refused and names the closing statement") {
  Loaded r{ load_network(
      { { "a.scav", R"(chart a { include "b.scav" as b, state A, })" },
        { "b.scav", R"(chart b { state B, include "a.scav" as a, })" } },
      "a.scav") };
  CHECK_FALSE(r.ok);
  CHECK(has_code(r.diags, DiagCode::IncludeCycle));
  // No chart at all: a network that cannot be built is not a partial model.
  CHECK(r.chart.documents.empty());
  CHECK(r.chart.states.empty());

  for (Diagnostic const &d : r.diags) {
    if (d.code != DiagCode::IncludeCycle) { continue; }
    CHECK(load_document_name(r.loader, d.doc) == "b.scav");
    scav_byte const *bytes{ nullptr };
    uint32_t len{ 0 };
    REQUIRE(load_document_bytes(r.loader, d.doc, &bytes, &len));
    CHECK((static_cast<size_t>(d.src.off) + d.src.len) <= len);
  }
}

TEST_CASE("load: a document that includes itself is a cycle") {
  Loaded r{ load_network(
      { { "a.scav", R"(chart a { include "a.scav" as me, state A, })" } },
      "a.scav") };
  CHECK_FALSE(r.ok);
  CHECK(has_code(r.diags, DiagCode::IncludeCycle));
}

TEST_CASE("load: a longer cycle is caught too") {
  Loaded r{ load_network({ { "a.scav", R"(chart a { include "b.scav" as b, })" },
                           { "b.scav", R"(chart b { include "c.scav" as c, })" },
                           { "c.scav", R"(chart c { include "a.scav" as a, })" } },
                         "a.scav") };
  CHECK_FALSE(r.ok);
  CHECK(has_code(r.diags, DiagCode::IncludeCycle));
}

TEST_CASE("load: a diamond is not a cycle") {
  // Two paths to one document is legal and common; only a back edge is not.
  Loaded r{ load_network(diamond(), "root.scav") };
  CHECK(r.ok);
  CHECK_FALSE(has_code(r.diags, DiagCode::IncludeCycle));
}

TEST_CASE("load: finishing with a document still pending is refused") {
  Loader s;
  constexpr std::string_view ROOT{ R"(chart a { include "b.scav" as b, state A, })" };
  REQUIRE(load_add(s, raw(ROOT), ROOT.size(), "a.scav"));

  Chart c;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(load_finish(s, c, diags));
  CHECK(has_code(diags, DiagCode::IncludePathUnresolved));
  CHECK(c.documents.empty());
  // Reported against the requesting document, where the statement to fix is.
  CHECK(diags[0].doc == DocId{ 0 });
  CHECK(diags[0].src.len != 0);
}

TEST_CASE("load: an include path that names no document is refused at add") {
  Loader s;
  constexpr std::string_view ROOT{ R"(chart a { include "sub/" as b, state A, })" };
  CHECK_FALSE(load_add(s, raw(ROOT), ROOT.size(), "a.scav"));
  CHECK(has_code(s.diags, DiagCode::IncludePathInvalid));

  Chart c;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(load_finish(s, c, diags));
  CHECK(has_code(diags, DiagCode::IncludePathInvalid));
  CHECK(c.documents.empty());
}

TEST_CASE("load: an unrequested document is refused") {
  Loader s;
  constexpr std::string_view ROOT{ R"(chart a { state A, })" };
  constexpr std::string_view OTHER{ R"(chart z { state Z, })" };
  REQUIRE(load_add(s, raw(ROOT), ROOT.size(), "a.scav"));
  CHECK_FALSE(load_add(s, raw(OTHER), OTHER.size(), "z.scav"));
  CHECK(has_code(s.diags, DiagCode::DocumentNotRequested));
}

TEST_CASE("load: adding one document twice is refused") {
  Loader s;
  constexpr std::string_view ROOT{ R"(chart a { include "b.scav" as b, })" };
  constexpr std::string_view SUB{ R"(chart b { state B, })" };
  REQUIRE(load_add(s, raw(ROOT), ROOT.size(), "a.scav"));
  REQUIRE(load_add(s, raw(SUB), SUB.size(), "b.scav"));
  CHECK_FALSE(load_add(s, raw(SUB), SUB.size(), "b.scav"));
  CHECK(has_code(s.diags, DiagCode::DocumentAlreadyLoaded));
}

TEST_CASE("load: a loader with no root finishes with nothing") {
  Loader s;
  Chart c;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(load_finish(s, c, diags));
  CHECK(has_code(diags, DiagCode::LoaderEmpty));
}

TEST_CASE("load: finishing into a chart that already holds a model is refused") {
  Loader s;
  constexpr std::string_view ROOT{ R"(chart a { state A, })" };
  REQUIRE(load_add(s, raw(ROOT), ROOT.size(), "a.scav"));

  Chart c;
  std::ignore = build_chart(c, "already", {});
  std::vector<Diagnostic> diags;
  CHECK_FALSE(load_finish(s, c, diags));
  CHECK(has_code(diags, DiagCode::LoaderEmpty));
}

TEST_CASE("load: a parse error in an included document names that document") {
  Loaded r{ load_network({ { "a.scav", R"(chart a { include "b.scav" as b, state A, })" },
                           { "b.scav", "chart b { state , }" } },
                         "a.scav") };
  CHECK_FALSE(r.ok);
  CHECK(r.chart.documents.empty());
  // parse_document holds one document and is not told which; the loader is
  // the layer that knows, so it stamps the DocId.
  REQUIRE_FALSE(r.diags.empty());
  CHECK(r.diags[0].doc == DocId{ 1 });
  CHECK(load_document_name(r.loader, r.diags[0].doc) == "b.scav");
  // And the bytes survive the failure, so the finding can be located.
  scav_byte const *bytes{ nullptr };
  uint32_t len{ 0 };
  CHECK(load_document_bytes(r.loader, r.diags[0].doc, &bytes, &len));
  CHECK(len != 0);
}

TEST_CASE("load: an alias colliding with a sibling state is an ordinary duplicate") {
  // An alias is a state, so the ordinary duplicate-name check covers it.
  Loaded r{ load_network({ { "a.scav", R"(chart a { include "b.scav" as b, state b, })" },
                           { "b.scav", R"(chart b { state B, })" } },
                         "a.scav") };
  REQUIRE(r.ok);  // structurally loadable; validation is what objects
  std::vector<Diagnostic> diags;
  CHECK_FALSE(validate_chart(r.chart, diags));
  CHECK(has_code(diags, DiagCode::DuplicateName));
}

TEST_CASE("load: an endpoint naming nothing across an include still diagnoses") {
  Loaded r{ load_network(
      { { "a.scav", R"(chart a { include "b.scav" as b, state A, trans A -> b/Nope, })" },
        { "b.scav", R"(chart b { state B, })" } },
      "a.scav") };
  CHECK_FALSE(r.ok);
  CHECK(has_code(r.diags, DiagCode::EndpointUnresolved));
  // The chart exists: this is a finding about a statement, not a failure to
  // build the network, so the rows are there to look at.
  CHECK_FALSE(r.chart.documents.empty());
  CHECK(r.chart.documents.size() == 2);
}

TEST_CASE("load: an included document sees its own scope, not the host's") {
  // Lexical scoping is per document: `A` in the leaf must not find the root's.
  Loaded r{ load_network({ { "a.scav", R"(chart a { include "b.scav" as b, state A, })" },
                           { "b.scav", R"(chart b { state B, trans B -> A, })" } },
                         "a.scav") };
  CHECK_FALSE(r.ok);
  CHECK(has_code(r.diags, DiagCode::EndpointUnresolved));
}

TEST_CASE("load: the loader is reusable state, not a one-shot object") {
  // Two independent loaders in one process: no library-global state.
  Loaded a{ load_network(diamond(), "root.scav") };
  Loaded b{ load_network(diamond(), "root.scav") };
  REQUIRE(a.ok);
  REQUIRE(b.ok);
  CHECK(chart_structural_hash(a.chart) == chart_structural_hash(b.chart));
}

TEST_CASE("load: one document included three times from one parent") {
  // Three aliases in one submachine, all naming one file. One Document, three
  // instantiations, and three disjoint sets of rows addressed apart.
  Loaded r{ load_network(
      { { "mill.scav",
          R"(chart mill {
               include "axis.scav" as x,
               include "axis.scav" as y,
               include "axis.scav" as z,
               state Ready,
               trans * -> Ready,
               trans Ready -> x/Homing,
               trans Ready -> z/Homing,
             })" },
        { "axis.scav",
          R"(chart axis { state Parked, state Homing, trans * -> Parked, })" } },
      "mill.scav") };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));

  std::vector<Diagnostic> diags;
  REQUIRE(validate_chart(r.chart, diags));

  CHECK(r.chart.documents.size() == 2);
  REQUIRE(r.chart.includes.size() == 3);
  CHECK(count_live_named(r.chart, "Homing") == 3);

  // One target, three instantiations, three hosts.
  CHECK(r.chart.includes[0].target == r.chart.includes[1].target);
  CHECK(r.chart.includes[1].target == r.chart.includes[2].target);
  CHECK(r.chart.includes[0].host != r.chart.includes[1].host);
  CHECK(r.chart.includes[1].host != r.chart.includes[2].host);

  // Each alias addresses its own copy, and the two endpoints that named one
  // landed on different rows.
  StateId x{ INVALID };
  StateId y{ INVALID };
  StateId z{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "x/Homing", x) ==
          ResolveStatus::Ok);
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "y/Homing", y) ==
          ResolveStatus::Ok);
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "z/Homing", z) ==
          ResolveStatus::Ok);
  CHECK(x != y);
  CHECK(y != z);
  CHECK(r.chart.states[x.v].inst != r.chart.states[y.v].inst);
}

TEST_CASE("load: a path descends through three include boundaries") {
  Loaded r{ load_network({ { "a.scav", R"(chart a { include "b.scav" as b, state A,
                                 trans A -> b/c/d/D, })" },
                           { "b.scav", R"(chart b { include "c.scav" as c, state B, })" },
                           { "c.scav", R"(chart c { include "d.scav" as d, state C, })" },
                           { "d.scav", R"(chart d { state D, })" } },
                         "a.scav") };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));

  StateId deep{ INVALID };
  REQUIRE(resolve_path(r.chart, r.chart.root_submachine, "b/c/d/D", deep) ==
          ResolveStatus::Ok);
  CHECK(path(r.chart, deep) == "b/c/d/D");
}

TEST_CASE("load: one leaf reached from several including documents") {
  // `leaf` is named by the root and by both middles, so it is one Document and
  // three instantiations whose rows never mix.
  Loaded r{ load_network(
      { { "root.scav",
          R"(chart root {
               include "one.scav" as one,
               include "two.scav" as two,
               include "leaf.scav" as leaf,
               state R,
             })" },
        { "one.scav", R"(chart one { include "leaf.scav" as leaf, state O, })" },
        { "two.scav", R"(chart two { include "leaf.scav" as leaf, state T, })" },
        { "leaf.scav", R"(chart leaf { state L, trans * -> L, })" } },
      "root.scav") };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));

  CHECK(r.chart.documents.size() == 4);
  CHECK(r.chart.includes.size() == 5);
  CHECK(count_live_named(r.chart, "L") == 3);

  for (char const *address : { "leaf/L", "one/leaf/L", "two/leaf/L" }) {
    StateId found{ INVALID };
    CAPTURE(address);
    REQUIRE(resolve_path(r.chart, r.chart.root_submachine, address, found) ==
            ResolveStatus::Ok);
    CHECK(path(r.chart, found) == address);
  }
}
