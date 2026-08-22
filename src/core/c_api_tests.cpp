// The C API driven as C: handles, out-params, error codes. Runs under the
// sanitizers, where a handle-lifetime mistake shows up.

#include "scav/scav_core_c.h"

#include "scav/scav_core.h"
#include "scav_c_handles.h"

#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view span_text(scav_byte const *bytes, uint32_t len) {
  return { reinterpret_cast<char const *>(bytes), len };
}

scav_result add(scav_load *loader, std::string_view text, char const *name) {
  return scav_load_add(loader,
                       reinterpret_cast<scav_byte const *>(text.data()),
                       static_cast<uint32_t>(text.size()),
                       name);
}

struct Doc {
  char const *name;
  std::string_view text;
};

// The corpus the ABI tests share: a diamond with a repeat, the same shape the
// C++ loader tests use.
std::vector<Doc> diamond() {
  return {
    { .name = "root.scav", .text = R"(chart root {
           include "mid.scav" as mid,
           include "leaf.scav" as leaf,
           state A,
           trans * -> A,
           trans A -> mid/M,
         })" },
    { .name = "mid.scav", .text = R"(chart mid {
           include "leaf.scav" as inner,
           state M,
           trans * -> M,
         })" },
    { .name = "leaf.scav", .text = R"(chart leaf { state L, trans * -> L, })" },
  };
}

// Drives one network to completion through the ABI alone. Returns the chart or
// null, exactly as a binding would see it.
scav_chart *drive(std::vector<Doc> const &corpus, scav_load **keep) {
  scav_load *loader{ nullptr };
  REQUIRE(scav_load_begin(&loader) == SCAV_OK);
  *keep = loader;
  if (add(loader, corpus[0].text, corpus[0].name) != SCAV_OK) { return nullptr; }

  for (uint32_t round = 0; round < 64; ++round) {
    scav_pending const *pending{ nullptr };
    uint32_t count{ 0 };
    REQUIRE(scav_load_pending(loader, &pending, &count) == SCAV_OK);
    if (count == 0) { break; }

    // Copied out before the first add, which invalidates the view.
    std::vector<std::string> wanted;
    for (uint32_t i = 0; i < count; ++i) {
      scav_byte const *bytes{ nullptr };
      uint32_t len{ 0 };
      REQUIRE(scav_load_path(loader, pending[i].path, &bytes, &len) == SCAV_OK);
      wanted.emplace_back(span_text(bytes, len));
    }
    for (std::string const &want : wanted) {
      for (Doc const &d : corpus) {
        if (want == d.name) { std::ignore = add(loader, d.text, d.name); }
      }
    }
  }

  scav_chart *chart{ nullptr };
  std::ignore = scav_load_finish(loader, &chart);
  return chart;
}

}  // namespace

TEST_CASE("abi: the version is a number a binding can check") {
  CHECK(scav_abi_version() != 0);
}

TEST_CASE("abi: every entry point rejects a null argument rather than crashing") {
  CHECK(scav_load_begin(nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_load_add(nullptr, nullptr, 0, "x") == SCAV_E_INVALID_ARG);
  CHECK(scav_load_pending(nullptr, nullptr, nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_load_finish(nullptr, nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_chart_counts(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) ==
        SCAV_E_INVALID_ARG);
  CHECK(scav_chart_structural_hash(nullptr, nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_chart_digest(nullptr, nullptr, 0, nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_load_diag_count(nullptr, nullptr) == SCAV_E_INVALID_ARG);
}

TEST_CASE("abi: destroy is idempotent on null") {
  scav_load_destroy(nullptr);
  scav_chart_destroy(nullptr);
}

TEST_CASE("abi: a null pointer with a non-zero length is refused") {
  scav_load *loader{ nullptr };
  REQUIRE(scav_load_begin(&loader) == SCAV_OK);
  CHECK(scav_load_add(loader, nullptr, 7, "x.scav") == SCAV_E_INVALID_ARG);
  scav_load_destroy(loader);
}

TEST_CASE("abi: a single-document load produces a chart with counts and a hash") {
  scav_load *loader{ nullptr };
  REQUIRE(scav_load_begin(&loader) == SCAV_OK);
  REQUIRE(add(loader, "chart c { state A, state B, trans A -> B, }", "c.scav") == SCAV_OK);

  scav_pending const *pending{ nullptr };
  uint32_t count{ 1 };
  REQUIRE(scav_load_pending(loader, &pending, &count) == SCAV_OK);
  CHECK(count == 0);

  scav_chart *chart{ nullptr };
  REQUIRE(scav_load_finish(loader, &chart) == SCAV_OK);
  REQUIRE(chart != nullptr);

  uint32_t docs{ 0 };
  uint32_t states{ 0 };
  uint32_t subs{ 0 };
  uint32_t trans{ 0 };
  uint32_t incs{ 0 };
  REQUIRE(scav_chart_counts(chart, &docs, &states, &subs, &trans, &incs) == SCAV_OK);
  CHECK(docs == 1);
  CHECK(states == 2);
  CHECK(trans == 1);
  CHECK(incs == 0);

  uint32_t hash{ 0 };
  REQUIRE(scav_chart_structural_hash(chart, &hash) == SCAV_OK);
  CHECK(hash != 0);

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}

TEST_CASE("abi: a three-document network resolves through the C surface alone") {
  scav_load *loader{ nullptr };
  scav_chart *chart{ drive(diamond(), &loader) };
  REQUIRE(chart != nullptr);

  uint32_t docs{ 0 };
  uint32_t states{ 0 };
  uint32_t subs{ 0 };
  uint32_t trans{ 0 };
  uint32_t incs{ 0 };
  REQUIRE(scav_chart_counts(chart, &docs, &states, &subs, &trans, &incs) == SCAV_OK);
  CHECK(docs == 3);  // parsed once each
  CHECK(incs == 3);  // and instantiated once per include statement
  CHECK(states > 5);

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}

TEST_CASE("abi: pending names what the loader still needs, resolved") {
  scav_load *loader{ nullptr };
  REQUIRE(scav_load_begin(&loader) == SCAV_OK);
  REQUIRE(add(loader,
              R"(chart a { include "sub/b.scav" as b, state A, })",
              "top/a.scav") == SCAV_OK);

  scav_pending const *pending{ nullptr };
  uint32_t count{ 0 };
  REQUIRE(scav_load_pending(loader, &pending, &count) == SCAV_OK);
  REQUIRE(count == 1);
  CHECK(pending[0].from == 0);

  scav_byte const *bytes{ nullptr };
  uint32_t len{ 0 };
  REQUIRE(scav_load_path(loader, pending[0].path, &bytes, &len) == SCAV_OK);
  CHECK(span_text(bytes, len) == "top/sub/b.scav");

  scav_load_destroy(loader);
}

TEST_CASE("abi: the digest honours the query-then-fill out-param protocol") {
  scav_load *loader{ nullptr };
  scav_chart *chart{ drive(diamond(), &loader) };
  REQUIRE(chart != nullptr);

  uint32_t needed{ 0 };
  REQUIRE(scav_chart_digest(chart, nullptr, 0, &needed) == SCAV_OK);
  REQUIRE(needed != 0);

  // Too small never truncates silently; it says how much was wanted.
  std::vector<scav_byte> small(needed - 1, 0);
  uint32_t again{ 0 };
  CHECK(scav_chart_digest(chart, small.data(), needed - 1, &again) == SCAV_E_CAPACITY);
  CHECK(again == needed);

  std::vector<scav_byte> exact(needed, 0);
  REQUIRE(scav_chart_digest(chart, exact.data(), needed, &again) == SCAV_OK);
  CHECK(again == needed);

  // And the digest really is what the hash is computed over.
  std::vector<scav_byte> roomy(needed + 32, 0);
  REQUIRE(scav_chart_digest(chart, roomy.data(), needed + 32, &again) == SCAV_OK);
  CHECK(again == needed);
  CHECK(std::memcmp(exact.data(), roomy.data(), needed) == 0);

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}

TEST_CASE("abi: a cycle is refused with no chart and a legible diagnostic") {
  std::vector<Doc> const cyclic{
    { .name = "a.scav", .text = R"(chart a { include "b.scav" as b, state A, })" },
    { .name = "b.scav", .text = R"(chart b { state B, include "a.scav" as a, })" },
  };
  scav_load *loader{ nullptr };
  scav_chart *chart{ drive(cyclic, &loader) };
  CHECK(chart == nullptr);

  uint32_t count{ 0 };
  REQUIRE(scav_load_diag_count(loader, &count) == SCAV_OK);
  REQUIRE(count != 0);

  bool saw_cycle{ false };
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t code{ 0 };
    uint32_t doc{ 0 };
    uint32_t off{ 0 };
    uint32_t len{ 0 };
    REQUIRE(scav_load_diag(loader, i, &code, &doc, &off, &len) == SCAV_OK);
    std::string_view const message{ scav_diag_message(code) };
    CHECK_FALSE(message.empty());
    if (message == "include cycle") {
      saw_cycle = true;
      scav_byte const *name{ nullptr };
      uint32_t name_len{ 0 };
      REQUIRE(scav_load_document_name(loader, doc, &name, &name_len) == SCAV_OK);
      CHECK(span_text(name, name_len) == "b.scav");
    }
  }
  CHECK(saw_cycle);
  scav_load_destroy(loader);
}

TEST_CASE("abi: finishing twice is a state error, not a second chart") {
  scav_load *loader{ nullptr };
  REQUIRE(scav_load_begin(&loader) == SCAV_OK);
  REQUIRE(add(loader, "chart c { state A, }", "c.scav") == SCAV_OK);

  scav_chart *first{ nullptr };
  REQUIRE(scav_load_finish(loader, &first) == SCAV_OK);
  REQUIRE(first != nullptr);

  scav_chart *second{ nullptr };
  CHECK(scav_load_finish(loader, &second) == SCAV_E_STATE);
  CHECK(second == nullptr);
  CHECK(add(loader, "chart d { state A, }", "d.scav") == SCAV_E_STATE);

  scav_chart_destroy(first);
  scav_load_destroy(loader);
}

TEST_CASE("abi: two loaders in one process do not share state") {
  // No library-global state and no init call, asserted rather than assumed.
  scav_load *first_loader{ nullptr };
  scav_load *second_loader{ nullptr };
  scav_chart *a{ drive(diamond(), &first_loader) };
  scav_chart *b{ drive(diamond(), &second_loader) };
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);

  uint32_t ha{ 0 };
  uint32_t hb{ 0 };
  REQUIRE(scav_chart_structural_hash(a, &ha) == SCAV_OK);
  REQUIRE(scav_chart_structural_hash(b, &hb) == SCAV_OK);
  CHECK(ha == hb);

  scav_load_destroy(first_loader);  // a chart outlives the loader that made it
  uint32_t after{ 0 };
  REQUIRE(scav_chart_structural_hash(a, &after) == SCAV_OK);
  CHECK(after == ha);

  scav_chart_destroy(a);
  scav_chart_destroy(b);
  scav_load_destroy(second_loader);
}

TEST_CASE("abi: a fresh chart carries no diagnostics") {
  scav_load *loader{ nullptr };
  scav_chart *chart{ drive(diamond(), &loader) };
  REQUIRE(chart != nullptr);

  uint32_t count{ 99 };
  REQUIRE(scav_chart_diag_count(chart, &count) == SCAV_OK);
  CHECK(count == 0);

  scav_diag d{};
  CHECK(scav_chart_diag(chart, 0, &d) == SCAV_E_INVALID_ARG);

  CHECK(scav_chart_diag_count(nullptr, &count) == SCAV_E_INVALID_ARG);
  CHECK(scav_chart_diag_count(chart, nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_chart_diag(nullptr, 0, &d) == SCAV_E_INVALID_ARG);
  CHECK(scav_chart_diag(chart, 0, nullptr) == SCAV_E_INVALID_ARG);

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}

TEST_CASE("abi: a chart diagnostic reads back field for field") {
  scav_load *loader{ nullptr };
  scav_chart *chart{ drive(diamond(), &loader) };
  REQUIRE(chart != nullptr);

  // Planted through the internal definition, the way layout and validation
  // will write them; the C caller sees only the flat struct.
  chart->diags.push_back({ .code = scav::DiagCode::DanglingRef,
                           .subject = { .kind = scav::ElemKind::State, .ordinal = 7 },
                           .doc = { 2 },
                           .src = { .off = 11, .len = 5 } });

  uint32_t count{ 0 };
  REQUIRE(scav_chart_diag_count(chart, &count) == SCAV_OK);
  REQUIRE(count == 1);

  scav_diag d{};
  REQUIRE(scav_chart_diag(chart, 0, &d) == SCAV_OK);
  CHECK(d.code == static_cast<uint32_t>(scav::DiagCode::DanglingRef));
  CHECK(d.subject_kind == static_cast<uint32_t>(scav::ElemKind::State));
  CHECK(d.subject_ordinal == 7);
  CHECK(d.doc == 2);
  CHECK(d.off == 11);
  CHECK(d.len == 5);
  CHECK(scav_diag_message(d.code) != nullptr);

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}

TEST_CASE("abi: a registered column reads back through the three-call accessor") {
  scav_load *loader{ nullptr };
  scav_chart *chart{ drive(diamond(), &loader) };
  REQUIRE(chart != nullptr);

  // Registered through the C++ API the way layout will; the C caller sees
  // only find, data, count.
  scav::ColumnId const id{ scav::column_register(
      chart->chart, "scav.geom.state", scav::ElemKind::State,
      scav::ValueKind::Pod, 16, 4, scav::COLUMN_DERIVED) };
  REQUIRE(id.v != scav::INVALID);
  scav::column_data(chart->chart, id)[0] = 0x5C;

  scav_column_id found{ 0 };
  REQUIRE(scav_column_find(chart, "scav.geom.state", &found) == SCAV_OK);
  CHECK(found == id.v);
  CHECK(scav_column_find(chart, "no.such.column", &found) == SCAV_E_INVALID_ARG);

  scav_byte const *data{ nullptr };
  uint32_t stride{ 0 };
  REQUIRE(scav_column_data(chart, found, &data, &stride) == SCAV_OK);
  CHECK(stride == 16);
  REQUIRE(data != nullptr);
  CHECK(data[0] == 0x5C);

  uint32_t count{ 0 };
  REQUIRE(scav_column_count(chart, found, &count) == SCAV_OK);
  CHECK(count == scav::chart_entity_count(chart->chart, scav::ElemKind::State));

  CHECK(scav_column_data(chart, 999, &data, &stride) == SCAV_E_INVALID_ARG);
  CHECK(scav_column_count(chart, 999, &count) == SCAV_E_INVALID_ARG);

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}

TEST_CASE("abi: scav_str reads the pool a strref names, and only the pool") {
  scav_load *loader{ nullptr };
  scav_chart *chart{ drive(diamond(), &loader) };
  REQUIRE(chart != nullptr);

  // Any named state's name is a span into the chart's string pool.
  scav::StrRef named{};
  for (scav::State const &s : chart->chart.states) {
    if (s.name.len != 0) {
      named = s.name;
      break;
    }
  }
  REQUIRE(named.len != 0);

  scav_byte const *bytes{ nullptr };
  uint32_t len{ 0 };
  scav_span const ref{ .off = named.off, .len = named.len };
  REQUIRE(scav_str(chart, ref, &bytes, &len) == SCAV_OK);
  CHECK(span_text(bytes, len) == scav::chart_string(chart->chart, named));

  scav_span const empty{ .off = 0, .len = 0 };
  REQUIRE(scav_str(chart, empty, &bytes, &len) == SCAV_OK);
  CHECK(bytes == nullptr);
  CHECK(len == 0);

  uint32_t const pool_size{
    static_cast<uint32_t>(chart->chart.strings.bytes.size())
  };
  scav_span const past{ .off = pool_size, .len = 1 };
  CHECK(scav_str(chart, past, &bytes, &len) == SCAV_E_INVALID_ARG);

  scav_chart_destroy(chart);
  scav_load_destroy(loader);
}
