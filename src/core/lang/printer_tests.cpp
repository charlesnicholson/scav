// The canonical rules one at a time, then the properties holding over all of
// them. Every expectation is spelled in full; a substring asserts nothing.

#include "core/core_internal.h"
#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace {

using namespace scav;
using namespace scav::test;

// Options ===================================================================

TEST_CASE("print: the default column budget is inside its own bounds") {
  PrintOptions const opts{ print_default_options() };
  CHECK(opts.columns == DEFAULT_PRINT_COLUMNS);
  CHECK(print_options_validate(opts));
}

TEST_CASE("print: a budget outside its bounds is rejected, not clamped") {
  CHECK_FALSE(print_options_validate({ .columns = 0 }));
  CHECK_FALSE(print_options_validate({ .columns = PRINT_COLUMNS_MIN - 1 }));
  CHECK_FALSE(print_options_validate({ .columns = PRINT_COLUMNS_MAX + 1 }));
  CHECK(print_options_validate({ .columns = PRINT_COLUMNS_MIN }));
  CHECK(print_options_validate({ .columns = PRINT_COLUMNS_MAX }));

  Parsed const r{ parse("chart c {}") };
  std::string out{ "untouched" };
  CHECK_FALSE(print_document(r.pd, { .columns = 0 }, out));
  CHECK(out == "untouched");
}

TEST_CASE("print: a document that produced no statements prints nothing") {
  ParsedDocument const empty{};
  std::string out;
  CHECK(print_document(empty, print_default_options(), out));
  CHECK(out.empty());
}

// Rule 1 -- keyword spelling ================================================

TEST_CASE("print: s, m and t normalize to their long spellings") {
  CHECK(print("chart c { s Idle, s Ready, m x { s In, t * -> In, }, t Idle -> Ready, }") ==
        "chart c {\n"
        "  state Idle,\n"
        "  state Ready,\n"
        "  submachine x { state In, trans * -> In },\n"
        "  trans Idle -> Ready,\n"
        "}\n");
}

TEST_CASE("print: a state named after a one-letter alias stays a name") {
  CHECK(print("chart c { state s, state m, state t, }") ==
        "chart c {\n"
        "  state s,\n"
        "  state m,\n"
        "  state t,\n"
        "}\n");
}

// Rule 2 -- repeated key becomes a list =====================================

TEST_CASE("print: a key written twice becomes one list in insertion order") {
  CHECK(print(R"(chart c { @k = "b", @k = "a", })") ==
        "chart c {\n"
        "  @k = [\"b\", \"a\"],\n"
        "}\n");
}

TEST_CASE("print: a one-element list collapses to a scalar") {
  CHECK(print(R"(chart c { @k = ["only"], })") ==
        "chart c {\n"
        "  @k = \"only\",\n"
        "}\n");
}

TEST_CASE("print: an empty list survives, since no scalar spells it") {
  CHECK(print("chart c { @k = [], }") ==
        "chart c {\n"
        "  @k = [],\n"
        "}\n");
}

TEST_CASE("print: a list and a scalar under one key concatenate") {
  CHECK(print(R"(chart c { @k = ["a", "b"], @k = "c", })") ==
        "chart c {\n"
        "  @k = [\"a\", \"b\", \"c\"],\n"
        "}\n");
}

// Rule 3 -- flag form =======================================================

TEST_CASE("print: an explicit true value becomes a flag, and a flag stays one") {
  CHECK(print(R"(chart c { @a = "true", @b, })") ==
        "chart c {\n"
        "  @a,\n"
        "  @b,\n"
        "}\n");
}

TEST_CASE("print: only the exact text true is a flag") {
  CHECK(print(R"(chart c { @a = "True", @b = "true ", @d = "false", })") ==
        "chart c {\n"
        "  @a = \"True\",\n"
        "  @b = \"true \",\n"
        "  @d = \"false\",\n"
        "}\n");
}

TEST_CASE("print: two flags under one key are a list, not a flag") {
  CHECK(print("chart c { @k, @k, }") ==
        "chart c {\n"
        "  @k = [\"true\", \"true\"],\n"
        "}\n");
}

// Rule 5 -- namespace block form ============================================

TEST_CASE("print: two keys sharing a namespace take the block spelling") {
  CHECK(print(R"(chart c { @ns:b = "2", @ns:a = "1", })") ==
        "chart c {\n"
        "  @ns { a = \"1\", b = \"2\" },\n"
        "}\n");
}

TEST_CASE("print: a sole key in a namespace un-blocks") {
  CHECK(print(R"(chart c { @ns { only = "1" }, })") ==
        "chart c {\n"
        "  @ns:only = \"1\",\n"
        "}\n");
}

TEST_CASE("print: a block and a colon key under one namespace merge into one block") {
  CHECK(print(R"(chart c { @ns { a }, @ns:b = "2", })") ==
        "chart c {\n"
        "  @ns { a, b = \"2\" },\n"
        "}\n");
}

TEST_CASE("print: a bare key equal to a namespace does not join its block") {
  CHECK(print(R"(chart c { @ns = "bare", @ns:a = "1", @ns:b = "2", })") ==
        "chart c {\n"
        "  @ns = \"bare\",\n"
        "  @ns { a = \"1\", b = \"2\" },\n"
        "}\n");
}

TEST_CASE("print: a namespace block with no keys at all disappears") {
  CHECK(print("chart c { @ns {}, state A, }") ==
        "chart c {\n"
        "  state A,\n"
        "}\n");
}

// Rule 6 -- trailing comma ==================================================

TEST_CASE("print: a broken block takes a trailing comma and a flat one does not") {
  CHECK(print("chart c { state A, }") ==
        "chart c {\n"
        "  state A,\n"
        "}\n");
  CHECK(print("chart c { state A { state B, state C, }, }") ==
        "chart c {\n"
        "  state A { state B, state C },\n"
        "}\n");
}

// Rule 7 -- attribute order =================================================

TEST_CASE("print: attributes sort by key bytes, not by the order they were written") {
  CHECK(print("chart c { @zeta, @alpha, @Mid, }") ==
        "chart c {\n"
        "  @Mid,\n"
        "  @alpha,\n"
        "  @zeta,\n"
        "}\n");
}

TEST_CASE("print: attributes rise above structure while structure keeps its order") {
  CHECK(print("chart c { state Z, @b, state A, @a, state M, }") ==
        "chart c {\n"
        "  @a,\n"
        "  @b,\n"
        "  state Z,\n"
        "  state A,\n"
        "  state M,\n"
        "}\n");
}

TEST_CASE("print: sorting is by bytes, so a namespace sorts after its bare key") {
  // "ns" < "ns:a" byte-wise, and ':' is 0x3A, below every letter.
  CHECK(print("chart c { @ns:a, @nsx, @ns, }") ==
        "chart c {\n"
        "  @ns,\n"
        "  @ns:a,\n"
        "  @nsx,\n"
        "}\n");
}

TEST_CASE("print: an attribute belongs to the block it was written in") {
  CHECK(print("chart c { @top, state A { @inner, }, }") ==
        "chart c {\n"
        "  @top,\n"
        "  state A { @inner },\n"
        "}\n");
  CHECK(print("chart c { @top, state A { @inner, }, }", 20) ==
        "chart c {\n"
        "  @top,\n"
        "  state A {\n"
        "    @inner,\n"
        "  },\n"
        "}\n");
}

// Line breaking =============================================================

TEST_CASE("print: a block within the budget stays on one line") {
  CHECK(print("chart c { state A { state B, }, }") ==
        "chart c {\n"
        "  state A { state B },\n"
        "}\n");
}

TEST_CASE("print: a block over the budget breaks, and only the block that overflows") {
  std::string const out{ print(
      "chart c { state A { state B, state C, }, state D { state E, }, }", 30) };
  CHECK(out ==
        "chart c {\n"
        "  state A {\n"
        "    state B,\n"
        "    state C,\n"
        "  },\n"
        "  state D { state E },\n"
        "}\n");
}

TEST_CASE("print: the budget counts codepoints, not UTF-8 bytes") {
  // Each accented character is two bytes and one column, so the byte count would
  // break this line and the codepoint count does not.
  std::string const text{ "chart c { state A \"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\", }" };
  CHECK(print(text, 30) ==
        "chart c {\n"
        "  state A \"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\",\n"
        "}\n");
  CHECK(print(text, 30) == print(print(text, 30), 30));
}

TEST_CASE("print: the chart block always breaks, however small the document") {
  CHECK(print("chart c { state A, }", PRINT_COLUMNS_MAX) ==
        "chart c {\n"
        "  state A,\n"
        "}\n");
  CHECK(print("chart c {}") == "chart c {\n}\n");
}

TEST_CASE("print: a long list breaks one value per line") {
  CHECK(print(R"(chart c { @k = ["aaaaaaaa", "bbbbbbbb", "cccccccc"], })", 24) ==
        "chart c {\n"
        "  @k = [\n"
        "    \"aaaaaaaa\",\n"
        "    \"bbbbbbbb\",\n"
        "    \"cccccccc\",\n"
        "  ],\n"
        "}\n");
}

TEST_CASE("print: a long namespace block breaks one entry per line") {
  CHECK(print(R"(chart c { @ns { aaaaaaaa = "1", bbbbbbbb = "2" }, })", 24) ==
        "chart c {\n"
        "  @ns {\n"
        "    aaaaaaaa = \"1\",\n"
        "    bbbbbbbb = \"2\",\n"
        "  },\n"
        "}\n");
}

TEST_CASE("print: the budget counts the @ and the namespace above the entry") {
  // The entry alone fits at 24; `@nsx:` in front of it does not, and measuring
  // the entry on its own emitted a 29-column line.
  CHECK(print(R"(chart c { @nsx:k = ["aaaaaa", "bbb"], })", 24) ==
        "chart c {\n"
        "  @nsx:k = [\n"
        "    \"aaaaaa\",\n"
        "    \"bbb\",\n"
        "  ],\n"
        "}\n");
  // A bare key carries only the `@`, and one wide enough still breaks.
  CHECK(print(R"(chart c { @k = ["aaaaaaaaaaaaaaaa", "bbb"], })", 24) ==
        "chart c {\n"
        "  @k = [\n"
        "    \"aaaaaaaaaaaaaaaa\",\n"
        "    \"bbb\",\n"
        "  ],\n"
        "}\n");
  // And the prefix does not push a list over when it still fits.
  CHECK(print(R"(chart c { @n:k = ["a", "b"], })", 24) ==
        "chart c {\n"
        "  @n:k = [\"a\", \"b\"],\n"
        "}\n");
}

TEST_CASE("print: no attribute line runs past the budget it can break under") {
  // The property behind the case above, over every prefix width and budget: a
  // line holding a breakable list is never wider than the budget.
  for (std::string const &ns : { std::string{}, std::string{ "n:" },
                                 std::string{ "averylongnamespace:" } }) {
    for (uint32_t const columns : { PRINT_COLUMNS_MIN, 24U, 32U, 48U, 90U }) {
      std::string const src{ "chart c { @" + ns +
                             R"(k = ["aaaaaaaa", "bbbbbbbb", "cccccccc"], })" };
      std::string const out{ print(src, columns) };
      CAPTURE(ns);
      CAPTURE(columns);
      CHECK(is_canonical(out, columns));
      size_t begin{ 0 };
      while (begin < out.size()) {
        size_t const end{ out.find('\n', begin) };
        std::string_view const line{ out.data() + begin,
                                     (end == std::string::npos ? out.size() : end) -
                                         begin };
        // A line still holding two values had room to break and did not.
        if (line.size() > columns) { CHECK(line.find("\", \"") == std::string_view::npos); }
        if (end == std::string::npos) { break; }
        begin = end + 1;
      }
    }
  }
}

TEST_CASE("print: an atom wider than the budget overflows rather than corrupting") {
  std::string const out{ print("chart c { state Aaaaaaaaaaaaaaaaaaaaaaaaaaaaa, }", 20) };
  CHECK(out ==
        "chart c {\n"
        "  state Aaaaaaaaaaaaaaaaaaaaaaaaaaaaa,\n"
        "}\n");
  CHECK(is_canonical(out, 20));
}

// Strings ===================================================================

TEST_CASE("print: a raw string comes back escaped, because one spelling is canonical") {
  CHECK(print(R"(chart c { state A """a"b\c""", })") ==
        "chart c {\n"
        "  state A \"a\\\"b\\\\c\",\n"
        "}\n");
}

TEST_CASE("print: newline, tab, quote and backslash take their short escapes") {
  CHECK(print("chart c { @k = \"a\\nb\\tc\\\"d\\\\e\", }") ==
        "chart c {\n"
        "  @k = \"a\\nb\\tc\\\"d\\\\e\",\n"
        "}\n");
}

TEST_CASE("print: a control character takes a unicode escape and round-trips") {
  std::string const out{ print(R"(chart c { @k = "a\u0001b", })") };
  CHECK(out ==
        "chart c {\n"
        "  @k = \"a\\u0001b\",\n"
        "}\n");
  CHECK(is_canonical(out));
}

TEST_CASE("print: a non-ASCII label passes through as its own bytes") {
  CHECK(print("chart c { state A \"caf\xC3\xA9 \xE2\x86\x92\", }") ==
        "chart c {\n"
        "  state A \"caf\xC3\xA9 \xE2\x86\x92\",\n"
        "}\n");
}

TEST_CASE("print: an escaped codepoint decodes and reprints as its own bytes") {
  CHECK(print(R"(chart c { state A "\u00e9", })") ==
        "chart c {\n"
        "  state A \"\xC3\xA9\",\n"
        "}\n");
}

// Endpoints and kinds =======================================================

TEST_CASE("print: every endpoint spelling survives verbatim") {
  CHECK(print("chart c {\n"
              "  trans * -> A,\n"
              "  trans A -> *,\n"
              "  trans On:main/Idle -> On:1/Ready,\n"
              "  trans dock/On/Seated -> A,\n"
              "}") ==
        "chart c {\n"
        "  trans * -> A,\n"
        "  trans A -> *,\n"
        "  trans On:main/Idle -> On:1/Ready,\n"
        "  trans dock/On/Seated -> A,\n"
        "}\n");
}

TEST_CASE("print: a transition kind is written only when it is not external") {
  CHECK(print("chart c {\n"
              "  trans external A -> B,\n"
              "  trans internal A -> B,\n"
              "  trans local A -> B,\n"
              "}") ==
        "chart c {\n"
        "  trans A -> B,\n"
        "  trans internal A -> B,\n"
        "  trans local A -> B,\n"
        "}\n");
}

TEST_CASE("print: a state kind is written only when it is not normal") {
  CHECK(print("chart c {\n"
              "  state A normal,\n"
              "  state B choice,\n"
              "  state C junction,\n"
              "  state D fork,\n"
              "  state E join,\n"
              "  state F history,\n"
              "  state G deephistory,\n"
              "}") ==
        "chart c {\n"
        "  state A,\n"
        "  state B choice,\n"
        "  state C junction,\n"
        "  state D fork,\n"
        "  state E join,\n"
        "  state F history,\n"
        "  state G deephistory,\n"
        "}\n");
}

TEST_CASE("print: a kind and a label appear in that order") {
  CHECK(print(R"(chart c { state A choice "pick one", })") ==
        "chart c {\n"
        "  state A choice \"pick one\",\n"
        "}\n");
}

TEST_CASE("print: a transition carries a label and a block") {
  CHECK(print(R"(chart c { trans A -> B "EV" { @guard = "x", }, })") ==
        "chart c {\n"
        "  trans A -> B \"EV\" { @guard = \"x\" },\n"
        "}\n");
}

TEST_CASE("print: an include keeps its authored path and alias") {
  CHECK(print(R"(chart c { include "./dock.scav" as dock, })") ==
        "chart c {\n"
        "  include \"./dock.scav\" as dock,\n"
        "}\n");
}

// Empty blocks ==============================================================

TEST_CASE("print: an empty state block disappears, since two spellings are one") {
  CHECK(print("chart c { state A {}, state B, }") ==
        "chart c {\n"
        "  state A,\n"
        "  state B,\n"
        "}\n");
}

TEST_CASE("print: an empty transition block disappears") {
  CHECK(print("chart c { trans A -> B {}, }") ==
        "chart c {\n"
        "  trans A -> B,\n"
        "}\n");
}

TEST_CASE("print: an empty submachine keeps its braces, the grammar requiring them") {
  CHECK(print("chart c { state A { submachine m {}, submachine n {}, }, }") ==
        "chart c {\n"
        "  state A { submachine m {}, submachine n {} },\n"
        "}\n");
}

// The implicit submachine ===================================================

TEST_CASE("print: a sole unnamed submachine is the implicit one written out") {
  CHECK(print("chart c { state A { submachine { state B, trans * -> B, }, }, }") ==
        "chart c {\n"
        "  state A { state B, trans * -> B },\n"
        "}\n");
}

TEST_CASE("print: a named or labelled sole submachine stays explicit") {
  CHECK(print("chart c { state A { submachine m { state B, }, }, }") ==
        "chart c {\n"
        "  state A { submachine m { state B } },\n"
        "}\n");
  CHECK(print(R"(chart c { state A { submachine "why" { state B, }, }, })") ==
        "chart c {\n"
        "  state A { submachine \"why\" { state B } },\n"
        "}\n");
}

TEST_CASE("print: an unnamed submachine beside a second one stays explicit") {
  CHECK(print("chart c { state A { submachine { state B, }, submachine n { state C, },"
              " }, }") ==
        "chart c {\n"
        "  state A { submachine { state B }, submachine n { state C } },\n"
        "}\n");
}

TEST_CASE("print: a submachine carrying its own attributes is never elided") {
  // Hoisting would move the attribute onto the owner state, which is a different
  // row of the model, not a different spelling of the same one.
  CHECK(print("chart c { state A { submachine { @mine, state B, }, }, }") ==
        "chart c {\n"
        "  state A { submachine { @mine, state B } },\n"
        "}\n");
}

TEST_CASE("print: eliding a submachine leaves its comments in the owner's block") {
  CHECK(print("chart c {\n"
              "  state A {\n"
              "    // about the region\n"
              "    submachine { state B, },\n"
              "  },\n"
              "}") ==
        "chart c {\n"
        "  state A {\n"
        "    // about the region\n"
        "    state B,\n"
        "  },\n"
        "}\n");
}

// Comments ==================================================================

TEST_CASE("print: a leading comment stays above its statement") {
  CHECK(print("chart c {\n"
              "  // about A\n"
              "  state A,\n"
              "}") ==
        "chart c {\n"
        "  // about A\n"
        "  state A,\n"
        "}\n");
}

TEST_CASE("print: a trailing comment stays on its statement's line") {
  CHECK(print("chart c {\n"
              "  state A, // about A\n"
              "  state B,\n"
              "}") ==
        "chart c {\n"
        "  state A, // about A\n"
        "  state B,\n"
        "}\n");
}

TEST_CASE("print: an own-line comment keeps the blank line that made it one") {
  CHECK(print("chart c {\n"
              "  // a heading\n"
              "\n"
              "  state A,\n"
              "}") ==
        "chart c {\n"
        "  // a heading\n"
        "\n"
        "  state A,\n"
        "}\n");
}

TEST_CASE("print: a comment before the chart keyword stays there") {
  CHECK(print("// file header\nchart c { state A, }") ==
        "// file header\n"
        "chart c {\n"
        "  state A,\n"
        "}\n");
}

TEST_CASE("print: a comment after the closing brace stays after it") {
  CHECK(print("chart c { state A, }\n// afterword\n") ==
        "chart c {\n"
        "  state A,\n"
        "}\n"
        "// afterword\n");
}

TEST_CASE("print: a comment on the opening brace's line stays there") {
  CHECK(print("chart c { // opens here\n  state A,\n}") ==
        "chart c { // opens here\n"
        "  state A,\n"
        "}\n");
}

TEST_CASE("print: a comment dangling at the end of a block stays inside it") {
  CHECK(print("chart c {\n  state A,\n  // nothing follows\n}") ==
        "chart c {\n"
        "  state A,\n"
        "  // nothing follows\n"
        "}\n");
}

TEST_CASE("print: a comment inside an otherwise empty block keeps the block") {
  CHECK(print("chart c { state A { // only a note\n }, }") ==
        "chart c {\n"
        "  state A { // only a note\n"
        "  },\n"
        "}\n");
}

TEST_CASE("print: a comment anywhere in a subtree keeps that subtree broken") {
  CHECK(print("chart c { state A { state B, // note\n state C, }, }") ==
        "chart c {\n"
        "  state A {\n"
        "    state B, // note\n"
        "    state C,\n"
        "  },\n"
        "}\n");
}

TEST_CASE("print: a comment on an attribute travels with it when it sorts") {
  CHECK(print("chart c {\n"
              "  // about zeta\n"
              "  @zeta, // trailing zeta\n"
              "  @alpha,\n"
              "}") ==
        "chart c {\n"
        "  @alpha,\n"
        "  // about zeta\n"
        "  @zeta, // trailing zeta\n"
        "}\n");
}

TEST_CASE("print: merging two statements under one key keeps both their comments") {
  std::string const out{ print("chart c {\n"
                               "  // first\n"
                               "  @k = \"a\", // one\n"
                               "  // second\n"
                               "  @k = \"b\",\n"
                               "}") };
  CHECK(out ==
        "chart c {\n"
        "  // first\n"
        "  // one\n"
        "  // second\n"
        "  @k = [\"a\", \"b\"],\n"
        "}\n");
  CHECK(is_canonical(out));
}

TEST_CASE("print: the comments of a dropped attribute rise to the top of the block") {
  CHECK(print("chart c {\n"
              "  state A,\n"
              "  // orphaned\n"
              "  @ns {},\n"
              "}") ==
        "chart c {\n"
        "  // orphaned\n"
        "  state A,\n"
        "}\n");
}

TEST_CASE("print: a run of comments keeps its order and its blank lines") {
  CHECK(print("chart c {\n"
              "  // one\n"
              "  // two\n"
              "\n"
              "  // three\n"
              "  state A,\n"
              "}") ==
        "chart c {\n"
        "  // one\n"
        "  // two\n"
        "\n"
        "  // three\n"
        "  state A,\n"
        "}\n");
}

TEST_CASE("print: trailing blanks inside a comment do not reach the output") {
  CHECK(print("chart c {\n  // padded   \n  state A,\n}") ==
        "chart c {\n"
        "  // padded\n"
        "  state A,\n"
        "}\n");
}

// Blank lines ===============================================================

TEST_CASE("print: a blank line between two statements survives") {
  CHECK(print("chart c {\n  state A,\n\n  state B,\n}") ==
        "chart c {\n"
        "  state A,\n"
        "\n"
        "  state B,\n"
        "}\n");
}

TEST_CASE("print: a run of blank lines collapses to one") {
  CHECK(print("chart c {\n  state A,\n\n\n\n  state B,\n}") ==
        "chart c {\n"
        "  state A,\n"
        "\n"
        "  state B,\n"
        "}\n");
}

TEST_CASE("print: a blank opening or closing a block is dropped") {
  CHECK(print("chart c {\n\n  state A,\n\n}") ==
        "chart c {\n"
        "  state A,\n"
        "}\n");
  CHECK(print("chart c {\n  state A {\n\n    state B,\n  },\n}") ==
        "chart c {\n"
        "  state A { state B },\n"
        "}\n");
}

TEST_CASE("print: a blank stays above the comments it was written above") {
  CHECK(print("chart c {\n  state A,\n\n  // about B\n  state B,\n}") ==
        "chart c {\n"
        "  state A,\n"
        "\n"
        "  // about B\n"
        "  state B,\n"
        "}\n");
}

TEST_CASE("print: a blank between a comment and its statement is the own-line rule") {
  // Two ways to write a gap, and each keeps its own shape: the blank above the
  // comment is the statement's, the blank below it is the comment's.
  CHECK(print("chart c {\n  state A,\n  // a heading\n\n  state B,\n}") ==
        "chart c {\n"
        "  state A,\n"
        "  // a heading\n"
        "\n"
        "  state B,\n"
        "}\n");
}

TEST_CASE("print: a trailing comment above does not swallow the blank below it") {
  CHECK(print("chart c {\n"
              "  state A, // trailing\n"
              "\n"
              "  state B,\n"
              "}") ==
        "chart c {\n"
        "  state A, // trailing\n"
        "\n"
        "  state B,\n"
        "}\n");
  // And with a heading between them, where the gap is measured from the end of
  // the trailing comment to the start of the heading.
  CHECK(print("chart c {\n"
              "  state A, // trailing\n"
              "\n"
              "  // a heading\n"
              "  state B,\n"
              "}") ==
        "chart c {\n"
        "  state A, // trailing\n"
        "\n"
        "  // a heading\n"
        "  state B,\n"
        "}\n");
}

TEST_CASE("print: a blank forces its block to break") {
  CHECK(print("chart c {\n  state A { state B,\n\n state C, },\n}") ==
        "chart c {\n"
        "  state A {\n"
        "    state B,\n"
        "\n"
        "    state C,\n"
        "  },\n"
        "}\n");
}

TEST_CASE("print: a blank above an attribute travels with it when it sorts") {
  CHECK(print("chart c {\n  @m,\n  @z,\n\n  @a,\n}") ==
        "chart c {\n"
        "  @a,\n"
        "  @m,\n"
        "  @z,\n"
        "}\n");
  CHECK(print("chart c {\n  @a,\n\n  @z,\n  state S,\n}") ==
        "chart c {\n"
        "  @a,\n"
        "\n"
        "  @z,\n"
        "  state S,\n"
        "}\n");
}

TEST_CASE("print: a blank between the attributes and the structure survives") {
  CHECK(print("chart c {\n  @a,\n\n  state S,\n}") ==
        "chart c {\n"
        "  @a,\n"
        "\n"
        "  state S,\n"
        "}\n");
}

TEST_CASE("print: every blank spelling is its own fixed point") {
  std::string const text{
    "// header\n"
    "\n"
    "chart c {\n"
    "  @z,\n"
    "\n"
    "  @a,\n"
    "  state Off,\n"
    "\n"
    "  // a heading\n"
    "\n"
    "  state On {\n"
    "    state Inner,\n"
    "\n"
    "    trans * -> Inner,\n"
    "  },\n"
    "}\n"
  };
  std::string const once{ print(text) };
  CHECK(once == print(once));
  CHECK(parse(once).ok);
  CHECK(once.find("\n\n") != std::string::npos);
}

// Structure ordering ========================================================

TEST_CASE("print: structure keeps document order, which is a layout hint") {
  CHECK(print(R"(chart c { state Z, trans Z -> A, state A, include "x" as ex,)"
              " state M, }") ==
        "chart c {\n"
        "  state Z,\n"
        "  trans Z -> A,\n"
        "  state A,\n"
        "  include \"x\" as ex,\n"
        "  state M,\n"
        "}\n");
}

TEST_CASE("print: nesting survives to sixteen levels") {
  std::string text{ "chart c {" };
  for (uint32_t i = 0; i < 16; ++i) { text += "state S" + std::to_string(i) + " {"; }
  text += "state Leaf,";
  for (uint32_t i = 0; i < 16; ++i) { text += "},"; }
  text += "}";

  // A budget too small for any nesting forces every level onto its own line, so
  // the leaf sits at the chart's depth plus sixteen: thirty-four spaces.
  std::string const out{ print(text, PRINT_COLUMNS_MIN) };
  CHECK(out.find("\n" + std::string(34, ' ') + "state Leaf,\n") != std::string::npos);
  CHECK(is_canonical(out, PRINT_COLUMNS_MIN));

  Parsed const again{ parse(out) };
  CHECK(again.ok);
  CHECK(again.diags.empty());
  CHECK(stmts_of(again.pd, StmtKind::State).size() == 17);
}

// Idempotence ===============================================================

TEST_CASE("print: the canonicity helper means canonical, not merely convergent") {
  // Every input converges by the second pass, so a helper that compared two
  // prints would accept anything and every case using it would assert nothing.
  CHECK_FALSE(is_canonical("chart c { s A, }"));
  CHECK_FALSE(is_canonical("chart c {\n  state A\n}\n"));
  CHECK(is_canonical("chart c {\n  state A,\n}\n"));
}


TEST_CASE("print: canonical output parses and prints as itself") {
  // One document exercising every rule at once, so the fixed point is asserted
  // over their interaction and not only one at a time.
  std::string const messy{
    "// header\n"
    "chart vac \"robot vacuum\" { // opens\n"
    "  @z = \"last\", @ns:b = \"2\", @ns:a = \"1\", @flag = \"true\",\n"
    "  include \"\"\"dock.scav\"\"\" as dock,\n"
    "  s Off \"powered down\", s PreConfig choice,\n"
    "  t * -> Off, t Off -> PreConfig \"POWER_ON\",\n"
    "  s On { m { s Idle, t * -> Idle, }, },\n"
    "  s Two { m main { s Idle, }, m aux \"sweeps\" { s Idle, }, },\n"
    "  // dangling\n"
    "}\n"
    "// afterword\n"
  };
  std::string const once{ print(messy) };
  CHECK(once == print(once));

  Parsed const again{ parse(once) };
  CHECK(again.ok);
  CHECK(again.diags.empty());
  CHECK(print(once) == once);
}

TEST_CASE("print: idempotence holds at every budget the bounds admit") {
  std::string const text{
    "chart c {\n"
    "  @ns { alpha = \"one\", beta = [\"two\", \"three\"] },\n"
    "  state Outer { state Inner { state Leaf, trans * -> Leaf, }, },\n"
    "  trans Outer/Inner/Leaf -> Outer \"a fairly long event name\",\n"
    "}\n"
  };
  for (uint32_t columns : { PRINT_COLUMNS_MIN, 24U, 40U, 90U, 200U }) {
    CAPTURE(columns);
    std::string const once{ print(text, columns) };
    CHECK(once == print(once, columns));
    CHECK(parse(once).ok);
  }
}

TEST_CASE("print: a document the parser rejected prints its rows without reading past") {
  // A block commits its children on close, so a failed parse leaves `state A` as
  // a row nothing points at and the tree walk never reaches it.
  Parsed const r{ parse("chart c { state A, state !, }") };
  CHECK_FALSE(r.ok);
  CHECK(r.pd.stmts.size() > 1);
  std::string out;
  CHECK(print_document(r.pd, print_default_options(), out));
  CHECK(out == "chart c {\n}\n");
}

TEST_CASE("print: payload rows out of step with the statements print nothing") {
  Parsed r{ parse("chart c { state A, }") };
  r.pd.stmt_payload.clear();
  std::string out;
  CHECK(print_document(r.pd, print_default_options(), out));
  CHECK(out.empty());
}

}  // namespace
