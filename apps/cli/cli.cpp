// Loading and diagnostic reporting, shared by every verb. A diagnostic carries a
// code plus a span or an entity, so a position is derived here.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace cli {

namespace {

// Diagnostics print as file:line:col against whichever byte pool produced them:
// the loader's buffers, or the chart's via a span or a subject's statement.
void append_diag_line(std::string &out,
                      char const *path,
                      LineCol const *lc,
                      DiagCode code) {
  out += path;
  if (lc != nullptr) {
    out += ':';
    append_u32(out, lc->line);
    out += ':';
    append_u32(out, lc->column);
  }
  out += ": ";
  out += diag_message(code);
  out += '\n';
}

// A finding from before any chart existed -- a parse error, a cycle, a missing
// document. Its span indexes the bytes the loader still holds.
void append_loader_diag(std::string &out,
                        char const *path,
                        Loader const &loader,
                        Diagnostic const &d) {
  std::string_view const name{ load_document_name(loader, d.doc) };
  std::string const where{ name.empty() ? std::string{ path } : std::string{ name } };
  scav_byte const *bytes{ nullptr };
  uint32_t len{ 0 };
  if ((d.src.len != 0) && load_document_bytes(loader, d.doc, &bytes, &len) &&
      ((static_cast<size_t>(d.src.off) + d.src.len) <= len)) {
    LineCol const lc{ diag_line_col(bytes, len, d.src.off) };
    append_diag_line(out, where.c_str(), &lc, d.code);
    return;
  }
  append_diag_line(out, where.c_str(), nullptr, d.code);
}

void append_chart_diag(std::string &out,
                       char const *path,
                       Chart const &c,
                       Diagnostic const &d) {
  // A model diagnostic carries only its subject, so the position comes from
  // walking to that subject's statement.
  StmtId const stmt{ [&]() -> StmtId {
    if ((d.src.len != 0) || !chart_ref_valid(c, d.subject)) { return { INVALID }; }
    switch (d.subject.kind) {
      case ElemKind::State: return c.states[d.subject.ordinal].stmt;
      case ElemKind::Submachine: return c.submachines[d.subject.ordinal].stmt;
      case ElemKind::Transition: return c.transitions[d.subject.ordinal].stmt;
      case ElemKind::Chart:
        return (c.root_submachine.v >= c.submachines.size())
                   ? StmtId{ INVALID }
                   : c.submachines[c.root_submachine.v].stmt;
      case ElemKind::Point:
      case ElemKind::PathBox:
      case ElemKind::None: break;
    }
    return { INVALID };
  }() };
  bool const walked{ (stmt.v != INVALID) && (stmt.v < c.stmts.size()) };
  Span const span{ walked ? c.stmts[stmt.v].src : d.src };
  DocId const doc{ walked ? c.stmts[stmt.v].doc : d.doc };
  // The document carrying the statement, which in a network is often not the
  // one named on the command line.
  if ((span.len != 0) && (doc.v < c.documents.size())) {
    Document const &document{ c.documents[doc.v] };
    std::string const where{ chart_string(c, document.path) };
    LineCol const lc{ diag_line_col(c.src_bytes.data() + document.text.off,
                                    document.text.len,
                                    span.off - document.text.off) };
    append_diag_line(out, where.empty() ? path : where.c_str(), &lc, d.code);
    return;
  }
  append_diag_line(out, path, nullptr, d.code);
}

}  // namespace

void write_stream(std::string const &text, std::FILE *to) {
  std::ignore = std::fwrite(text.data(), 1, text.size(), to);
}

bool read_source(char const *path, std::vector<scav_byte> &out) {
  if (read_file(path, out)) { return true; }
  std::string err{ "scav: cannot read '" };
  err += path;
  err += "'\n";
  write_stream(err, stderr);
  return false;
}

bool write_source(char const *path, std::string const &text) {
  // <cstdio> rather than an ofstream, matching core's reason: the global stream
  // objects would otherwise land in every consumer's static-init.
  std::FILE *const f{ std::fopen(path, "wb") };
  bool ok{ f != nullptr };
  if (ok) {
    ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (std::fclose(f) == 0) && ok;
  }
  if (!ok) {
    std::string err{ "scav: cannot write '" };
    err += path;
    err += "'\n";
    write_stream(err, stderr);
  }
  return ok;
}

void append_u32(std::string &out, uint32_t value) {
  std::array<char, 10> digits{};
  uint32_t n{ 0 };
  do {
    digits[n++] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  } while (value != 0);
  while (n-- > 0) { out += digits[n]; }
}

void append_hash(std::string &out, uint32_t value) {
  constexpr std::string_view DIGITS{ "0123456789abcdef" };
  for (uint32_t i = 8; i-- > 0;) { out += DIGITS[(value >> (i * 4U)) & 0xFU]; }
  out += '\n';
}

void load_network(char const *path, bool validate, Network &out) {
  std::string failed;
  bool const loaded{ load_file(path, out.loader, out.chart, out.diags, failed) };

  std::string err;
  if (!failed.empty()) {
    err += "scav: cannot read '";
    err += failed;
    err += "'\n";
    write_stream(err, stderr);
    out.code = EXIT_UNUSABLE;
    return;
  }

  // A load that never reached a chart leaves nothing to print, and its findings
  // index the loader's buffers rather than a chart's.
  if (out.chart.documents.empty()) {
    for (Diagnostic const &d : out.diags) {
      append_loader_diag(err, path, out.loader, d);
    }
    write_stream(err, stderr);
    out.code = EXIT_UNUSABLE;
    return;
  }

  bool clean{ loaded };
  if (validate) { clean = validate_chart(out.chart, out.diags) && clean; }
  for (Diagnostic const &d : out.diags) { append_chart_diag(err, path, out.chart, d); }
  write_stream(err, stderr);
  out.code = clean ? EXIT_CLEAN : EXIT_DIAGNOSED;
}

}  // namespace cli
