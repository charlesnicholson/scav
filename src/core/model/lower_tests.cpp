// Corpus charts become entity arrays, placement rules fire, wildcards
// synthesize, and every entity walks back to its declaring statement.

#include "core/core_internal.h"
#include "core/model/model.h"
#include "core/tests/test_charts.h"
#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

struct Lowered {
  Chart c;
  std::vector<Diagnostic> diags;
  bool parsed;
  bool clean;
};

Lowered lower(std::string_view text, std::string_view name = "test.scav") {
  Lowered r;
  Parsed const p{ parse(text, name) };
  r.parsed = p.ok;
  if (!p.ok) {
    r.clean = false;
    return r;
  }
  r.clean = lower_document(r.c, p.pd, r.diags);
  return r;
}

uint32_t live_count_states(Chart const &c) {
  uint32_t n{ 0 };
  for (State const &s : c.states) {
    if (s.live != 0) { ++n; }
  }
  return n;
}

std::string_view src_text(Chart const &c, Span span) {
  if (span.len == 0) { return {}; }
  return { reinterpret_cast<char const *>(c.src_bytes.data() + span.off), span.len };
}

// The statement's source text, so provenance assertions read like the chart.
std::string_view stmt_text(Chart const &c, StmtId stmt) {
  return src_text(c, c.stmts[stmt.v].src);
}

bool validates(Chart const &c) {
  std::vector<Diagnostic> diags;
  return validate_chart(c, diags);
}

}  // namespace

TEST_CASE("lower: tcp -- every entity, resolved in-document") {
  Lowered r{ lower(TCP, "tcp.scav") };
  REQUIRE(r.parsed);
  CHECK(r.clean);
  CHECK(r.diags.empty());
  // 11 authored states plus a `$initial` each in root, inbound and outbound.
  // Established's block holds only explicit submachines, so it gains none.
  CHECK(live_count_states(r.c) == 14);
  CHECK(r.c.submachines.size() == 3);
  CHECK(r.c.transitions.size() == 15);
  CHECK(r.c.attrs.size() == 1);  // the chart @doc
  CHECK(r.c.includes.empty());
  CHECK(validates(r.c));
  CHECK(chart_string(r.c, r.c.name) == "tcp");
  CHECK(chart_string(r.c, r.c.label) == "TCP connection");

  // The long hierarchical edge resolved through its qualifier.
  StateId half_closed{ INVALID };
  REQUIRE(resolve_path(r.c,
                       r.c.root_submachine,
                       "Established:inbound/HalfClosed",
                       half_closed) == ResolveStatus::Ok);
  StateId closing{ INVALID };
  REQUIRE(resolve_path(r.c, r.c.root_submachine, "Closing", closing) == ResolveStatus::Ok);
  bool found{ false };
  for (Transition const &t : r.c.transitions) {
    if ((t.src == half_closed) && (t.dst == closing)) { found = true; }
  }
  CHECK(found);
}

TEST_CASE("lower: ota -- aliases, fork/join, a raw label, and a final state") {
  Lowered r{ lower(OTA, "ota.scav") };
  REQUIRE(r.parsed);
  CHECK(r.clean);
  // 9 authored + 2 initials (root, main) + 1 final (`Failed -> *`).
  CHECK(live_count_states(r.c) == 12);
  CHECK(r.c.submachines.size() == 2);
  CHECK(r.c.transitions.size() == 16);
  // @vendor:component + two @tags list rows + Downloading's @doc.
  CHECK(r.c.attrs.size() == 4);
  CHECK(validates(r.c));

  AttrKeyId const tags{ chart_attr_key_find(r.c, "tags") };
  REQUIRE(tags.v != INVALID);
  Span const chart_attrs{ chart_attrs_of(r.c, chart_ref()) };
  uint32_t rows{ 0 };
  for (uint32_t i = 0; i < chart_attrs.len; ++i) {
    if (r.c.attrs[chart_attrs.off + i].key == tags) { ++rows; }
  }
  CHECK(rows == 2);  // a list is one row per value under one key

  StateId final_state{ INVALID };
  REQUIRE(resolve_path(r.c, r.c.root_submachine, "$final", final_state) ==
          ResolveStatus::Ok);
  CHECK(r.c.states[final_state.v].kind == StateKind::Final);
}

TEST_CASE("lower: vac -- an include lowers to its host; crossing it diagnoses") {
  Lowered r{ lower(VAC, "vac.scav") };
  REQUIRE(r.parsed);
  // `trans Ready -> dock/On/Seated` descends past an include that lowering
  // alone leaves unresolved.
  CHECK_FALSE(r.clean);
  REQUIRE(r.diags.size() == 1);
  CHECK(r.diags[0].code == DiagCode::EndpointCrossesInclude);
  // The diagnostic's span quotes the offending statement.
  std::string_view const quoted{ src_text(r.c, r.diags[0].src) };
  CHECK(quoted.find("dock/On/Seated") != std::string_view::npos);

  // 7 authored + the dock host + 3 initials; root/main/aux submachines.
  CHECK(live_count_states(r.c) == 11);
  CHECK(r.c.submachines.size() == 3);
  CHECK(r.c.transitions.size() == 5);  // 6 authored, 1 skipped
  REQUIRE(r.c.includes.size() == 1);
  CHECK(r.c.includes[0].target.v == INVALID);
  CHECK(chart_string(r.c, r.c.includes[0].alias) == "dock");
  CHECK(path(r.c, r.c.includes[0].host) == "dock");
  CHECK(validates(r.c));

  StateId idle{ INVALID };
  CHECK(resolve_path(r.c, r.c.root_submachine, "On:main/Idle", idle) == ResolveStatus::Ok);
  CHECK(resolve_path(r.c, r.c.root_submachine, "On:aux/Idle", idle) == ResolveStatus::Ok);
}

TEST_CASE("lower: every entity's stmt walks back to the text that declared it") {
  Lowered r{ lower(TCP, "tcp.scav") };
  REQUIRE(r.parsed);
  for (State const &s : r.c.states) {
    REQUIRE(s.stmt.v != INVALID);
    std::string_view const text{ stmt_text(r.c, s.stmt) };
    if (s.name.len != 0) {
      // The declaring statement quotes the authored name.
      CHECK(text.find(chart_string(r.c, s.name)) != std::string_view::npos);
    } else {
      // A synthesized pseudostate points at the trans statement whose `*`
      // created it.
      CHECK(text.find('*') != std::string_view::npos);
    }
  }
  for (Transition const &t : r.c.transitions) {
    CHECK(stmt_text(r.c, t.stmt).find("trans") == 0);
  }
  // The document row covers the whole normalized source.
  REQUIRE(r.c.documents.size() == 1);
  CHECK(r.c.documents[0].text.len == r.c.src_bytes.size());
  CHECK(r.c.documents[0].statements.len == r.c.stmts.size());
  CHECK(chart_string(r.c, r.c.documents[0].path) == "tcp.scav");
}

TEST_CASE("lower: the implicit submachine is ordinal 0 wherever it is written") {
  // Explicit written first; the direct state still lands in ordinal 0.
  Lowered r{ lower("chart c { state On { submachine m { state B }, state A } }") };
  REQUIRE(r.parsed);
  CHECK(r.clean);
  StateId a{ INVALID };
  REQUIRE(resolve_path(r.c, r.c.root_submachine, "On:0/A", a) == ResolveStatus::Ok);
  CHECK(path(r.c, a) == "On:0/A");
  StateId b{ INVALID };
  REQUIRE(resolve_path(r.c, r.c.root_submachine, "On:m/B", b) == ResolveStatus::Ok);
  // The implicit one is unnamed and first; the explicit one is ordinal 1.
  StateId const on{ r.c.states[a.v].parent.v == INVALID
                        ? StateId{ INVALID }
                        : r.c.submachines[r.c.states[a.v].parent.v].owner };
  REQUIRE(on.v != INVALID);
  Span const subs{ r.c.states[on.v].submachines };
  REQUIRE(subs.len == 2);
  CHECK(r.c.submachines[r.c.submachine_ids[subs.off].v].ordinal == 0);
  CHECK(r.c.submachines[r.c.submachine_ids[subs.off].v].name.len == 0);
  CHECK(r.c.submachines[r.c.submachine_ids[subs.off + 1].v].ordinal == 1);
}

TEST_CASE("lower: no implicit submachine when a block holds only explicit ones") {
  Lowered r{ lower(
      "chart c { state On { submachine a { state X }, submachine b { state Y } } }") };
  REQUIRE(r.parsed);
  CHECK(r.clean);
  StateId on{ INVALID };
  REQUIRE(resolve_path(r.c, r.c.root_submachine, "On", on) == ResolveStatus::Ok);
  Span const subs{ r.c.states[on.v].submachines };
  REQUIRE(subs.len == 2);
  CHECK(r.c.submachines[r.c.submachine_ids[subs.off].v].ordinal == 0);
  CHECK(chart_string(r.c, r.c.submachines[r.c.submachine_ids[subs.off].v].name) == "a");
}

TEST_CASE("lower: placement rules fire where the parser deliberately does not") {
  SUBCASE("a submachine directly in a chart block") {
    Lowered r{ lower("chart c { submachine m { state A } }") };
    REQUIRE(r.parsed);
    CHECK_FALSE(r.clean);
    REQUIRE_FALSE(r.diags.empty());
    CHECK(r.diags[0].code == DiagCode::MisplacedStatement);
  }
  SUBCASE("a submachine directly in a submachine block") {
    Lowered r{ lower(
        "chart c { state S { submachine m { submachine n { state A } } } }") };
    REQUIRE(r.parsed);
    CHECK_FALSE(r.clean);
    CHECK(r.diags[0].code == DiagCode::MisplacedStatement);
  }
  SUBCASE("anything but an attr in a trans block") {
    Lowered r{ lower(
        R"(chart c { state A, state B, trans A -> B { state X, @w = "1" } })") };
    REQUIRE(r.parsed);
    CHECK_FALSE(r.clean);
    REQUIRE(r.diags.size() == 1);
    CHECK(r.diags[0].code == DiagCode::MisplacedStatement);
    // The attr still applied; the misplaced state was skipped, not the block.
    REQUIRE(r.c.transitions.size() == 1);
    CHECK(chart_attr_find(r.c, ref(TransId{ 0 }), "w") != INVALID);
    StateId x{ INVALID };
    CHECK(resolve_path(r.c, r.c.root_submachine, "X", x) == ResolveStatus::NotFound);
  }
}

TEST_CASE("lower: wildcard endpoints") {
  SUBCASE("* to * is refused") {
    Lowered r{ lower("chart c { trans * -> * }") };
    REQUIRE(r.parsed);
    CHECK_FALSE(r.clean);
    REQUIRE(r.diags.size() == 1);
    CHECK(r.diags[0].code == DiagCode::WildcardBothEndpoints);
    CHECK(r.c.transitions.empty());
    CHECK(live_count_states(r.c) == 0);  // no orphan pseudostates
  }
  SUBCASE("two wildcard sources are two initials, which validation flags") {
    Lowered r{ lower("chart c { state A, state B, trans * -> A, trans * -> B }") };
    REQUIRE(r.parsed);
    CHECK(r.clean);
    std::vector<Diagnostic> diags;
    CHECK_FALSE(validate_chart(r.c, diags));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == DiagCode::MultipleInitial);
  }
  SUBCASE("a failed path skips the statement without synthesizing") {
    Lowered r{ lower("chart c { trans * -> Nowhere }") };
    REQUIRE(r.parsed);
    CHECK_FALSE(r.clean);
    CHECK(r.diags[0].code == DiagCode::EndpointUnresolved);
    CHECK(live_count_states(r.c) == 0);
  }
}

TEST_CASE("lower: paths resolve lexically, nearest submachine first") {
  Lowered r{ lower(R"(chart c {
    state Idle,
    state Off,
    state On {
      submachine main {
        state Idle,
        state Ready,
        trans Ready -> Idle,
        trans Ready -> Off,
      },
    },
  })") };
  REQUIRE(r.parsed);
  CHECK(r.clean);
  StateId inner_idle{ INVALID };
  REQUIRE(resolve_path(r.c, r.c.root_submachine, "On/Idle", inner_idle) ==
          ResolveStatus::Ok);
  StateId outer_off{ INVALID };
  REQUIRE(resolve_path(r.c, r.c.root_submachine, "Off", outer_off) == ResolveStatus::Ok);
  REQUIRE(r.c.transitions.size() == 2);
  CHECK(r.c.transitions[0].dst == inner_idle);  // nearest Idle, not the root's
  CHECK(r.c.transitions[1].dst == outer_off);   // outward when absent locally
}

TEST_CASE("lower: qualifier failures carry their own code") {
  SUBCASE("ordinal past the submachines") {
    Lowered r{ lower(
        "chart c { state A, state On { submachine m { state B } }, "
        "trans A -> On:5/B }") };
    REQUIRE(r.parsed);
    CHECK_FALSE(r.clean);
    CHECK(r.diags[0].code == DiagCode::BadSubmachineQualifier);
  }
  SUBCASE("a qualifier on the final segment selects nothing") {
    Lowered r{ lower("chart c { state A, state B, trans A -> B:0 }") };
    REQUIRE(r.parsed);
    CHECK_FALSE(r.clean);
    CHECK(r.diags[0].code == DiagCode::BadSubmachineQualifier);
  }
}

TEST_CASE("lower: attr spellings all land as rows") {
  Lowered r{ lower(R"(chart c {
    @flag,
    @ns:key = "v",
    @ns { a, b = "c" },
    @list = ["x", "y"],
    state S { @doc = "d" },
  })") };
  REQUIRE(r.parsed);
  CHECK(r.clean);
  // flag + ns:key + ns:a + ns:b + two list rows on the chart; @doc on S.
  CHECK(chart_attrs_of(r.c, chart_ref()).len == 6);
  uint32_t const flag{ chart_attr_find(r.c, chart_ref(), "flag") };
  REQUIRE(flag != INVALID);
  CHECK(chart_string(r.c, r.c.attrs[flag].value) == "true");
  uint32_t const ns_a{ chart_attr_find(r.c, chart_ref(), "ns:a") };
  REQUIRE(ns_a != INVALID);
  CHECK(chart_string(r.c, r.c.attrs[ns_a].value) == "true");
  uint32_t const ns_b{ chart_attr_find(r.c, chart_ref(), "ns:b") };
  REQUIRE(ns_b != INVALID);
  CHECK(chart_string(r.c, r.c.attrs[ns_b].value) == "c");
  CHECK(chart_attr_find(r.c, ref(StateId{ 0 }), "doc") != INVALID);
  CHECK(validates(r.c));
}

TEST_CASE("lower: refuses a non-empty chart and a chartless parse") {
  Parsed const p{ parse("chart c { state A }") };
  REQUIRE(p.ok);
  Chart c;
  build_chart(c, "already", {});
  std::vector<Diagnostic> diags;
  CHECK_FALSE(lower_document(c, p.pd, diags));
  CHECK(diags.empty());
  CHECK(c.states.empty());

  ParsedDocument const empty_pd{};
  Chart fresh;
  CHECK_FALSE(lower_document(fresh, empty_pd, diags));
}

TEST_CASE("lower: any of the three child kinds earns a state its implicit submachine") {
  // A state block gets one as soon as it holds a state, a transition or an
  // include, and the answer comes from whichever of the three is met first.
  SUBCASE("a transition leads the block") {
    Lowered const r{ lower("chart c { state A, state On { trans A -> A, }, }") };
    REQUIRE(r.parsed);
    CHECK(r.clean);
    StateId on{ INVALID };
    REQUIRE(resolve_path(r.c, r.c.root_submachine, "On", on) == ResolveStatus::Ok);
    CHECK(r.c.states[on.v].submachines.len == 1);
  }
  SUBCASE("an include leads the block") {
    Lowered const r{ lower(R"(chart c { state On { include "w.scav" as w, }, })") };
    REQUIRE(r.parsed);
    CHECK(r.clean);
    StateId on{ INVALID };
    REQUIRE(resolve_path(r.c, r.c.root_submachine, "On", on) == ResolveStatus::Ok);
    CHECK(r.c.states[on.v].submachines.len == 1);
    StateId host{ INVALID };
    CHECK(resolve_path(r.c, r.c.root_submachine, "On/w", host) == ResolveStatus::Ok);
    REQUIRE(r.c.includes.size() == 1);
    CHECK(r.c.includes[0].host == host);
  }
  SUBCASE("only submachines and attrs earns none") {
    Lowered const r{ lower(
        R"(chart c { state On { @k, submachine m { state X, }, }, })") };
    REQUIRE(r.parsed);
    CHECK(r.clean);
    StateId on{ INVALID };
    REQUIRE(resolve_path(r.c, r.c.root_submachine, "On", on) == ResolveStatus::Ok);
    CHECK(r.c.states[on.v].submachines.len == 1);  // the authored one, not an implicit
    CHECK(chart_string(
              r.c,
              r.c.submachines[r.c.submachine_ids[r.c.states[on.v].submachines.off].v]
                  .name) == "m");
  }
}

TEST_CASE("lower: instantiating a root into a chart that has one adds nothing") {
  Parsed const p{ parse("chart c { state A, }") };
  REQUIRE(p.ok);
  Chart c;
  REQUIRE(build_chart(c, "already", {}).v == 0);
  std::vector<PendingTrans> trans;
  std::vector<PendingInc> incs;
  std::vector<Diagnostic> diags;
  InstJob const job{ .doc = { 0 },
                     .inst = { INVALID },
                     .host = { INVALID },
                     .stmt_base = 0 };
  CHECK(model_instantiate(c, p.pd, job, trans, incs, diags));
  CHECK(c.states.empty());
  CHECK(c.submachines.size() == 1);
  CHECK(chart_string(c, c.name) == "already");

  // And a document that parsed no chart statement has no root row to hang on.
  ParsedDocument const chartless{};
  Chart fresh;
  CHECK_FALSE(model_instantiate(fresh, chartless, job, trans, incs, diags));
  CHECK(fresh.submachines.empty());
}

TEST_CASE("lower: a chart already holding a document is refused before entities") {
  Parsed const p{ parse("chart c { state A, }") };
  REQUIRE(p.ok);
  Chart c;
  uint32_t base{ 0 };
  std::ignore = model_attach_document(c, p.pd, base);
  REQUIRE(c.documents.size() == 1);
  REQUIRE(c.submachines.empty());
  std::vector<Diagnostic> diags;
  CHECK_FALSE(lower_document(c, p.pd, diags));
  CHECK(c.states.empty());
  CHECK(c.documents.size() == 1);
}

TEST_CASE("lower: a parentless row is left out of the rebuilt spans, not written past") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  c.states[a.v].parent = SubmachineId{ INVALID };
  model_finalize_containment(c);

  Span const kids{ c.submachines[root.v].children };
  REQUIRE(kids.len == 1);
  CHECK(c.state_ids[kids.off] == b);
  // Validation is where the missing link is reported, one finding on the row.
  std::vector<Diagnostic> diags;
  CHECK_FALSE(validate_chart(c, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::MissingRequiredId);
  CHECK(diags[0].subject == ref(a));
}

TEST_CASE("lower: a pending transition naming an unsupplied document is skipped") {
  Parsed const p{ parse("chart c { state A, }") };
  REQUIRE(p.ok);
  Chart c;
  std::vector<Diagnostic> diags;
  REQUIRE(lower_document(c, p.pd, diags));
  REQUIRE(c.transitions.empty());

  std::array<ParsedDocument const *, 1> const docs{ &p.pd };
  std::vector<PendingTrans> const pending{ { .row = 0,
                                             .doc = { 5 },
                                             .inst = { INVALID },
                                             .scope = c.root_submachine,
                                             .stmt_base = 0 } };
  std::vector<Diagnostic> more;
  CHECK(model_resolve_transitions(c, docs.data(), 1, pending, more));
  CHECK(more.empty());
  CHECK(c.transitions.empty());
}

TEST_CASE("lower: a chart statement inside a block is refused where it stands") {
  // The grammar admits one chart and only as the root, so this shape is a
  // producer bug rather than authored text. The document is built by hand
  // because no source text reaches the arm that reports it.
  constexpr std::string_view SOURCE{ "chart c { state A { chart nested {} } }" };
  ParsedDocument pd;
  for (char const ch : SOURCE) { pd.src_bytes.push_back(static_cast<scav_byte>(ch)); }
  auto const span_of = [&](std::string_view text) {
    return make_span(narrow_clamp<uint32_t>(SOURCE.find(text)), size32(text));
  };
  Span const whole{ make_span(0, size32(SOURCE)) };

  pd.id = { 0 };
  pd.doc = { .path = string_pool_add(pd.strings, "hand.scav"),
             .text = whole,
             .statements = make_span(0, 3) };
  pd.stmts = {
    { .kind = StmtKind::Chart,
      .doc = { 0 },
      .src = whole,
      .comments = {},
      .blank_before = 0 },
    { .kind = StmtKind::State,
      .doc = { 0 },
      .src = span_of("state A { chart nested {} }"),
      .comments = {},
      .blank_before = 0 },
    { .kind = StmtKind::Chart,
      .doc = { 0 },
      .src = span_of("chart nested {}"),
      .comments = {},
      .blank_before = 0 },
  };
  pd.stmt_payload = { 0, 0, 1 };
  pd.stmt_children = { make_span(0, 1), make_span(1, 1), {} };
  pd.stmt_ids = { StmtId{ 1 }, StmtId{ 2 } };
  pd.charts = { { .name = string_pool_add(pd.strings, "c"), .label = {} },
                { .name = string_pool_add(pd.strings, "nested"), .label = {} } };
  pd.states = { { .name = string_pool_add(pd.strings, "A"),
                  .label = {},
                  .kind = StateKind::Normal,
                  .has_block = 1 } };

  Chart c;
  uint32_t base{ 0 };
  std::ignore = model_attach_document(c, pd, base);
  std::vector<PendingTrans> trans;
  std::vector<PendingInc> incs;
  std::vector<Diagnostic> diags;
  InstJob const job{ .doc = { 0 },
                     .inst = { INVALID },
                     .host = { INVALID },
                     .stmt_base = base };
  CHECK_FALSE(model_instantiate(c, pd, job, trans, incs, diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == DiagCode::MisplacedStatement);
  CHECK(diags[0].doc == DocId{ 0 });
  CHECK(src_text(c, diags[0].src) == "chart nested {}");
  // The state was created; only the statement inside it was refused, and the
  // block it opened earned no submachine.
  REQUIRE(c.states.size() == 1);
  CHECK(chart_string(c, c.states[0].name) == "A");
  CHECK(c.states[0].submachines.len == 0);
}
