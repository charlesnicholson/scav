// Two walks. Discovery covers documents, claiming a DocId per unseen key, which
// `pending` reports. Finish covers instantiations, then resolves endpoints.

#include "core/core_internal.h"
#include "core/model/model.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scav {

namespace {

// A DAG is not a cycle and still expands exponentially: N documents each
// including the next twice is 2^N instantiations from a few KB of input.
constexpr uint32_t MAX_INSTANTIATIONS{ 1U << 16U };

// One include statement's path, already resolved. Gathered before anything is
// claimed, because claiming grows the pools a view would point into.
struct DiscoveredRef {
  uint32_t row;
  std::string key;
};

void report(Loader &loader, DiagCode code, DocId doc, Span src) {
  loader.diags.push_back({ .code = code, .doc = doc, .src = src });
}

// Document-local: no chart exists yet, so this indexes the bytes
// load_document_bytes hands back.
Span stmt_span(Loader const &loader, DocId doc, uint32_t row) {
  if ((doc.v >= loader.parsed.size()) || (row >= loader.parsed[doc.v].stmts.size())) {
    return {};
  }
  return loader.parsed[doc.v].stmts[row].src;
}

DocId find_doc(Loader const &loader, std::string_view key) {
  for (uint32_t i = 0; i < loader.docs.size(); ++i) {
    if (string_pool_view(loader.paths, loader.docs[i].name) == key) { return { i }; }
  }
  return { INVALID };
}

// Claims the next DocId for `key`, or returns the one it already has. The
// parsed slot is created empty and filled when the bytes arrive.
DocId claim_doc(Loader &loader, std::string_view key, DocId from, uint32_t stmt_row) {
  if (DocId const found{ find_doc(loader, key) }; found.v != INVALID) { return found; }
  DocId const id{ narrow_clamp<uint32_t>(loader.docs.size()) };
  loader.docs.push_back({ .name = string_pool_add(loader.paths, key),
                          .arrived = 0,
                          .edges = {},
                          .from = from,
                          .stmt_row = stmt_row });
  loader.parsed.emplace_back();
  return id;
}

// Every include statement of `doc` in ascending statement row. A flat scan of
// the statement array, so nesting is irrelevant and the order is source order.
bool discover(Loader &loader, DocId doc) {
  std::string const base{ load_document_name(loader, doc) };
  std::vector<DiscoveredRef> refs;
  bool ok{ true };

  {
    ParsedDocument const &pd{ loader.parsed[doc.v] };
    refs.reserve(pd.includes.size());
    for (uint32_t row = 0; row < pd.stmts.size(); ++row) {
      if (pd.stmts[row].kind != StmtKind::Include) { continue; }
      IncludeStmt const &inc{ pd.includes[pd.stmt_payload[row]] };
      std::string key;
      if (!path_resolve(base, string_pool_view(pd.strings, inc.path), key)) {
        report(loader, DiagCode::IncludePathInvalid, doc, pd.stmts[row].src);
        ok = false;
        continue;
      }
      refs.push_back({ .row = row, .key = std::move(key) });
    }
  }

  // Claiming grows s.docs and s.parsed, so it happens after the scan above; a
  // reference into either would not survive it.
  uint32_t const off{ narrow_clamp<uint32_t>(loader.edges.size()) };
  for (DiscoveredRef const &ref : refs) {
    DocId const to{ claim_doc(loader, ref.key, doc, ref.row) };
    loader.edges.push_back({ .from = doc, .to = to, .stmt_row = ref.row });
  }
  loader.docs[doc.v].edges =
      make_span(off, narrow_clamp<uint32_t>(loader.edges.size()) - off);
  return ok;
}

// The index of the edge closing the first cycle, or INVALID. Iterative DFS,
// roots in DocId order and edges in claim order, so the report is fixed.
uint32_t first_back_edge(Loader const &loader) {
  constexpr uint32_t WHITE{ 0 };
  constexpr uint32_t GRAY{ 1 };
  constexpr uint32_t BLACK{ 2 };
  std::vector<uint32_t> color(loader.docs.size(), WHITE);

  struct Frame {
    uint32_t doc;
    uint32_t next;  // how many of this document's edges are walked
  };
  std::vector<Frame> stack;

  for (uint32_t root = 0; root < loader.docs.size(); ++root) {
    if (color[root] != WHITE) { continue; }
    color[root] = GRAY;
    stack.push_back({ .doc = root, .next = 0 });
    while (!stack.empty()) {
      Frame &f{ stack.back() };
      Span const edges{ loader.docs[f.doc].edges };
      if (f.next == edges.len) {
        color[f.doc] = BLACK;
        stack.pop_back();
        continue;
      }
      uint32_t const at{ edges.off + f.next };
      f.next += 1;
      uint32_t const to{ loader.edges[at].to.v };
      if (color[to] == GRAY) { return at; }
      if (color[to] == WHITE) {
        color[to] = GRAY;
        stack.push_back({ .doc = to, .next = 0 });
      }
    }
  }
  return INVALID;
}

// Which document the include statement at `row` of `doc` names. Edges within
// one document are pushed in ascending row, so this is a binary search.
DocId edge_target(Loader const &loader, DocId doc, uint32_t row) {
  Span const edges{ loader.docs[doc.v].edges };
  uint32_t lo{ 0 };
  uint32_t hi{ edges.len };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2U) };
    uint32_t const at{ loader.edges[edges.off + mid].stmt_row };
    if (at == row) { return loader.edges[edges.off + mid].to; }
    if (at < row) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return { INVALID };
}

// Everything the loader found before a chart existed. Production order is
// already DocId order then statement row, so it is not re-sorted.
bool report_loader_diags(Loader const &loader, std::vector<Diagnostic> &diags) {
  diags.insert(diags.end(), loader.diags.begin(), loader.diags.end());
  return false;
}

}  // namespace

std::string_view load_document_name(Loader const &loader, DocId doc) {
  if (doc.v >= loader.docs.size()) { return {}; }
  return string_pool_view(loader.paths, loader.docs[doc.v].name);
}

bool load_document_bytes(Loader const &loader,
                         DocId doc,
                         scav_byte const **out,
                         uint32_t *len) {
  if ((doc.v >= loader.docs.size()) || (loader.docs[doc.v].arrived == 0)) { return false; }
  ParsedDocument const &pd{ loader.parsed[doc.v] };
  *out = pd.src_bytes.data();
  *len = narrow_clamp<uint32_t>(pd.src_bytes.size());
  return true;
}

bool load_add(Loader &loader, scav_byte const *bytes, size_t len, std::string_view name) {
  if (loader.poisoned != 0) { return false; }

  // Normalized like any include path, so `./charts/vac.scav` and
  // `charts/vac.scav` are one key and the whole network is spelled alike.
  DocId doc{ INVALID };
  if (loader.docs.empty()) {
    std::string key;
    if (!path_resolve({}, name, key)) {
      report(loader, DiagCode::IncludePathInvalid, { 0 }, {});
      loader.poisoned = 1;
      return false;
    }
    doc = claim_doc(loader, key, { INVALID }, INVALID);
  } else {
    doc = find_doc(loader, name);
    if (doc.v == INVALID) {
      report(loader, DiagCode::DocumentNotRequested, { INVALID }, {});
      loader.poisoned = 1;
      return false;
    }
    if (loader.docs[doc.v].arrived != 0) {
      report(loader, DiagCode::DocumentAlreadyLoaded, doc, {});
      loader.poisoned = 1;
      return false;
    }
  }

  // Parsed under its resolved key, so a diagnostic quotes the name the network
  // knows it by rather than one caller's spelling.
  ParsedDocument pd;
  size_t const mark{ loader.diags.size() };
  bool const parsed{ parse_document(bytes,
                                    len,
                                    load_document_name(loader, doc),
                                    parse_default_options(),
                                    pd,
                                    loader.diags) };
  // parse_document handles one document and is not told which, so it stamps
  // DocId 0 on everything. Only the loader knows the real one.
  for (size_t i = mark; i < loader.diags.size(); ++i) { loader.diags[i].doc = doc; }

  // Kept even when the parse failed: normalization already filled src_bytes,
  // which is what the diagnostics just recorded point into.
  loader.parsed[doc.v] = std::move(pd);
  loader.docs[doc.v].arrived = 1;
  if (!parsed) {
    loader.poisoned = 1;
    return false;
  }

  if (!discover(loader, doc)) {
    loader.poisoned = 1;
    return false;
  }
  return true;
}

std::vector<Pending> const &load_pending(Loader &loader) {
  loader.pending.clear();
  for (uint32_t i = 0; i < loader.docs.size(); ++i) {
    LoadDoc const &doc{ loader.docs[i] };
    if (doc.arrived != 0) { continue; }
    loader.pending.push_back({ .path = make_span(doc.name.off, doc.name.len),
                               .from = doc.from,
                               .stmt_row = doc.stmt_row });
  }
  return loader.pending;
}

bool load_finish(Loader &loader, Chart &out, std::vector<Diagnostic> &diags) {
  if (loader.poisoned != 0) { return report_loader_diags(loader, diags); }
  if (loader.docs.empty()) {
    report(loader, DiagCode::LoaderEmpty, { INVALID }, {});
    return report_loader_diags(loader, diags);
  }
  if (!out.documents.empty() || !out.submachines.empty()) {
    report(loader, DiagCode::LoaderEmpty, { INVALID }, {});
    return report_loader_diags(loader, diags);
  }

  // Reported against the requesting document, where the offending include
  // statement lives.
  bool complete{ true };
  for (uint32_t i = 0; i < loader.docs.size(); ++i) {
    LoadDoc const &doc{ loader.docs[i] };
    if (doc.arrived != 0) { continue; }
    report(loader,
           DiagCode::IncludePathUnresolved,
           doc.from,
           stmt_span(loader, doc.from, doc.stmt_row));
    complete = false;
  }
  if (!complete) { return report_loader_diags(loader, diags); }

  if (uint32_t const bad{ first_back_edge(loader) }; bad != INVALID) {
    IncludeEdge const &edge{ loader.edges[bad] };
    report(loader,
           DiagCode::IncludeCycle,
           edge.from,
           stmt_span(loader, edge.from, edge.stmt_row));
    return report_loader_diags(loader, diags);
  }

  // Past here every document is attached, so a finding is an ordinary chart
  // diagnostic and its span indexes out.src_bytes.
  std::vector<uint32_t> stmt_base(loader.docs.size(), 0);
  std::vector<ParsedDocument const *> docs;
  docs.reserve(loader.docs.size());
  for (uint32_t d = 0; d < loader.docs.size(); ++d) {
    model_attach_document(out, loader.parsed[d], stmt_base[d]);
    docs.push_back(&loader.parsed[d]);
  }

  bool clean{ true };
  bool overflowed{ false };
  std::vector<PendingTrans> trans;
  std::vector<PendingInc> incs;
  std::vector<InstJob> queue;
  queue.push_back({ .doc = { 0 },
                    .inst = { INVALID },
                    .host = { INVALID },
                    .stmt_base = stmt_base[0] });

  for (size_t at = 0; at < queue.size(); ++at) {
    InstJob const job{ queue[at] };  // copied: instantiating grows the queue
    size_t const mark{ incs.size() };
    bool const ok{
      model_instantiate(out, loader.parsed[job.doc.v], job, trans, incs, diags)
    };
    clean = clean && ok;

    for (size_t k = mark; k < incs.size(); ++k) {
      PendingInc const &inc{ incs[k] };
      DocId const target{ edge_target(loader, inc.doc, inc.row) };
      if (target.v == INVALID) { continue; }  // an invalid path, already reported
      out.includes[inc.inst.v].target = target;
      queue.push_back({ .doc = target,
                        .inst = inc.inst,
                        .host = out.includes[inc.inst.v].host,
                        .stmt_base = stmt_base[target.v] });
    }
    if (queue.size() > MAX_INSTANTIATIONS) {
      overflowed = true;
      break;
    }
  }

  // A chart handed back is a complete network, so an overrun discards it and
  // reports the cap alone rather than a dangling attachment per include.
  if (overflowed) {
    out = Chart{};
    report(loader, DiagCode::IncludeExpansionTooLarge, { 0 }, {});
    return report_loader_diags(loader, diags);
  }

  model_finalize_containment(out);
  bool const resolved{ model_resolve_transitions(out,
                                                 docs.data(),
                                                 narrow_clamp<uint32_t>(docs.size()),
                                                 trans,
                                                 diags) };
  return clean && resolved;
}

}  // namespace scav
