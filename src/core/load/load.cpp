// Two walks. Discovery covers documents, claiming a DocId per unseen key, which
// `pending` reports. Finish covers instantiations, then resolves endpoints.

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

void report(LoadSession &s, DiagCode code, DocId doc, Span src) {
  s.diags.push_back({ .code = code, .doc = doc, .src = src });
}

// Document-local: no chart exists yet, so this indexes the bytes
// load_document_bytes hands back.
Span stmt_span(LoadSession const &s, DocId doc, uint32_t row) {
  if ((doc.v >= s.parsed.size()) || (row >= s.parsed[doc.v].stmts.size())) { return {}; }
  return s.parsed[doc.v].stmts[row].src;
}

DocId find_doc(LoadSession const &s, std::string_view key) {
  for (uint32_t i = 0; i < s.docs.size(); ++i) {
    if (string_pool_view(s.paths, s.docs[i].name) == key) { return { i }; }
  }
  return { INVALID };
}

// Claims the next DocId for `key`, or returns the one it already has. The
// parsed slot is created empty and filled when the bytes arrive.
DocId claim_doc(LoadSession &s, std::string_view key, DocId from, uint32_t stmt_row) {
  if (DocId const found{ find_doc(s, key) }; found.v != INVALID) { return found; }
  DocId const id{ narrow_clamp<uint32_t>(s.docs.size()) };
  s.docs.push_back({ .name = string_pool_add(s.paths, key),
                     .arrived = 0,
                     .edges = {},
                     .from = from,
                     .stmt_row = stmt_row });
  s.parsed.emplace_back();
  return id;
}

// Every include statement of `doc` in ascending statement row. A flat scan of
// the statement array, so nesting is irrelevant and the order is source order.
bool discover(LoadSession &s, DocId doc) {
  std::string const base{ load_document_name(s, doc) };
  std::vector<DiscoveredRef> refs;
  bool ok{ true };

  {
    ParsedDocument const &pd{ s.parsed[doc.v] };
    refs.reserve(pd.includes.size());
    for (uint32_t row = 0; row < pd.stmts.size(); ++row) {
      if (pd.stmts[row].kind != StmtKind::Include) { continue; }
      IncludeStmt const &inc{ pd.includes[pd.stmt_payload[row]] };
      std::string key;
      if (!path_resolve(base, string_pool_view(pd.strings, inc.path), key)) {
        report(s, DiagCode::IncludePathInvalid, doc, pd.stmts[row].src);
        ok = false;
        continue;
      }
      refs.push_back({ .row = row, .key = std::move(key) });
    }
  }

  // Claiming grows s.docs and s.parsed, so it happens after the scan above; a
  // reference into either would not survive it.
  uint32_t const off{ narrow_clamp<uint32_t>(s.edges.size()) };
  for (DiscoveredRef const &ref : refs) {
    DocId const to{ claim_doc(s, ref.key, doc, ref.row) };
    s.edges.push_back({ .from = doc, .to = to, .stmt_row = ref.row });
  }
  s.docs[doc.v].edges = make_span(off, narrow_clamp<uint32_t>(s.edges.size()) - off);
  return ok;
}

// The index of the edge closing the first cycle, or INVALID. Iterative DFS,
// roots in DocId order and edges in claim order, so the report is fixed.
uint32_t first_back_edge(LoadSession const &s) {
  constexpr uint32_t WHITE{ 0 };
  constexpr uint32_t GRAY{ 1 };
  constexpr uint32_t BLACK{ 2 };
  std::vector<uint32_t> color(s.docs.size(), WHITE);

  struct Frame {
    uint32_t doc;
    uint32_t next;  // how many of this document's edges are walked
  };
  std::vector<Frame> stack;

  for (uint32_t root = 0; root < s.docs.size(); ++root) {
    if (color[root] != WHITE) { continue; }
    color[root] = GRAY;
    stack.push_back({ .doc = root, .next = 0 });
    while (!stack.empty()) {
      Frame &f{ stack.back() };
      Span const edges{ s.docs[f.doc].edges };
      if (f.next == edges.len) {
        color[f.doc] = BLACK;
        stack.pop_back();
        continue;
      }
      uint32_t const at{ edges.off + f.next };
      f.next += 1;
      uint32_t const to{ s.edges[at].to.v };
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
DocId edge_target(LoadSession const &s, DocId doc, uint32_t row) {
  Span const edges{ s.docs[doc.v].edges };
  uint32_t lo{ 0 };
  uint32_t hi{ edges.len };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2U) };
    uint32_t const at{ s.edges[edges.off + mid].stmt_row };
    if (at == row) { return s.edges[edges.off + mid].to; }
    if (at < row) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return { INVALID };
}

// Everything the session found before a chart existed. Production order is
// already DocId order then statement row, so it is not re-sorted.
bool report_session_diags(LoadSession const &s, std::vector<Diagnostic> &diags) {
  diags.insert(diags.end(), s.diags.begin(), s.diags.end());
  return false;
}

}  // namespace

std::string_view load_document_name(LoadSession const &s, DocId doc) {
  if (doc.v >= s.docs.size()) { return {}; }
  return string_pool_view(s.paths, s.docs[doc.v].name);
}

bool load_document_bytes(LoadSession const &s,
                         DocId doc,
                         scav_byte const **out,
                         uint32_t *len) {
  if ((doc.v >= s.docs.size()) || (s.docs[doc.v].arrived == 0)) { return false; }
  ParsedDocument const &pd{ s.parsed[doc.v] };
  *out = pd.src_bytes.data();
  *len = narrow_clamp<uint32_t>(pd.src_bytes.size());
  return true;
}

bool load_add(LoadSession &s, scav_byte const *bytes, size_t len, std::string_view name) {
  if (s.poisoned != 0) { return false; }

  // Normalized like any include path, so `./charts/vac.scav` and
  // `charts/vac.scav` are one key and the whole network is spelled alike.
  DocId doc{ INVALID };
  if (s.docs.empty()) {
    std::string key;
    if (!path_resolve({}, name, key)) {
      report(s, DiagCode::IncludePathInvalid, { 0 }, {});
      s.poisoned = 1;
      return false;
    }
    doc = claim_doc(s, key, { INVALID }, INVALID);
  } else {
    doc = find_doc(s, name);
    if (doc.v == INVALID) {
      report(s, DiagCode::DocumentNotRequested, { INVALID }, {});
      s.poisoned = 1;
      return false;
    }
    if (s.docs[doc.v].arrived != 0) {
      report(s, DiagCode::DocumentAlreadyLoaded, doc, {});
      s.poisoned = 1;
      return false;
    }
  }

  // Parsed under its resolved key, so a diagnostic quotes the name the network
  // knows it by rather than one caller's spelling.
  ParsedDocument pd;
  size_t const mark{ s.diags.size() };
  bool const parsed{ parse_document(bytes,
                                    len,
                                    load_document_name(s, doc),
                                    parse_default_options(),
                                    pd,
                                    s.diags) };
  // parse_document handles one document and is not told which, so it stamps
  // DocId 0 on everything. Only the session knows the real one.
  for (size_t i = mark; i < s.diags.size(); ++i) { s.diags[i].doc = doc; }

  // Kept even when the parse failed: normalization already filled src_bytes,
  // which is what the diagnostics just recorded point into.
  s.parsed[doc.v] = std::move(pd);
  s.docs[doc.v].arrived = 1;
  if (!parsed) {
    s.poisoned = 1;
    return false;
  }

  if (!discover(s, doc)) {
    s.poisoned = 1;
    return false;
  }
  return true;
}

std::vector<Pending> const &load_pending(LoadSession &s) {
  s.pending.clear();
  for (uint32_t i = 0; i < s.docs.size(); ++i) {
    LoadDoc const &doc{ s.docs[i] };
    if (doc.arrived != 0) { continue; }
    s.pending.push_back({ .path = make_span(doc.name.off, doc.name.len),
                          .from = doc.from,
                          .stmt_row = doc.stmt_row });
  }
  return s.pending;
}

bool load_finish(LoadSession &s, Chart &out, std::vector<Diagnostic> &diags) {
  if (s.poisoned != 0) { return report_session_diags(s, diags); }
  if (s.docs.empty()) {
    report(s, DiagCode::LoadSessionEmpty, { INVALID }, {});
    return report_session_diags(s, diags);
  }
  if (!out.documents.empty() || !out.submachines.empty()) {
    report(s, DiagCode::LoadSessionEmpty, { INVALID }, {});
    return report_session_diags(s, diags);
  }

  // Reported against the requesting document, where the offending include
  // statement lives.
  bool complete{ true };
  for (uint32_t i = 0; i < s.docs.size(); ++i) {
    LoadDoc const &doc{ s.docs[i] };
    if (doc.arrived != 0) { continue; }
    report(s,
           DiagCode::IncludePathUnresolved,
           doc.from,
           stmt_span(s, doc.from, doc.stmt_row));
    complete = false;
  }
  if (!complete) { return report_session_diags(s, diags); }

  if (uint32_t const bad{ first_back_edge(s) }; bad != INVALID) {
    IncludeEdge const &edge{ s.edges[bad] };
    report(s, DiagCode::IncludeCycle, edge.from, stmt_span(s, edge.from, edge.stmt_row));
    return report_session_diags(s, diags);
  }

  // Past here every document is attached, so a finding is an ordinary chart
  // diagnostic and its span indexes out.src_bytes.
  std::vector<uint32_t> stmt_base(s.docs.size(), 0);
  std::vector<ParsedDocument const *> docs;
  docs.reserve(s.docs.size());
  for (uint32_t d = 0; d < s.docs.size(); ++d) {
    model_attach_document(out, s.parsed[d], stmt_base[d]);
    docs.push_back(&s.parsed[d]);
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
    bool const ok{ model_instantiate(out, s.parsed[job.doc.v], job, trans, incs, diags) };
    clean = clean && ok;

    for (size_t k = mark; k < incs.size(); ++k) {
      PendingInc const &inc{ incs[k] };
      DocId const target{ edge_target(s, inc.doc, inc.row) };
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
    report(s, DiagCode::IncludeExpansionTooLarge, { 0 }, {});
    return report_session_diags(s, diags);
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
