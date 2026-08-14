// Compiled with the consumer's own flags -- exceptions and RTTI on -- against an
// archive built without either. The answer must be the same.
//
// It also reaches every public header of every library, with nothing on the
// include path but the installed prefix. That is what makes the public/private
// split real rather than a convention: a public header that includes a private
// one, or names a type only a private header declares, fails to compile here and
// nowhere else.

#include <scav/scav_core.h>
#include <scav/scav_types.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

// Parse, lower, validate, resolve: the whole P1 pipeline through the installed
// public header, so every section of the API is proven linkable and none of it
// needs a private header to use.
int check_model() {
  std::string_view const chart_text{ R"(chart consumer "from an installed scav" {
  state Idle,
  state Running { @mode = "fast" },
  trans * -> Idle,
  trans Idle -> Running "go",
})" };

  scav::ParsedDocument doc;
  std::vector<scav::Diagnostic> diags;
  bool const parsed{ scav::parse_document(
      reinterpret_cast<scav_byte const *>(chart_text.data()),
      static_cast<uint32_t>(chart_text.size()),
      "consumer.scav",
      scav::parse_default_options(),
      doc,
      diags) };
  if (!parsed) {
    scav::DiagCode const code{ diags.empty() ? scav::DiagCode::Ok : diags[0].code };
    std::fprintf(stderr, "parse failed: %s\n", scav::diag_message(code));
    return 1;
  }

  scav::Chart chart;
  if (!scav::lower_document(chart, doc, diags)) {
    std::fprintf(stderr, "lowering reported a diagnostic\n");
    return 1;
  }
  if (!scav::validate_chart(chart, diags)) {
    std::fprintf(stderr, "validation reported a diagnostic\n");
    return 1;
  }

  // Two authored states plus the `*`'s initial pseudostate.
  if (chart.states.size() != 3) {
    std::fprintf(stderr, "expected 3 states, got %zu\n", chart.states.size());
    return 1;
  }
  scav::StateId running{ scav::INVALID };
  if (scav::resolve_path(chart, chart.root_submachine, "Running", running) !=
      scav::ResolveStatus::Ok) {
    std::fprintf(stderr, "resolve_path failed to find Running\n");
    return 1;
  }
  uint32_t const attr{ chart_attr_find(
      chart,
      { .kind = scav::ElemKind::State, .ordinal = running.v },
      "mode") };
  if (attr == scav::INVALID ||
      scav::chart_string(chart, chart.attrs[attr].value) != "fast") {
    std::fprintf(stderr, "the attribute did not survive lowering\n");
    return 1;
  }

  std::printf("consumer model ok: %zu states\n", static_cast<size_t>(chart.states.size()));
  return 0;
}

int check_core() {
  std::string_view const chart{ R"(chart consumer "from an installed scav" {
  state Idle,
  state Running,
  trans * -> Idle,
  trans Idle -> Running "go",
})" };

  scav::ParsedDocument doc;
  std::vector<scav::Diagnostic> diags;
  bool const ok{ scav::parse_document(reinterpret_cast<scav_byte const *>(chart.data()),
                                      static_cast<uint32_t>(chart.size()),
                                      "consumer.scav",
                                      scav::parse_default_options(),
                                      doc,
                                      diags) };
  if (!ok) {
    scav::DiagCode const code{ diags.empty() ? scav::DiagCode::Ok : diags[0].code };
    std::fprintf(stderr, "parse failed: %s\n", scav::diag_message(code));
    return 1;
  }

  // Five statements: the chart, two states, two transitions.
  if (doc.stmts.size() != 5) {
    std::fprintf(stderr, "expected 5 statements, got %zu\n", doc.stmts.size());
    return 1;
  }
  if (scav::string_pool_view(doc.strings, doc.charts[0].name) != "consumer") {
    std::fprintf(stderr, "chart name did not survive the round trip\n");
    return 1;
  }
  std::printf("consumer core ok: %zu statements\n", doc.stmts.size());
  return 0;
}

}  // namespace

int main() {
  if (int const rc = check_core(); rc != 0) { return rc; }
  return check_model();
}
