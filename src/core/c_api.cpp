// The C projection of scav_core.h. Each function converts its arguments, calls
// one core function, and converts the result; a handle owns its C++ objects.

#include "scav/scav_core_c.h"

#include "core/c_api_internal.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace {

// Copied field by field rather than reinterpreted. The layouts match today and
// nothing enforces that they keep matching.
scav_pending to_abi(scav::Pending const &p) {
  return { .path = { .off = p.path.off, .len = p.path.len },
           .from = p.from.v,
           .stmt_row = p.stmt_row };
}

// Whichever diagnostic vector is current: the loader's before finish, the
// handle's after. Outside extern "C", which forbids returning a std:: type.
std::vector<scav::Diagnostic> const &diags_of(scav_load const *loader) {
  return (loader->finished != 0) ? loader->diags : loader->loader.diags;
}

}  // namespace

extern "C" {

uint32_t scav_abi_version(void) { return 3; }

scav_result scav_load_begin(scav_load **out) {
  if (out == nullptr) { return SCAV_E_INVALID_ARG; }
  *out = new scav_load{ .loader = {}, .diags = {}, .pending = {}, .finished = 0 };
  return SCAV_OK;
}

scav_result scav_load_add(scav_load *loader,
                          scav_byte const *bytes,
                          uint32_t len,
                          char const *name) {
  if ((loader == nullptr) || (name == nullptr)) { return SCAV_E_INVALID_ARG; }
  if ((bytes == nullptr) && (len != 0)) { return SCAV_E_INVALID_ARG; }
  if (loader->finished != 0) { return SCAV_E_STATE; }
  return scav::load_add(loader->loader, bytes, len, name) ? SCAV_OK : SCAV_E_LOAD;
}

scav_result scav_load_pending(scav_load *loader,
                              scav_pending const **out,
                              uint32_t *out_count) {
  if ((loader == nullptr) || (out == nullptr) || (out_count == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  if (loader->finished != 0) { return SCAV_E_STATE; }
  loader->pending.clear();
  for (scav::Pending const &p : scav::load_pending(loader->loader)) {
    loader->pending.push_back(to_abi(p));
  }
  *out = loader->pending.data();
  *out_count = static_cast<uint32_t>(loader->pending.size());
  return SCAV_OK;
}

scav_result scav_load_path(scav_load const *loader,
                           scav_span path,
                           scav_byte const **out,
                           uint32_t *out_len) {
  if ((loader == nullptr) || (out == nullptr) || (out_len == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  std::vector<scav_byte> const &pool{ loader->loader.paths.bytes };
  if ((static_cast<uint64_t>(path.off) + path.len) > pool.size()) {
    return SCAV_E_INVALID_ARG;
  }
  *out = pool.data() + path.off;
  *out_len = path.len;
  return SCAV_OK;
}

scav_result scav_load_finish(scav_load *loader, scav_chart **out) {
  if ((loader == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  if (loader->finished != 0) { return SCAV_E_STATE; }
  loader->finished = 1;
  *out = nullptr;

  scav_chart *const built{ new scav_chart{ .chart = {} } };
  bool const ok{ scav::load_finish(loader->loader, built->chart, loader->diags) };
  if (built->chart.documents.empty()) {
    // Nothing was built. An empty chart would read as an empty network rather
    // than a failed one.
    delete built;
    return SCAV_E_LOAD;
  }
  *out = built;
  return ok ? SCAV_OK : SCAV_E_LOAD;
}

void scav_load_destroy(scav_load *loader) { delete loader; }

void scav_chart_destroy(scav_chart *chart) { delete chart; }

scav_result scav_load_diag_count(scav_load const *loader, uint32_t *out_count) {
  if ((loader == nullptr) || (out_count == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out_count = static_cast<uint32_t>(diags_of(loader).size());
  return SCAV_OK;
}

scav_result scav_load_diag(scav_load const *loader,
                           uint32_t index,
                           uint32_t *out_code,
                           uint32_t *out_doc,
                           uint32_t *out_off,
                           uint32_t *out_len) {
  if ((loader == nullptr) || (out_code == nullptr) || (out_doc == nullptr) ||
      (out_off == nullptr) || (out_len == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  std::vector<scav::Diagnostic> const &from{ diags_of(loader) };
  if (index >= from.size()) { return SCAV_E_INVALID_ARG; }
  scav::Diagnostic const &d{ from[index] };
  *out_code = static_cast<uint32_t>(d.code);
  *out_doc = d.doc.v;
  *out_off = d.src.off;
  *out_len = d.src.len;
  return SCAV_OK;
}

char const *scav_diag_message(uint32_t code) {
  return scav::diag_message(static_cast<scav::DiagCode>(code));
}

scav_result scav_load_document_name(scav_load const *loader,
                                    uint32_t doc,
                                    scav_byte const **out,
                                    uint32_t *out_len) {
  if ((loader == nullptr) || (out == nullptr) || (out_len == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  std::string_view const name{ scav::load_document_name(loader->loader, { doc }) };
  if (name.empty()) { return SCAV_E_INVALID_ARG; }
  *out = reinterpret_cast<scav_byte const *>(name.data());
  *out_len = static_cast<uint32_t>(name.size());
  return SCAV_OK;
}

scav_result scav_chart_counts(scav_chart const *chart,
                              uint32_t *out_documents,
                              uint32_t *out_states,
                              uint32_t *out_submachines,
                              uint32_t *out_transitions,
                              uint32_t *out_includes) {
  if ((chart == nullptr) || (out_documents == nullptr) || (out_states == nullptr) ||
      (out_submachines == nullptr) || (out_transitions == nullptr) ||
      (out_includes == nullptr)) {
    return SCAV_E_INVALID_ARG;
  }
  *out_documents = static_cast<uint32_t>(chart->chart.documents.size());
  *out_states = static_cast<uint32_t>(chart->chart.states.size());
  *out_submachines = static_cast<uint32_t>(chart->chart.submachines.size());
  *out_transitions = static_cast<uint32_t>(chart->chart.transitions.size());
  *out_includes = static_cast<uint32_t>(chart->chart.includes.size());
  return SCAV_OK;
}

scav_result scav_chart_diag_count(scav_chart const *chart, uint32_t *out_count) {
  if ((chart == nullptr) || (out_count == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out_count = static_cast<uint32_t>(chart->diags.size());
  return SCAV_OK;
}

scav_result scav_chart_diag(scav_chart const *chart, uint32_t index, scav_diag *out) {
  if ((chart == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  if (index >= chart->diags.size()) { return SCAV_E_INVALID_ARG; }
  scav::Diagnostic const &d{ chart->diags[index] };
  *out = { .code = static_cast<uint32_t>(d.code),
           .subject_kind = static_cast<uint32_t>(d.subject.kind),
           .subject_ordinal = d.subject.ordinal,
           .doc = d.doc.v,
           .off = d.src.off,
           .len = d.src.len };
  return SCAV_OK;
}

scav_result scav_chart_structural_hash(scav_chart const *chart, uint32_t *out) {
  if ((chart == nullptr) || (out == nullptr)) { return SCAV_E_INVALID_ARG; }
  *out = scav::chart_structural_hash(chart->chart);
  return SCAV_OK;
}

scav_result scav_chart_digest(scav_chart const *chart,
                              scav_byte *out,
                              uint32_t cap,
                              uint32_t *out_count) {
  if ((chart == nullptr) || (out_count == nullptr)) { return SCAV_E_INVALID_ARG; }
  // Recomputed per call rather than cached, so it cannot go stale.
  std::vector<scav_byte> digest;
  scav::chart_digest_bytes(chart->chart, digest);
  *out_count = static_cast<uint32_t>(digest.size());
  if (cap == 0) { return SCAV_OK; }
  if ((out == nullptr) || (cap < digest.size())) { return SCAV_E_CAPACITY; }
  for (size_t i = 0; i < digest.size(); ++i) { out[i] = digest[i]; }
  return SCAV_OK;
}

}  // extern "C"
