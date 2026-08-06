// The grammar of PRD 15, statement by statement, plus the statement tree, the
// trivia attachment, the depth cap and every diagnostic the parser can emit.
//
// Charts are inline raw literals. A parser test that opens a file is testing
// the filesystem as well, and PRD 16.2's whole point is that nothing here needs
// one.

#include "core/lang/synth_document.h"
#include "core/test_support.h"
#include "scav/scav_diagnostics.h"
#include "scav/scav_ids.h"
#include "scav/scav_lexer.h"
#include "scav/scav_parser.h"
#include "scav/scav_string_pool.h"
#include "scav/scav_syntax_tree.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

// The children of a statement, as statement rows.
std::vector<uint32_t> children_of(ParsedDocument const &pd, uint32_t stmt) {
  std::vector<uint32_t> out;
  Span const span{ pd.stmt_children[stmt] };
  out.reserve(span.len);
  for (uint32_t i = 0; i < span.len; ++i) { out.push_back(pd.stmt_ids[span.off + i].v); }
  return out;
}

// A whole tree flattened depth-first, spelled as "kind:name", which is enough
// to assert shape without asserting row numbers.
std::vector<std::string> shape(ParsedDocument const &pd) {
  std::vector<std::string> out;
  std::vector<uint32_t> stack{ syntax_root_statement(pd) };
  std::vector<uint32_t> order;
  while (!stack.empty()) {
    uint32_t const stmt{ stack.back() };
    stack.pop_back();
    order.push_back(stmt);
    // Pushed in reverse so the depth-first walk pops them in document order.
    std::vector<uint32_t> const kids{ children_of(pd, stmt) };
    uint32_t remaining{ static_cast<uint32_t>(kids.size()) };
    while (remaining > 0) {
      --remaining;
      stack.push_back(kids[remaining]);
    }
  }
  out.reserve(order.size());
  for (uint32_t const stmt : order) {
    std::string line{ syntax_elem_kind_name(pd.stmts[stmt].kind) };
    switch (pd.stmts[stmt].kind) {
      case ElemKind::Chart:
        line += ":";
        line += str(pd, pd.charts[pd.stmt_payload[stmt]].name);
        break;
      case ElemKind::State:
        line += ":";
        line += str(pd, state_at(pd, stmt).name);
        break;
      case ElemKind::Submachine:
        line += ":";
        line += str(pd, submachine_at(pd, stmt).name);
        break;
      case ElemKind::Trans:
        line += ":";
        line += path_text(pd, trans_at(pd, stmt).src);
        line += "->";
        line += path_text(pd, trans_at(pd, stmt).dst);
        break;
      case ElemKind::Attr:
        line += ":";
        line += str(pd, pd.attr_entries[attr_at(pd, stmt).entries.off].key);
        break;
      case ElemKind::Include:
        line += ":";
        line += str(pd, include_at(pd, stmt).alias);
        break;
    }
    out.push_back(line);
  }
  return out;
}

// Comments owned by a statement, as (position, text) pairs.
std::vector<std::string> comments_of(ParsedDocument const &pd, uint32_t stmt) {
  static constexpr std::array<char const *, 3> NAMES{ "leading", "trailing", "own-line" };
  std::vector<std::string> out;
  Span const span{ pd.stmts[stmt].comments };
  out.reserve(span.len);
  for (uint32_t i = 0; i < span.len; ++i) {
    Trivia const &t{ pd.comments[span.off + i] };
    std::string line{ NAMES[static_cast<uint32_t>(t.pos)] };
    line += " ";
    line += src(pd, t.src);
    out.push_back(line);
  }
  return out;
}

}  // namespace

TEST_CASE("parse: the smallest legal chart") {
  Parsed const r{ parse("chart c {}") };
  REQUIRE(r.ok);
  CHECK(r.diags.empty());
  REQUIRE(r.pd.stmts.size() == 1);
  CHECK(r.pd.stmts[0].kind == ElemKind::Chart);
  CHECK(str(r.pd, r.pd.charts[0].name) == "c");
  CHECK(str(r.pd, r.pd.charts[0].label).empty());
  CHECK(r.pd.stmt_children[0].len == 0);
  CHECK(r.pd.doc.statements == make_span(0, 1));
  CHECK(str(r.pd, r.pd.doc.path) == "test.scav");
}

TEST_CASE("parse: a chart takes an optional label") {
  Parsed const r{ parse(R"(chart eg91 "EG91 modem driver" {})") };
  REQUIRE(r.ok);
  CHECK(str(r.pd, r.pd.charts[0].name) == "eg91");
  CHECK(str(r.pd, r.pd.charts[0].label) == "EG91 modem driver");
}

TEST_CASE("parse: the document must be a chart") {
  CHECK(first_code(parse("").diags) == DiagCode::ExpectedChart);
  CHECK(first_code(parse("state A {}").diags) == DiagCode::ExpectedChart);
  CHECK(first_code(parse("{}").diags) == DiagCode::ExpectedChart);
  CHECK(first_code(parse("chart").diags) == DiagCode::ExpectedIdentifier);
  CHECK(first_code(parse("chart c").diags) == DiagCode::ExpectedBlock);
}

TEST_CASE("parse: nothing may follow the chart") {
  Parsed const r{ parse("chart c {} state B,") };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::TrailingContent);
}

TEST_CASE("parse: trailing trivia after the chart is fine") {
  CHECK(parse("chart c {}\n// done\n").ok);
  CHECK(parse("chart c {}\n\n\n").ok);
}

TEST_CASE("parse: states directly in a block, with kinds and labels") {
  Parsed const r{ parse(R"(
chart c {
  state Off "modem powered down",
  state Booting,
  state PreConfig choice,
  state Deep deephistory "remembers",
}
)") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const states{ stmts_of(r.pd, ElemKind::State) };
  REQUIRE(states.size() == 4);
  CHECK(str(r.pd, state_at(r.pd, states[0]).name) == "Off");
  CHECK(str(r.pd, state_at(r.pd, states[0]).label) == "modem powered down");
  CHECK(state_at(r.pd, states[0]).kind == StateKind::Normal);
  CHECK(state_at(r.pd, states[1]).kind == StateKind::Normal);
  CHECK(state_at(r.pd, states[2]).kind == StateKind::Choice);
  CHECK(state_at(r.pd, states[3]).kind == StateKind::DeepHistory);
  CHECK(str(r.pd, state_at(r.pd, states[3]).label) == "remembers");
}

TEST_CASE("parse: every state kind the format spells") {
  for (auto const &[word, kind] : std::vector<std::pair<std::string, StateKind>>{
           { "normal", StateKind::Normal },
           { "choice", StateKind::Choice },
           { "junction", StateKind::Junction },
           { "fork", StateKind::Fork },
           { "join", StateKind::Join },
           { "history", StateKind::History },
           { "deephistory", StateKind::DeepHistory } }) {
    Parsed const r{ parse("chart c { state A " + word + ", }") };
    REQUIRE_MESSAGE(r.ok, word);
    CHECK(state_at(r.pd, stmts_of(r.pd, ElemKind::State)[0]).kind == kind);
  }
}

TEST_CASE("parse: initial and final are not spellable as kinds") {
  // They are reachable only through `*` in an endpoint (PRD 7), so accepting
  // the words here would be a second spelling of a state nobody can write.
  CHECK(first_code(parse("chart c { state A initial, }").diags) ==
        DiagCode::UnknownStateKind);
  CHECK(first_code(parse("chart c { state A final, }").diags) ==
        DiagCode::UnknownStateKind);
  CHECK(first_code(parse("chart c { state A nonsense, }").diags) ==
        DiagCode::UnknownStateKind);
}

TEST_CASE("parse: a state block distinguishes absent from empty") {
  Parsed const bare{ parse("chart c { state A, }") };
  Parsed const empty{ parse("chart c { state A {}, }") };
  REQUIRE(bare.ok);
  REQUIRE(empty.ok);
  CHECK(state_at(bare.pd, stmts_of(bare.pd, ElemKind::State)[0]).has_block == 0);
  CHECK(state_at(empty.pd, stmts_of(empty.pd, ElemKind::State)[0]).has_block == 1);
}

TEST_CASE("parse: submachines, named, labelled and anonymous") {
  Parsed const r{ parse(R"(
chart c {
  state On {
    submachine main { state Idle, },
    submachine strays "consumes stray AT errors" { state Idle, },
    submachine { state Solo, },
    submachine "just a label" { state Other, },
  },
}
)") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const subs{ stmts_of(r.pd, ElemKind::Submachine) };
  REQUIRE(subs.size() == 4);
  CHECK(str(r.pd, submachine_at(r.pd, subs[0]).name) == "main");
  CHECK(str(r.pd, submachine_at(r.pd, subs[0]).label).empty());
  CHECK(str(r.pd, submachine_at(r.pd, subs[1]).name) == "strays");
  CHECK(str(r.pd, submachine_at(r.pd, subs[1]).label) == "consumes stray AT errors");
  CHECK(str(r.pd, submachine_at(r.pd, subs[2]).name).empty());
  CHECK(str(r.pd, submachine_at(r.pd, subs[3]).name).empty());
  CHECK(str(r.pd, submachine_at(r.pd, subs[3]).label) == "just a label");
}

TEST_CASE("parse: a submachine block is mandatory") {
  CHECK(first_code(parse("chart c { submachine main, }").diags) ==
        DiagCode::ExpectedBlock);
  CHECK(first_code(parse("chart c { submachine, }").diags) == DiagCode::ExpectedBlock);
}

TEST_CASE("parse: transitions, kinds, wildcards and labels") {
  Parsed const r{ parse(R"(
chart c {
  trans * -> Off,
  trans Off -> Booting "EG91_POWER_ON",
  trans internal Ready -> Ready "AT_ERR",
  trans local A -> B,
  trans external A -> B,
  trans Done -> *,
}
)") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const ts{ stmts_of(r.pd, ElemKind::Trans) };
  REQUIRE(ts.size() == 6);
  CHECK(trans_at(r.pd, ts[0]).src.wildcard == 1);
  CHECK(path_text(r.pd, trans_at(r.pd, ts[0]).dst) == "Off");
  CHECK(str(r.pd, trans_at(r.pd, ts[1]).label) == "EG91_POWER_ON");
  CHECK(trans_at(r.pd, ts[1]).kind == TransKind::External);  // the default
  CHECK(trans_at(r.pd, ts[2]).kind == TransKind::Internal);
  CHECK(trans_at(r.pd, ts[3]).kind == TransKind::Local);
  CHECK(trans_at(r.pd, ts[4]).kind == TransKind::External);
  CHECK(trans_at(r.pd, ts[5]).dst.wildcard == 1);
  CHECK(trans_at(r.pd, ts[5]).dst.segs.len == 0);
}

TEST_CASE("parse: transition paths, qualified by name and by ordinal") {
  Parsed const r{ parse(R"(
chart c {
  trans On/Ready/Online -> Off,
  trans On:1/Idle -> On:main/Ready,
  trans wifi/On/Connected -> A,
  trans A:0 -> B:99,
}
)") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const ts{ stmts_of(r.pd, ElemKind::Trans) };
  CHECK(path_text(r.pd, trans_at(r.pd, ts[0]).src) == "On/Ready/Online");
  CHECK(path_text(r.pd, trans_at(r.pd, ts[1]).src) == "On:1/Idle");
  CHECK(path_text(r.pd, trans_at(r.pd, ts[1]).dst) == "On:main/Ready");
  CHECK(path_text(r.pd, trans_at(r.pd, ts[2]).src) == "wifi/On/Connected");
  CHECK(path_text(r.pd, trans_at(r.pd, ts[3]).src) == "A:0");
  CHECK(path_text(r.pd, trans_at(r.pd, ts[3]).dst) == "B:99");

  // The ordinal and the name qualifier are distinct slots, not one field.
  PathSeg const &by_ordinal{ r.pd.path_segs[trans_at(r.pd, ts[1]).src.segs.off] };
  CHECK(by_ordinal.ordinal == 1);
  CHECK(by_ordinal.qualifier.len == 0);
  PathSeg const &by_name{ r.pd.path_segs[trans_at(r.pd, ts[1]).dst.segs.off] };
  CHECK(by_name.ordinal == INVALID);
  CHECK(str(r.pd, by_name.qualifier) == "main");
}

TEST_CASE("parse: a submachine ordinal that does not fit is rejected") {
  CHECK(first_code(parse("chart c { trans A:4294967295 -> B, }").diags) ==
        DiagCode::NumberOutOfRange);
  CHECK(parse("chart c { trans A:4294967294 -> B, }").ok);
}

TEST_CASE("parse: a transition takes an optional block") {
  Parsed const r{ parse(R"(
chart c {
  trans A -> B { @doc = "why" },
  trans C -> D,
}
)") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const ts{ stmts_of(r.pd, ElemKind::Trans) };
  CHECK(trans_at(r.pd, ts[0]).has_block == 1);
  CHECK(children_of(r.pd, ts[0]).size() == 1);
  CHECK(trans_at(r.pd, ts[1]).has_block == 0);
}

TEST_CASE("parse: a malformed transition names what was missing") {
  CHECK(first_code(parse("chart c { trans A B, }").diags) == DiagCode::ExpectedArrow);
  CHECK(first_code(parse("chart c { trans -> B, }").diags) == DiagCode::ExpectedEndpoint);
  CHECK(first_code(parse("chart c { trans A ->, }").diags) == DiagCode::ExpectedEndpoint);
  CHECK(first_code(parse("chart c { trans A -> B / , }").diags) ==
        DiagCode::ExpectedIdentifier);
  CHECK(first_code(parse("chart c { trans A: -> B, }").diags) ==
        DiagCode::ExpectedIdentifier);
}

TEST_CASE("parse: includes") {
  Parsed const r{ parse(R"(chart c { include "wifi.scav" as wifi, })") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const incs{ stmts_of(r.pd, ElemKind::Include) };
  REQUIRE(incs.size() == 1);
  CHECK(str(r.pd, include_at(r.pd, incs[0]).path) == "wifi.scav");
  CHECK(str(r.pd, include_at(r.pd, incs[0]).alias) == "wifi");
}

TEST_CASE("parse: a malformed include names what was missing") {
  CHECK(first_code(parse("chart c { include wifi as w, }").diags) ==
        DiagCode::ExpectedString);
  CHECK(first_code(parse(R"(chart c { include "w.scav" wifi, })").diags) ==
        DiagCode::ExpectedToken);
  CHECK(first_code(parse(R"(chart c { include "w.scav" as, })").diags) ==
        DiagCode::ExpectedIdentifier);
}

TEST_CASE("parse: attributes in all three spellings") {
  Parsed const r{ parse(R"(
chart c {
  @flag,
  @doc = "some text",
  @libhsm:handler = "false",
  @libhsm { submachine_handler, legacy = "false" },
  @tags = ["a", "b", "c"],
  @empty_list = [],
}
)") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const attrs{ stmts_of(r.pd, ElemKind::Attr) };
  REQUIRE(attrs.size() == 6);

  AttrStmt const &flag{ attr_at(r.pd, attrs[0]) };
  CHECK(flag.ns.len == 0);
  REQUIRE(flag.entries.len == 1);
  CHECK(str(r.pd, r.pd.attr_entries[flag.entries.off].key) == "flag");
  CHECK(r.pd.attr_entries[flag.entries.off].kind == AttrValueKind::Flag);

  AttrStmt const &doc{ attr_at(r.pd, attrs[1]) };
  CHECK(r.pd.attr_entries[doc.entries.off].kind == AttrValueKind::Scalar);
  CHECK(str(r.pd, r.pd.attr_values[r.pd.attr_entries[doc.entries.off].values.off]) ==
        "some text");

  AttrStmt const &ns{ attr_at(r.pd, attrs[2]) };
  CHECK(str(r.pd, ns.ns) == "libhsm");
  CHECK(str(r.pd, r.pd.attr_entries[ns.entries.off].key) == "handler");

  AttrStmt const &block{ attr_at(r.pd, attrs[3]) };
  CHECK(str(r.pd, block.ns) == "libhsm");
  REQUIRE(block.entries.len == 2);
  CHECK(str(r.pd, r.pd.attr_entries[block.entries.off].key) == "submachine_handler");
  CHECK(r.pd.attr_entries[block.entries.off].kind == AttrValueKind::Flag);
  CHECK(str(r.pd, r.pd.attr_entries[block.entries.off + 1].key) == "legacy");
  CHECK(r.pd.attr_entries[block.entries.off + 1].kind == AttrValueKind::Scalar);

  AttrStmt const &tags{ attr_at(r.pd, attrs[4]) };
  AttrEntry const &tag_entry{ r.pd.attr_entries[tags.entries.off] };
  CHECK(tag_entry.kind == AttrValueKind::List);
  REQUIRE(tag_entry.values.len == 3);
  CHECK(str(r.pd, r.pd.attr_values[tag_entry.values.off]) == "a");
  CHECK(str(r.pd, r.pd.attr_values[tag_entry.values.off + 2]) == "c");

  AttrStmt const &empty{ attr_at(r.pd, attrs[5]) };
  CHECK(r.pd.attr_entries[empty.entries.off].kind == AttrValueKind::List);
  CHECK(r.pd.attr_entries[empty.entries.off].values.len == 0);
}

TEST_CASE("parse: a flag stays distinct from an explicit true") {
  // Which spelling is canonical is the printer's rule (PRD 15). Folding here
  // would decide it early and irreversibly.
  Parsed const r{ parse(R"(chart c { @a, @b = "true", })") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const attrs{ stmts_of(r.pd, ElemKind::Attr) };
  CHECK(r.pd.attr_entries[attr_at(r.pd, attrs[0]).entries.off].kind ==
        AttrValueKind::Flag);
  CHECK(r.pd.attr_entries[attr_at(r.pd, attrs[1]).entries.off].kind ==
        AttrValueKind::Scalar);
}

TEST_CASE("parse: a malformed attribute names what was missing") {
  CHECK(first_code(parse("chart c { @, }").diags) == DiagCode::ExpectedIdentifier);
  CHECK(first_code(parse("chart c { @a = , }").diags) == DiagCode::ExpectedValue);
  CHECK(first_code(parse("chart c { @a = 12, }").diags) == DiagCode::ExpectedValue);
  CHECK(first_code(parse("chart c { @a = [x], }").diags) == DiagCode::ExpectedString);
  CHECK(first_code(parse(R"(chart c { @a = ["x", })").diags) == DiagCode::ExpectedString);
  CHECK(first_code(parse("chart c { @a:, }").diags) == DiagCode::ExpectedIdentifier);
  CHECK(first_code(parse("chart c { @a { b = }, }").diags) == DiagCode::ExpectedValue);
}

TEST_CASE("parse: trailing commas are legal everywhere a list appears") {
  CHECK(parse("chart c { state A, }").ok);
  CHECK(parse("chart c { state A }").ok);
  CHECK(parse(R"(chart c { @t = ["a", "b",], })").ok);
  CHECK(parse(R"(chart c { @t = ["a", "b"], })").ok);
  CHECK(parse("chart c { @n { a, b, }, }").ok);
  CHECK(parse("chart c { @n { a, b }, }").ok);
}

TEST_CASE("parse: a missing separator is its own diagnostic") {
  Parsed const r{ parse("chart c { state A state B }") };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::ExpectedSeparator);
}

TEST_CASE("parse: an unknown statement keyword is rejected") {
  CHECK(first_code(parse("chart c { region R {}, }").diags) == DiagCode::ExpectedItem);
  CHECK(first_code(parse("chart c { 12, }").diags) == DiagCode::ExpectedItem);
  CHECK(first_code(parse("chart c { -> , }").diags) == DiagCode::ExpectedItem);
}

TEST_CASE("parse: s, m and t are aliases only in statement-leading position") {
  Parsed const r{ parse(
      "m main { s Idle, s Ready, t * -> Idle, t internal Ready -> Ready "
      "\"AT_ERR\", }",
      "fragment.scav") };
  CHECK_FALSE(r.ok);  // no chart, so the fragment alone is not a document

  Parsed const whole{
    parse(
        R"(chart c { m main { s Idle, s Ready, t * -> Idle, t internal Ready -> Ready "AT_ERR", }, })")
  };
  REQUIRE(whole.ok);
  CHECK(stmts_of(whole.pd, ElemKind::Submachine).size() == 1);
  CHECK(stmts_of(whole.pd, ElemKind::State).size() == 2);
  CHECK(stmts_of(whole.pd, ElemKind::Trans).size() == 2);
}

TEST_CASE("parse: an alias and the long spelling produce identical trees") {
  Parsed const terse{ parse("chart c { s On { m main { s Idle, t * -> Idle, }, }, }") };
  Parsed const verbose{ parse(
      "chart c { state On { submachine main { state Idle, trans * "
      "-> Idle, }, }, }") };
  REQUIRE(terse.ok);
  REQUIRE(verbose.ok);
  CHECK(shape(terse.pd) == shape(verbose.pd));
}

TEST_CASE("parse: a state may be named after any contextual word") {
  for (char const *name : { "choice",
                            "history",
                            "deephistory",
                            "as",
                            "kind",
                            "s",
                            "m",
                            "t",
                            "normal",
                            "fork",
                            "join",
                            "junction",
                            "initial",
                            "final" }) {
    Parsed const r{ parse(std::string{ "chart c { state " } + name + ", }") };
    REQUIRE_MESSAGE(r.ok, name);
    CHECK(str(r.pd, state_at(r.pd, stmts_of(r.pd, ElemKind::State)[0]).name) == name);
  }
}

TEST_CASE("parse: a reserved word may not be a name") {
  for (char const *word : { "chart",
                            "include",
                            "state",
                            "submachine",
                            "trans",
                            "external",
                            "internal",
                            "local" }) {
    std::string const text{ std::string{ "chart c { state " } + word + ", }" };
    CHECK_MESSAGE(first_code(parse(text).diags) == DiagCode::ReservedWordAsName, word);
  }
  CHECK(first_code(parse("chart trans {}").diags) == DiagCode::ReservedWordAsName);
  CHECK(first_code(parse("chart c { @state = \"x\", }").diags) ==
        DiagCode::ReservedWordAsName);
}

TEST_CASE("parse: whitespace is not significant, so one line parses the same") {
  std::string_view const spread{ R"(
chart c {
  state On {
    submachine main {
      state Idle,
      trans * -> Idle,
    },
  },
}
)" };
  std::string_view const oneline{
    "chart c { state On { submachine main { state Idle, trans * -> Idle, }, }, }"
  };
  Parsed const a{ parse(spread) };
  Parsed const b{ parse(oneline) };
  REQUIRE(a.ok);
  REQUIRE(b.ok);
  CHECK(shape(a.pd) == shape(b.pd));
}

TEST_CASE("parse: the statement tree keeps document order at every level") {
  Parsed const r{ parse(R"(
chart c {
  state First,
  state On {
    @doc = "the composite",
    submachine main {
      state Idle,
      state Ready,
      trans * -> Idle,
    },
    submachine strays {
      state Junk,
    },
  },
  state Last,
}
)") };
  REQUIRE(r.ok);
  CHECK(shape(r.pd) == std::vector<std::string>{ "chart:c",
                                                 "state:First",
                                                 "state:On",
                                                 "attr:doc",
                                                 "submachine:main",
                                                 "state:Idle",
                                                 "state:Ready",
                                                 "trans:*->Idle",
                                                 "submachine:strays",
                                                 "state:Junk",
                                                 "state:Last" });
}

TEST_CASE("parse: a statement span covers the whole construct, block included") {
  Parsed const r{ parse("chart c { state On { state Idle, }, }") };
  REQUIRE(r.ok);
  CHECK(src(r.pd, r.pd.stmts[0].src) == "chart c { state On { state Idle, }, }");

  std::vector<uint32_t> const states{ stmts_of(r.pd, ElemKind::State) };
  CHECK(src(r.pd, r.pd.stmts[states[0]].src) == "state On { state Idle, }");
  CHECK(src(r.pd, r.pd.stmts[states[1]].src) == "state Idle");
}

TEST_CASE("parse: every diagnostic locates to a span inside the document") {
  // P0's exit gate. A diagnostic nobody can point at is not a diagnostic.
  for (std::string_view const text : { "chart c { state A state B }",
                                       "chart c { region R {}, }",
                                       "chart c { trans A B, }",
                                       "chart c { @a = 12, }",
                                       "chart c { submachine main, }",
                                       "chart c { state A initial, }",
                                       "chart c {} extra",
                                       "chart c { state trans, }" }) {
    Parsed const r{ parse(text) };
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.diags.empty());
    for (Diagnostic const &d : r.diags) {
      CHECK(d.doc == DocId{ 0 });
      CHECK(d.src.off <= r.pd.src_bytes.size());
      CHECK(static_cast<size_t>(d.src.off) + d.src.len <= r.pd.src_bytes.size());
      LineCol const at{ diag_line_col(r.pd.src_bytes.data(),
                                      static_cast<uint32_t>(r.pd.src_bytes.size()),
                                      d.src.off) };
      CHECK(at.line >= 1);
      CHECK(at.column >= 1);
    }
  }
}

TEST_CASE("parse: strings are interned into a byte-sorted pool") {
  Parsed const r{ parse(R"(chart zeta { state Mid, state Alpha, state Omega, })") };
  REQUIRE(r.ok);
  // Sorted by bytes, so the pool is a function of the content and not of the
  // order the parser met it in.
  std::string const pool{ reinterpret_cast<char const *>(r.pd.strings.bytes.data()),
                          r.pd.strings.bytes.size() };
  CHECK(pool == "AlphaMidOmegatest.scavzeta");
  CHECK(str(r.pd, r.pd.charts[0].name) == "zeta");
  CHECK(str(r.pd, state_at(r.pd, stmts_of(r.pd, ElemKind::State)[1]).name) == "Alpha");
}

TEST_CASE("parse: two documents differing only in order share a pool") {
  Parsed const a{ parse("chart c { state Alpha, state Beta, }", "d.scav") };
  Parsed const b{ parse("chart c { state Beta, state Alpha, }", "d.scav") };
  REQUIRE(a.ok);
  REQUIRE(b.ok);
  CHECK(a.pd.strings.bytes == b.pd.strings.bytes);
}

TEST_CASE("parse: a repeated name is interned once") {
  Parsed const r{ parse("chart c { state Idle, state Idle, state Idle, }") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const states{ stmts_of(r.pd, ElemKind::State) };
  CHECK(state_at(r.pd, states[0]).name == state_at(r.pd, states[1]).name);
  CHECK(state_at(r.pd, states[1]).name == state_at(r.pd, states[2]).name);
}

TEST_CASE("parse: an identifier is ASCII, so a non-ASCII name is a lexical error") {
  Parsed const r{ parse("chart c { state Caf\xc3\xa9, }") };
  CHECK_FALSE(r.ok);
  CHECK(has_code(r.diags, DiagCode::UnexpectedCharacter));
}

TEST_CASE("parse: src_bytes holds the normalized document, not the raw one") {
  // The name is ASCII because `ident` is; the decomposed sequence goes in the
  // label, which is where the grammar allows one.
  Parsed const r{ parse(
      "\xef\xbb\xbf"
      "chart c {\r\n  state Cafe \"Caf\x65\xcc\x81\",\r\n}\r\n") };
  REQUIRE(r.ok);
  std::string const text{ reinterpret_cast<char const *>(r.pd.src_bytes.data()),
                          r.pd.src_bytes.size() };
  CHECK(text == "chart c {\n  state Cafe \"Caf\xc3\xa9\",\n}\n");
  CHECK(r.pd.doc.text == make_span(0, static_cast<uint32_t>(text.size())));
  CHECK(str(r.pd, state_at(r.pd, stmts_of(r.pd, ElemKind::State)[0]).label) ==
        "Caf\xc3\xa9");
}

TEST_CASE("parse: a normalization failure stops before the lexer") {
  Parsed const r{ parse("chart c { state \xc3 , }") };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::Utf8InvalidByte);
  CHECK(r.pd.stmts.empty());
}

TEST_CASE("parse: comments attach as leading, trailing or own-line") {
  Parsed const r{ parse(R"(
// about the chart
chart c {
  // about Idle
  state Idle, // after Idle

  // floating, attached to nothing in particular

  // about Ready
  state Ready,
}
)") };
  REQUIRE(r.ok);
  std::vector<uint32_t> const states{ stmts_of(r.pd, ElemKind::State) };
  CHECK(comments_of(r.pd, 0) == std::vector<std::string>{ "leading // about the chart" });
  CHECK(comments_of(r.pd, states[0]) ==
        std::vector<std::string>{ "leading // about Idle", "trailing // after Idle" });
  // A blank line *after* is what detaches a comment from the statement below
  // it; a blank line before only detaches it from the statement above.
  CHECK(
      comments_of(r.pd, states[1]) ==
      std::vector<std::string>{ "own-line // floating, attached to nothing in particular",
                                "leading // about Ready" });
}

TEST_CASE("parse: a comment inside an empty block belongs to the block's owner") {
  Parsed const r{ parse(R"(
chart c {
  state On {
    // nothing here yet
  },
}
)") };
  REQUIRE(r.ok);
  uint32_t const on{ stmts_of(r.pd, ElemKind::State)[0] };
  REQUIRE(comments_of(r.pd, on).size() == 1);
  // Whether it sits before the statement or inside its block is derivable from
  // the offsets, so CommentPos does not need a fourth value.
  CHECK(r.pd.comments[r.pd.stmts[on].comments.off].src.off > r.pd.stmts[on].src.off);
}

TEST_CASE("parse: a comment after a nested block trails the statement it follows") {
  Parsed const r{ parse(R"(
chart c {
  state On {
    state Idle,
  }, // after the whole composite
  state Off,
}
)") };
  REQUIRE(r.ok);
  uint32_t const on{ stmts_of(r.pd, ElemKind::State)[0] };
  CHECK(comments_of(r.pd, on) ==
        std::vector<std::string>{ "trailing // after the whole composite" });
}

TEST_CASE("parse: a comment after the chart's closing brace belongs to the chart") {
  Parsed const r{ parse("chart c {\n  state A,\n}\n// the end\n") };
  REQUIRE(r.ok);
  CHECK(comments_of(r.pd, 0) == std::vector<std::string>{ "own-line // the end" });
}

TEST_CASE("parse: every comment is owned by exactly one statement") {
  Parsed const r{ parse(R"(
// one
chart c {
  // two
  state A, // three
  // four
  state B {
    // five
    state C, // six
  }, // seven
  // eight
}
// nine
)") };
  REQUIRE(r.ok);
  uint32_t total{ 0 };
  for (Statement const &s : r.pd.stmts) { total += s.comments.len; }
  CHECK(total == 9);
  CHECK(r.pd.comments.size() == 9);

  // And every span is contiguous and in range, which is what makes it a span.
  for (Statement const &s : r.pd.stmts) {
    CHECK(static_cast<size_t>(s.comments.off) + s.comments.len <= r.pd.comments.size());
  }
}

TEST_CASE("parse: a document with no comments has no trivia array") {
  Parsed const r{ parse("chart c { state A, }") };
  REQUIRE(r.ok);
  CHECK(r.pd.comments.empty());
  for (Statement const &s : r.pd.stmts) { CHECK(s.comments.len == 0); }
}

TEST_CASE("parse: nesting to the depth-16 design target") {
  SynthSpec spec{ synth_default_spec() };
  spec.depth = 16;
  spec.min_roots = 1;
  SynthStats stats{};
  std::string const text{ synth_document(spec, stats) };

  Parsed const r{ parse(text) };
  REQUIRE_MESSAGE(r.ok, diag_message(first_code(r.diags)));
  CHECK(stmts_of(r.pd, ElemKind::State).size() == stats.states);
  CHECK(stmts_of(r.pd, ElemKind::Submachine).size() == stats.submachines);
  CHECK(stmts_of(r.pd, ElemKind::Trans).size() == stats.transitions);
  CHECK(stmts_of(r.pd, ElemKind::Attr).size() == stats.attrs);
  CHECK(r.pd.stmts.size() == stats.statements);
  CHECK(r.pd.comments.size() == stats.comments);
}

TEST_CASE("parse: a hostile depth-10,000 document is rejected, not crashed") {
  // The reason the descent is a heap vector. A call-recursive parser's answer
  // here is a stack overflow, which is a crash and not a diagnostic.
  Parsed const r{ parse(synth_deep_document(10000)) };
  CHECK_FALSE(r.ok);
  CHECK(first_code(r.diags) == DiagCode::DepthLimitExceeded);
}

TEST_CASE("parse: the depth cap is exact") {
  // The chart's own block is one level, so a cap of n admits n - 1 nested
  // states.
  CHECK(parse_deep(synth_deep_document(3), 4).ok);
  CHECK_FALSE(parse_deep(synth_deep_document(4), 4).ok);
  CHECK(first_code(parse_deep(synth_deep_document(4), 4).diags) ==
        DiagCode::DepthLimitExceeded);
  CHECK(parse_deep(synth_deep_document(0), 1).ok);
}

TEST_CASE("parse: a zero cap falls back to the default rather than rejecting everything") {
  CHECK(parse_deep("chart c { state A { state B, }, }", 0).ok);
}

TEST_CASE("parse: the default cap clears the deepest legal chart by a wide margin") {
  // Two block levels per state level plus the chart's own.
  CHECK(DEFAULT_MAX_DEPTH > (2 * 16) + 1);
  CHECK(parse_deep(synth_deep_document(DEFAULT_MAX_DEPTH - 1), DEFAULT_MAX_DEPTH).ok);
}

TEST_CASE("parse: an unclosed block runs out of tokens rather than looping") {
  CHECK_FALSE(parse("chart c { state A {").ok);
  CHECK_FALSE(parse("chart c { state A { state B,").ok);
  CHECK_FALSE(parse("chart c {").ok);
  CHECK_FALSE(parse("chart c { submachine m {").ok);
}

TEST_CASE("parse: a lexical error still reports the syntax error behind it") {
  // One run should not make an author fix stray bytes before it will tell them
  // about the missing comma.
  Parsed const r{ parse("chart c { state A ? state B }") };
  CHECK_FALSE(r.ok);
  CHECK(has_code(r.diags, DiagCode::UnexpectedCharacter));
  CHECK(has_code(r.diags, DiagCode::ExpectedSeparator));
}

TEST_CASE("parse: parse_tokens rejects a token stream with no End sentinel") {
  LexResult const empty;
  ParsedDocument pd;
  std::vector<Diagnostic> diags;
  CHECK_FALSE(parse_tokens(nullptr,
                           0,
                           empty,
                           DocId{ 0 },
                           "x.scav",
                           parse_default_options(),
                           pd,
                           diags));
  CHECK(first_code(diags) == DiagCode::ExpectedChart);
}

TEST_CASE("parse_footprint: counts every array the document holds") {
  Parsed const small{ parse("chart c {}") };
  Parsed const large{ parse(R"(
chart c {
  state A, state B, state C,
  trans A -> B, trans B -> C,
  @doc = "text",
}
)") };
  CHECK(parse_footprint(large.pd) > parse_footprint(small.pd));
  CHECK(parse_footprint(small.pd) >= small.pd.src_bytes.size());
}

TEST_CASE("parse_tree: every enum has a name and every kind word round-trips") {
  for (uint32_t i = 0; i <= static_cast<uint32_t>(ElemKind::Attr); ++i) {
    CHECK(std::string{ syntax_elem_kind_name(static_cast<ElemKind>(i)) } != "unknown");
  }
  for (uint32_t i = 0; i <= static_cast<uint32_t>(StateKind::DeepHistory); ++i) {
    StateKind const kind{ static_cast<StateKind>(i) };
    char const *const name{ syntax_state_kind_name(kind) };
    CHECK(std::string{ name } != "unknown");
    StateKind parsed{ StateKind::Normal };
    // initial and final have names but are not spellable, which is the one
    // asymmetry here and it is deliberate.
    bool const spellable{ (kind != StateKind::Initial) && (kind != StateKind::Final) };
    CHECK(syntax_state_kind_from_name(name, parsed) == spellable);
    if (spellable) { CHECK(parsed == kind); }
  }
  for (uint32_t i = 0; i <= static_cast<uint32_t>(TransKind::Local); ++i) {
    TransKind const kind{ static_cast<TransKind>(i) };
    TransKind parsed{ TransKind::External };
    CHECK(syntax_trans_kind_from_name(syntax_trans_kind_name(kind), parsed));
    CHECK(parsed == kind);
  }
  CHECK(std::string{ syntax_elem_kind_name(static_cast<ElemKind>(99)) } == "unknown");
  CHECK(std::string{ syntax_state_kind_name(static_cast<StateKind>(99)) } == "unknown");
  CHECK(std::string{ syntax_trans_kind_name(static_cast<TransKind>(99)) } == "unknown");

  StateKind ignored_state{ StateKind::Normal };
  TransKind ignored_trans{ TransKind::External };
  CHECK_FALSE(syntax_state_kind_from_name("nope", ignored_state));
  CHECK_FALSE(syntax_trans_kind_from_name("nope", ignored_trans));
}

TEST_CASE(
    "parse_tree: syntax_root_statement is the chart, or INVALID when nothing parsed") {
  Parsed const r{ parse("chart c {}") };
  CHECK(syntax_root_statement(r.pd) == 0);
  ParsedDocument const empty{};
  CHECK(syntax_root_statement(empty) == INVALID);
}
