// Compiled with the consumer's own flags -- exceptions and RTTI on -- against an
// archive built without either. The answer must be the same.
//
// It also reaches every public header of every library, with nothing on the
// include path but the installed prefix. That is what makes the public/private
// split real rather than a convention: a public header that includes a private
// one, or names a type only a private header declares, fails to compile here and
// nowhere else.

#include <scav/scav_core.h>
#include <scav/scav_toy.h>
#include <scav/scav_types.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

int check_toy() {
  char const *const text{ "scav" };
  auto const *const bytes{ reinterpret_cast<scav_byte const *>(text) };
  auto const len{ static_cast<uint32_t>(std::strlen(text)) };

  // Pinned, so a consumer that links but computes something else fails rather
  // than passing silently.
  constexpr uint64_t EXPECTED{ 0xf75ced18b5176da0ULL };

  uint64_t const actual{ scav_toy_checksum(bytes, len) };
  if (actual != EXPECTED) {
    std::fprintf(stderr,
                 "scav_toy_checksum(\"scav\") = %016llx, expected %016llx\n",
                 static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(EXPECTED));
    return 1;
  }
  std::printf("consumer toy ok: %016llx\n", static_cast<unsigned long long>(actual));
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
  if (int const rc = check_toy(); rc != 0) { return rc; }
  return check_core();
}
