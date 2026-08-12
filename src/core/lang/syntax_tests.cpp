// syntax.cpp: the spellings of the statement enums, and the one convention about
// where the chart statement sits. Compiled against scavcore_testable.

#include "scav/scav_core.h"

#include "core/test_support.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace {
using namespace scav;
using namespace scav::test;
}  // namespace

TEST_CASE("syntax: every enum has a name and every kind word round-trips") {
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
