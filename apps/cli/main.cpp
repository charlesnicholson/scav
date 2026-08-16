// The scav executable. `dump` loads a document network and prints its entity
// rows, each line ending with the source file and line that declared it.

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace scav;

// Where a statement started, as "file:line". False when the element has no
// statement -- built from code, or the chart row of a hand-made model.
struct Loc {
  std::string_view file;
  uint32_t line;
};

bool stmt_loc(Chart const &c, StmtId stmt, Loc &out) {
  if (stmt.v == INVALID || stmt.v >= c.stmts.size()) { return false; }
  Statement const &st{ c.stmts[stmt.v] };
  if (st.doc.v >= c.documents.size()) { return false; }
  Document const &doc{ c.documents[st.doc.v] };
  LineCol const lc{ diag_line_col(c.src_bytes.data() + doc.text.off,
                                  doc.text.len,
                                  st.src.off - doc.text.off) };
  out = { .file = chart_string(c, doc.path), .line = lc.line };
  return true;
}

void append_u32(std::string &out, uint32_t v) { out += std::to_string(v); }

void append_loc(std::string &out, Chart const &c, StmtId stmt) {
  Loc loc{};
  if (!stmt_loc(c, stmt, loc)) { return; }
  out += " (";
  out += loc.file;
  out += ':';
  append_u32(out, loc.line);
  out += ')';
}

void append_indent(std::string &out, uint32_t depth) {
  for (uint32_t i = 0; i < depth; ++i) { out += "  "; }
}

void append_quoted(std::string &out, std::string_view text) {
  out += " \"";
  out += text;  // verbatim; this is a dump, not the canonical printer
  out += '"';
}

// A state's own segment: its name, or the `$kind` spelling. The last segment of
// its full path.
void append_state_segment(std::string &out, Chart const &c, StateId id) {
  State const &s{ c.states[id.v] };
  if (s.name.len != 0) {
    out += chart_string(c, s.name);
    return;
  }
  std::string path;
  chart_path_of(c, id, path);
  size_t const slash{ path.rfind('/') };
  out += (slash == std::string::npos) ? path : path.substr(slash + 1);
}

void append_attrs(std::string &out, Chart const &c, ElemRef subject, uint32_t depth) {
  Span const span{ chart_attrs_of(c, subject) };
  for (uint32_t i = 0; i < span.len; ++i) {
    Attr const &a{ c.attrs[span.off + i] };
    append_indent(out, depth);
    out += '@';
    out += chart_attr_key(c, a.key);
    out += " =";
    append_quoted(out, chart_string(c, a.value));
    append_loc(out, c, a.stmt);
    out += '\n';
  }
}

// The containment tree. A transition groups under the submachine holding its
// source state.
void append_model(std::string &out, Chart const &c) {
  out += "chart ";
  out += chart_string(c, c.name);
  if (c.label.len != 0) { append_quoted(out, chart_string(c, c.label)); }
  append_loc(
      out,
      c,
      c.stmts.empty() ? StmtId{ INVALID } : c.submachines[c.root_submachine.v].stmt);
  out += '\n';
  append_attrs(out, c, { .kind = ElemKind::Chart, .ordinal = 0 }, 1);

  // Bounds-checked throughout: dump prints a model validation may have
  // rejected, so no reference here can be assumed good.
  auto const trans_by_sub{ [&] {
    std::vector<std::vector<uint32_t>> by_sub(c.submachines.size());
    for (uint32_t i = 0; i < c.transitions.size(); ++i) {
      Transition const &t{ c.transitions[i] };
      if ((t.live == 0) || (t.src.v >= c.states.size())) { continue; }
      uint32_t const owner{ c.states[t.src.v].parent.v };
      if (owner < by_sub.size()) { by_sub[owner].push_back(i); }
    }
    return by_sub;
  }() };

  enum class What : uint32_t { Sub, State, Trans };
  struct Frame {
    What what;
    uint32_t id;
    uint32_t depth;
  };
  std::vector<Frame> stack;
  if (c.root_submachine.v != INVALID) {
    stack.push_back({ .what = What::Sub, .id = c.root_submachine.v, .depth = 1 });
  }

  while (!stack.empty()) {
    Frame const f{ stack.back() };
    stack.pop_back();

    switch (f.what) {
      case What::Sub: {
        Submachine const &m{ c.submachines[f.id] };
        if (m.live == 0) { break; }
        append_indent(out, f.depth);
        out += "submachine ";
        if (m.name.len != 0) {
          out += chart_string(c, m.name);
        } else {
          out += ':';
          append_u32(out, m.ordinal);
        }
        if (m.label.len != 0) { append_quoted(out, chart_string(c, m.label)); }
        append_loc(out, c, m.stmt);
        out += '\n';
        append_attrs(out,
                     c,
                     { .kind = ElemKind::Submachine, .ordinal = f.id },
                     f.depth + 1);
        // Pushed in reverse so they print in span order: states, then this
        // submachine's transitions after them.
        stack.push_back({ .what = What::Trans, .id = f.id, .depth = f.depth + 1 });
        for (uint32_t i = m.children.len; i-- > 0;) {
          uint32_t const at{ m.children.off + i };
          if (at >= c.state_ids.size()) { continue; }
          StateId const child{ c.state_ids[at] };
          if (child.v >= c.states.size()) { continue; }
          stack.push_back({ .what = What::State, .id = child.v, .depth = f.depth + 1 });
        }
        break;
      }

      case What::State: {
        State const &s{ c.states[f.id] };
        if (s.live == 0) { break; }
        append_indent(out, f.depth);
        out += "state ";
        append_state_segment(out, c, StateId{ f.id });
        if ((s.name.len != 0) && (s.kind != StateKind::Normal)) {
          out += ' ';
          out += syntax_state_kind_name(s.kind);
        }
        if (s.label.len != 0) { append_quoted(out, chart_string(c, s.label)); }
        append_loc(out, c, s.stmt);
        out += '\n';
        append_attrs(out, c, { .kind = ElemKind::State, .ordinal = f.id }, f.depth + 1);
        for (uint32_t i = s.submachines.len; i-- > 0;) {
          uint32_t const at{ s.submachines.off + i };
          if (at >= c.submachine_ids.size()) { continue; }
          SubmachineId const sub{ c.submachine_ids[at] };
          if (sub.v >= c.submachines.size()) { continue; }
          stack.push_back({ .what = What::Sub, .id = sub.v, .depth = f.depth + 1 });
        }
        break;
      }

      case What::Trans: {
        for (uint32_t const idx : trans_by_sub[f.id]) {
          Transition const &t{ c.transitions[idx] };
          append_indent(out, f.depth);
          out += "trans ";
          std::string path;
          chart_path_of(c, t.src, path);
          out += path;
          out += " -> ";
          path.clear();
          chart_path_of(c, t.dst, path);
          out += path;
          if (t.kind != TransKind::External) {
            out += ' ';
            out += syntax_trans_kind_name(t.kind);
          }
          if (t.label.len != 0) { append_quoted(out, chart_string(c, t.label)); }
          append_loc(out, c, t.stmt);
          out += '\n';
          append_attrs(out,
                       c,
                       { .kind = ElemKind::Transition, .ordinal = idx },
                       f.depth + 1);
        }
        break;
      }
    }
  }

  // The edge list, after the tree. An include's content already printed under
  // its alias state; this says which alias instantiates which document.
  for (Include const &inc : c.includes) {
    append_indent(out, 1);
    out += "include ";
    out += chart_string(c, inc.alias);
    append_quoted(out, chart_string(c, inc.path));
    if (inc.target.v >= c.documents.size()) {
      out += " unresolved";
    } else {
      out += " -> ";
      out += chart_string(c, c.documents[inc.target.v].path);
    }
    append_loc(out, c, inc.stmt);
    out += '\n';
  }
}

void write_stream(std::string const &text, std::FILE *to) {
  std::ignore = std::fwrite(text.data(), 1, text.size(), to);
}

// Diagnostics print as file:line:col against whichever byte pool produced them:
// the session's buffers, or the chart's via a span or a subject's statement.
void append_diag_line(std::string &out,
                      char const *path,
                      LineCol const *lc,
                      DiagCode code) {
  out += path;
  if (lc) {
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
// document. Its span indexes the bytes the session still holds.
void append_session_diag(std::string &out,
                         char const *path,
                         LoadSession const &session,
                         Diagnostic const &d) {
  std::string_view const name{ load_document_name(session, d.doc) };
  std::string const where{ name.empty() ? std::string{ path } : std::string{ name } };
  scav_byte const *bytes{ nullptr };
  uint32_t len{ 0 };
  if ((d.src.len != 0) && load_document_bytes(session, d.doc, &bytes, &len) &&
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
  Span span{ d.src };
  DocId doc{ d.doc };
  if ((span.len == 0) && (d.subject.kind != ElemKind::None) &&
      chart_ref_valid(c, d.subject)) {
    // A model diagnostic carries only its subject, so the position comes from
    // walking to that subject's statement.
    StmtId stmt{ INVALID };
    switch (d.subject.kind) {
      case ElemKind::State: stmt = c.states[d.subject.ordinal].stmt; break;
      case ElemKind::Submachine: stmt = c.submachines[d.subject.ordinal].stmt; break;
      case ElemKind::Transition: stmt = c.transitions[d.subject.ordinal].stmt; break;
      case ElemKind::Chart:
        stmt = (c.root_submachine.v >= c.submachines.size())
                   ? StmtId{ INVALID }
                   : c.submachines[c.root_submachine.v].stmt;
        break;
      case ElemKind::Point:
      case ElemKind::PathBox:
      case ElemKind::None: break;
    }
    if ((stmt.v != INVALID) && (stmt.v < c.stmts.size())) {
      span = c.stmts[stmt.v].src;
      doc = c.stmts[stmt.v].doc;
    }
  }
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

// Eight lowercase hex digits and a newline. Hand-rolled rather than formatted,
// so no locale can reach it.
void append_hash(std::string &out, uint32_t value) {
  constexpr std::string_view DIGITS{ "0123456789abcdef" };
  for (uint32_t i = 8; i-- > 0;) { out += DIGITS[(value >> (i * 4U)) & 0xFU]; }
  out += '\n';
}

int dump(char const *path, bool hash_only) {
  std::string err;
  LoadSession session;
  Chart c;
  std::vector<Diagnostic> diags;
  std::string failed;
  bool loaded{ load_file(path, session, c, diags, failed) };

  if (!failed.empty()) {
    err += "scav: cannot read '";
    err += failed;
    err += "'\n";
    write_stream(err, stderr);
    return 2;
  }

  // A load that never reached a chart leaves nothing to print, and its
  // findings index the session's buffers rather than a chart's.
  if (c.documents.empty()) {
    for (Diagnostic const &d : diags) { append_session_diag(err, path, session, d); }
    write_stream(err, stderr);
    return 2;
  }

  loaded = validate_chart(c, diags) && loaded;
  for (Diagnostic const &d : diags) { append_chart_diag(err, path, c, d); }
  write_stream(err, stderr);

  std::string out;
  if (hash_only) {
    append_hash(out, chart_structural_hash(c));
  } else {
    append_model(out, c);
  }
  write_stream(out, stdout);
  return loaded ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
  if ((argc >= 3) && (std::string_view{ argv[1] } == "dump")) {
    // `--hash` prints the model's structural digest instead of the model.
    bool const hash_only{ std::string_view{ argv[2] } == "--hash" };
    if (argc == 3 && !hash_only) { return dump(argv[2], false); }
    if (argc == 4 && hash_only) { return dump(argv[3], true); }
  }
  write_stream("usage: scav dump [--hash] <chart.scav>\n", stderr);
  return 2;
}
