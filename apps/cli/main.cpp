// The scav executable (PRD 3.2). One verb so far: `dump` loads a document from
// a file and prints the model -- the entity rows, not the original syntax --
// each element line ending with the source file and line its declaration
// started on. An application, so file I/O and the stream globals are fair
// game here; the libraries take bytes and return data.

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace scav;

bool read_file(char const *path, std::vector<scav_byte> &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) { return false; }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

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
  out += text;  // verbatim: this is a dump, not the canonical printer (P3)
  out += '"';
}

// A state's own segment: its name, or the $kind synthetic spelling -- the last
// segment of its full path, which is how the model addresses it (PRD 9).
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
    out += '\n';
  }
}

// The containment tree, walked with an explicit stack like every other walk in
// scav: a submachine prints its line, its states, then its transitions; a
// state prints its line, attrs, and submachines. Transitions belong to no
// container in the model, so they group under the submachine holding their
// source state, in array order.
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

  auto const trans_by_sub{ [&] {
    std::vector<std::vector<uint32_t>> by_sub(c.submachines.size());
    for (uint32_t i = 0; i < c.transitions.size(); ++i) {
      Transition const &t{ c.transitions[i] };
      if (t.live == 0) { continue; }
      by_sub[c.states[t.src.v].parent.v].push_back(i);
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
          StateId const child{ c.state_ids[m.children.off + i] };
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
          SubmachineId const sub{ c.submachine_ids[s.submachines.off + i] };
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

  for (Include const &inc : c.includes) {
    append_indent(out, 1);
    out += "include ";
    out += chart_string(c, inc.alias);
    if (inc.target.v == INVALID) {
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

// Diagnostics print as file:line:col against whichever byte pool the phase
// used. Parse diagnostics index the parsed document's buffer; model
// diagnostics carry a span into the chart's, or a subject whose statement
// supplies one.
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

void append_parse_diag(std::string &out,
                       char const *path,
                       ParsedDocument const &pd,
                       Diagnostic const &d) {
  if (static_cast<size_t>(d.src.off) + d.src.len <= pd.src_bytes.size()) {
    LineCol const lc{ diag_line_col(pd.src_bytes.data(), pd.src_bytes.size(), d.src.off) };
    append_diag_line(out, path, &lc, d.code);
    return;
  }
  append_diag_line(out, path, nullptr, d.code);
}

void append_chart_diag(std::string &out,
                       char const *path,
                       Chart const &c,
                       Diagnostic const &d) {
  Span span{ d.src };
  DocId doc{ d.doc };
  if ((span.len == 0) && (d.subject.kind != ElemKind::None)) {
    // A model diagnostic carries only its subject; the position is derived by
    // walking to the subject's statement (PRD 6).
    StmtId stmt{ INVALID };
    switch (d.subject.kind) {
      case ElemKind::State: stmt = c.states[d.subject.ordinal].stmt; break;
      case ElemKind::Submachine: stmt = c.submachines[d.subject.ordinal].stmt; break;
      case ElemKind::Transition: stmt = c.transitions[d.subject.ordinal].stmt; break;
      case ElemKind::Chart:
        stmt = (c.root_submachine.v == INVALID) ? StmtId{ INVALID }
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
  if ((span.len != 0) && (doc.v < c.documents.size())) {
    Document const &document{ c.documents[doc.v] };
    LineCol const lc{ diag_line_col(c.src_bytes.data() + document.text.off,
                                    document.text.len,
                                    span.off - document.text.off) };
    append_diag_line(out, path, &lc, d.code);
    return;
  }
  append_diag_line(out, path, nullptr, d.code);
}

int dump(char const *path) {
  std::string err;
  std::vector<scav_byte> bytes;
  if (!read_file(path, bytes)) {
    err += "scav: cannot read '";
    err += path;
    err += "'\n";
    write_stream(err, stderr);
    return 2;
  }

  ParsedDocument pd;
  std::vector<Diagnostic> parse_diags;
  if (!parse_document(bytes.data(),
                      bytes.size(),
                      path,
                      parse_default_options(),
                      pd,
                      parse_diags)) {
    for (Diagnostic const &d : parse_diags) { append_parse_diag(err, path, pd, d); }
    write_stream(err, stderr);
    return 2;
  }

  Chart c;
  std::vector<Diagnostic> diags;
  bool const clean{ [&] {
    bool const lowered{ lower_document(c, pd, diags) };
    return validate_chart(c, diags) && lowered;
  }() };
  for (Diagnostic const &d : diags) { append_chart_diag(err, path, c, d); }
  write_stream(err, stderr);

  std::string out;
  append_model(out, c);
  write_stream(out, stdout);
  return clean ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
  if ((argc == 3) && (std::string_view{ argv[1] } == "dump")) { return dump(argv[2]); }
  write_stream("usage: scav dump <chart.scav>\n", stderr);
  return 2;
}
