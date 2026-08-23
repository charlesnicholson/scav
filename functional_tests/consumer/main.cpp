// Compiled with exceptions and RTTI on against archives built without either,
// reaching every public header with only the install prefix on the path.

#include <scav/scav_core.h>
#include <scav/scav_core_c.h>
#include <scav/scav_layout.h>
#include <scav/scav_layout_c.h>
#include <scav/scav_types.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

// The C API through the installed header, compiled outside our build.
int check_abi() {
  if (scav_abi_version() == 0) {
    std::fprintf(stderr, "C API version is zero\n");
    return 1;
  }

  std::string_view const root{
    R"(chart net { include "leaf.scav" as leaf, state A, trans A -> leaf/L, })"
  };
  std::string_view const leaf{ R"(chart leaf { state L, })" };

  scav_load *loader{ nullptr };
  if (scav_load_begin(&loader) != SCAV_OK) {
    std::fprintf(stderr, "scav_load_begin failed\n");
    return 1;
  }

  auto const add = [&](std::string_view text, char const *name) {
    return scav_load_add(loader,
                         reinterpret_cast<scav_byte const *>(text.data()),
                         static_cast<uint32_t>(text.size()),
                         name);
  };

  int rc{ 0 };
  if (add(root, "net.scav") != SCAV_OK) {
    std::fprintf(stderr, "adding the root failed\n");
    rc = 1;
  }

  scav_pending const *pending{ nullptr };
  uint32_t count{ 0 };
  if ((rc == 0) && (scav_load_pending(loader, &pending, &count) != SCAV_OK)) {
    std::fprintf(stderr, "scav_load_pending failed\n");
    rc = 1;
  }
  if ((rc == 0) && (count != 1)) {
    std::fprintf(stderr, "expected 1 pending document, got %u\n", count);
    rc = 1;
  }
  if ((rc == 0) && (add(leaf, "leaf.scav") != SCAV_OK)) {
    std::fprintf(stderr, "adding the leaf failed\n");
    rc = 1;
  }

  scav_chart *chart{ nullptr };
  if ((rc == 0) && (scav_load_finish(loader, &chart) != SCAV_OK)) {
    std::fprintf(stderr, "scav_load_finish failed\n");
    rc = 1;
  }
  if ((rc == 0) && (chart != nullptr)) {
    uint32_t documents{ 0 };
    uint32_t states{ 0 };
    uint32_t submachines{ 0 };
    uint32_t transitions{ 0 };
    uint32_t includes{ 0 };
    if (scav_chart_counts(chart,
                          &documents,
                          &states,
                          &submachines,
                          &transitions,
                          &includes) != SCAV_OK) {
      std::fprintf(stderr, "scav_chart_counts failed\n");
      rc = 1;
    } else if ((documents != 2) || (includes != 1)) {
      std::fprintf(stderr, "expected 2 documents and 1 include\n");
      rc = 1;
    } else {
      std::printf("consumer c api ok: %u documents, %u states\n", documents, states);
    }
  }

  // Layout through the installed C surface: run, then read a geometry column.
  if ((rc == 0) && (chart != nullptr)) {
    scav_layout_opts opts{};
    uint32_t placed{ 0 };
    scav_column_id column{ 0 };
    if ((scav_profile_named("readable", &opts.profile) != SCAV_OK) ||
        (scav_layout_run(chart, nullptr, &opts, nullptr, 0, &placed) != SCAV_OK) ||
        (scav_column_find(chart, "scav.geom.state", &column) != SCAV_OK)) {
      std::fprintf(stderr, "layout through the C surface failed\n");
      rc = 1;
    } else {
      std::printf("consumer layout run ok\n");
    }
  }

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
  return rc;
}

// Load, validate and resolve through the installed public header, so every
// section of the API is proven linkable without reaching a private one.
int check_model() {
  std::string_view const chart_text{ R"(chart consumer "from an installed scav" {
  state Idle,
  state Running { @mode = "fast" },
  trans * -> Idle,
  trans Idle -> Running "go",
})" };

  scav::Loader loader;
  std::vector<scav::Diagnostic> diags;
  if (!scav::load_add(loader,
                      reinterpret_cast<scav_byte const *>(chart_text.data()),
                      chart_text.size(),
                      "consumer.scav")) {
    scav::DiagCode const code{ diags.empty() ? scav::DiagCode::Ok : diags[0].code };
    std::fprintf(stderr, "load_add failed: %s\n", scav::diag_message(code));
    return 1;
  }
  if (!scav::load_pending(loader).empty()) {
    std::fprintf(stderr, "a chart with no includes wanted a document\n");
    return 1;
  }

  scav::Chart chart;
  if (!scav::load_finish(loader, chart, diags)) {
    std::fprintf(stderr, "load_finish reported a diagnostic\n");
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

// The layout inputs through both installed headers: a shipped profile and the
// router registry, C++ and C.
int check_layout() {
  scav_profile profile{};
  if (!scav::profile_named("readable", profile) || !scav::profile_validate(profile)) {
    std::fprintf(stderr, "the shipped profile failed to load or validate\n");
    return 1;
  }
  scav_profile c_profile{};
  if ((scav_profile_named("compact", &c_profile) != SCAV_OK) ||
      (scav_profile_validate(&c_profile) != SCAV_OK)) {
    std::fprintf(stderr, "the C profile surface failed\n");
    return 1;
  }
  scav_router_id router{ 0 };
  scav_byte const *name{ nullptr };
  uint32_t len{ 0 };
  if ((scav_router_name(0, &name, &len) != SCAV_OK) ||
      (scav_router_by_name(name, len, &router) != SCAV_OK)) {
    std::fprintf(stderr, "the router registry failed\n");
    return 1;
  }
  std::printf("consumer layout ok: %u routers\n", scav::router_count());
  return 0;
}

}  // namespace

int main() {
  if (int const rc = check_core(); rc != 0) { return rc; }
  if (int const rc = check_model(); rc != 0) { return rc; }
  if (int const rc = check_layout(); rc != 0) { return rc; }
  return check_abi();
}
