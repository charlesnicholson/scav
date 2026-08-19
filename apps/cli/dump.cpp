// The model, entity rows rather than syntax: for a terminal by default, `--json`
// for a consumer that is not C++, `--hash` for comparing two transports.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cli {

namespace {

// The text dump ============================================================

// Where a statement started, as "file:line". False when the element has no
// statement -- built from code, or the chart row of a hand-made model.
struct Loc {
  std::string_view file;
  uint32_t line;
};

bool stmt_loc(Chart const &c, StmtId stmt, Loc &out) {
  if ((stmt.v == INVALID) || (stmt.v >= c.stmts.size())) { return false; }
  Statement const &st{ c.stmts[stmt.v] };
  if (st.doc.v >= c.documents.size()) { return false; }
  Document const &doc{ c.documents[st.doc.v] };
  LineCol const lc{ diag_line_col(c.src_bytes.data() + doc.text.off,
                                  doc.text.len,
                                  st.src.off - doc.text.off) };
  out = { .file = chart_string(c, doc.path), .line = lc.line };
  return true;
}

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
  std::string const path{ [&] {
    std::string p;
    chart_path_of(c, id, p);
    return p;
  }() };
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
      if (uint32_t const owner{ c.states[t.src.v].parent.v }; owner < by_sub.size()) {
        by_sub[owner].push_back(i);
      }
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
          chart_path_of(c, t.src, out);
          out += " -> ";
          chart_path_of(c, t.dst, out);
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

// The JSON projection ======================================================

// One array per entity array, one field per row field, ids as numbers and
// INVALID as null. Output only: no comments, and no canonical byte form.

void append_json_string(std::string &out, std::string_view text) {
  constexpr std::string_view HEX{ "0123456789abcdef" };
  out += '"';
  for (char const ch : text) {
    scav_byte const b{ static_cast<scav_byte>(ch) };
    switch (b) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (b < 0x20U) {
          out += "\\u00";
          out += HEX[(b >> 4U) & 0xFU];
          out += HEX[b & 0xFU];
        } else {
          out += ch;  // UTF-8 passes through, which JSON permits
        }
        break;
    }
  }
  out += '"';
}

// `null` rather than 4294967295, so a consumer need not know the sentinel.
void append_json_id(std::string &out, uint32_t id) {
  if (id == INVALID) {
    out += "null";
    return;
  }
  append_u32(out, id);
}

// One `{...}` object under construction. `n` counts fields written, which is
// what puts the separator before the second and not the first.
struct Row {
  std::string *out;
  uint32_t n;
};

void row_key(Row &r, std::string_view key) {
  if (r.n++ != 0) { *r.out += ", "; }
  append_json_string(*r.out, key);
  *r.out += ": ";
}

void row_str(Row &r, std::string_view key, std::string_view value) {
  row_key(r, key);
  append_json_string(*r.out, value);
}

void row_num(Row &r, std::string_view key, uint32_t value) {
  row_key(r, key);
  append_u32(*r.out, value);
}

void row_id(Row &r, std::string_view key, uint32_t id) {
  row_key(r, key);
  append_json_id(*r.out, id);
}

template <typename Ids>
void row_ids(Row &r, std::string_view key, Ids const &ids, Span span) {
  row_key(r, key);
  *r.out += '[';
  for (uint32_t i = 0; i < span.len; ++i) {
    uint32_t const at{ span.off + i };
    if (at >= ids.size()) { break; }
    if (i != 0) { *r.out += ", "; }
    append_u32(*r.out, ids[at].v);
  }
  *r.out += ']';
}

// A span reads as the row indices it covers, which is how a consumer reaches
// into the flat array it points at.
void row_range(Row &r, std::string_view key, Span span) {
  row_key(r, key);
  *r.out += '[';
  for (uint32_t i = 0; i < span.len; ++i) {
    if (i != 0) { *r.out += ", "; }
    append_u32(*r.out, span.off + i);
  }
  *r.out += ']';
}

// `"name": [` then one indented object per row, closing the array. `write` takes
// the row index and fills the object.
template <typename Write>
void append_json_array(std::string &out,
                       std::string_view name,
                       size_t count,
                       Write write) {
  out += "  ";
  append_json_string(out, name);
  out += ": [\n";
  for (size_t i = 0; i < count; ++i) {
    out += "    {";
    Row row{ .out = &out, .n = 0 };
    write(row, static_cast<uint32_t>(i));
    out += ((i + 1) < count) ? "},\n" : "}\n";
  }
  out += "  ]";
}

char const *json_value_kind_name(ValueKind kind) {
  switch (kind) {
    case ValueKind::U32: return "u32";
    case ValueKind::I32: return "i32";
    case ValueKind::U64: return "u64";
    case ValueKind::I64: return "i64";
    case ValueKind::StrRef: return "strref";
    case ValueKind::Span: return "span";
    case ValueKind::Blob: return "blob";
    case ValueKind::Pod: return "pod";
  }
  return "unknown";
}

char const *json_elem_kind_name(ElemKind kind) {
  switch (kind) {
    case ElemKind::State: return "state";
    case ElemKind::Submachine: return "submachine";
    case ElemKind::Transition: return "transition";
    case ElemKind::Chart: return "chart";
    case ElemKind::Point: return "point";
    case ElemKind::PathBox: return "pathbox";
    case ElemKind::None: return "none";
  }
  return "unknown";
}

void append_json(std::string &out, Chart const &c) {
  out += "{\n  \"chart\": {";
  Row chart{ .out = &out, .n = 0 };
  row_str(chart, "name", chart_string(c, c.name));
  row_str(chart, "label", chart_string(c, c.label));
  row_id(chart, "root_submachine", c.root_submachine.v);
  row_range(chart, "attrs", c.chart_attrs);
  row_num(chart, "structural_hash", chart_structural_hash(c));
  out += "},\n";

  append_json_array(out, "documents", c.documents.size(), [&](Row &r, uint32_t i) {
    row_str(r, "path", chart_string(c, c.documents[i].path));
  });
  out += ",\n";

  append_json_array(out, "states", c.states.size(), [&](Row &r, uint32_t i) {
    State const &s{ c.states[i] };
    row_str(r, "name", chart_string(c, s.name));
    row_str(r, "label", chart_string(c, s.label));
    row_str(r, "kind", syntax_state_kind_name(s.kind));
    row_id(r, "parent", s.parent.v);
    row_ids(r, "submachines", c.submachine_ids, s.submachines);
    row_range(r, "attrs", s.attrs);
    row_id(r, "stmt", s.stmt.v);
    row_id(r, "inst", s.inst.v);
    row_num(r, "live", s.live);
  });
  out += ",\n";

  append_json_array(out, "submachines", c.submachines.size(), [&](Row &r, uint32_t i) {
    Submachine const &m{ c.submachines[i] };
    row_str(r, "name", chart_string(c, m.name));
    row_str(r, "label", chart_string(c, m.label));
    row_id(r, "owner", m.owner.v);
    row_num(r, "ordinal", m.ordinal);
    row_ids(r, "children", c.state_ids, m.children);
    row_range(r, "attrs", m.attrs);
    row_id(r, "stmt", m.stmt.v);
    row_id(r, "inst", m.inst.v);
    row_num(r, "live", m.live);
  });
  out += ",\n";

  append_json_array(out, "transitions", c.transitions.size(), [&](Row &r, uint32_t i) {
    Transition const &t{ c.transitions[i] };
    row_id(r, "src", t.src.v);
    row_id(r, "dst", t.dst.v);
    row_str(r, "kind", syntax_trans_kind_name(t.kind));
    row_str(r, "label", chart_string(c, t.label));
    row_range(r, "attrs", t.attrs);
    row_id(r, "stmt", t.stmt.v);
    row_id(r, "inst", t.inst.v);
    row_num(r, "live", t.live);
  });
  out += ",\n";

  append_json_array(out, "includes", c.includes.size(), [&](Row &r, uint32_t i) {
    Include const &inc{ c.includes[i] };
    row_str(r, "alias", chart_string(c, inc.alias));
    row_str(r, "path", chart_string(c, inc.path));
    row_id(r, "target", inc.target.v);
    row_id(r, "host", inc.host.v);
    row_id(r, "stmt", inc.stmt.v);
  });
  out += ",\n";

  append_json_array(out, "attrs", c.attrs.size(), [&](Row &r, uint32_t i) {
    Attr const &a{ c.attrs[i] };
    row_str(r, "key", chart_attr_key(c, a.key));
    row_str(r, "value", chart_string(c, a.value));
    row_id(r, "stmt", a.stmt.v);
  });
  out += ",\n";

  // Descriptors only: a column's bytes are typed by its registrant, and JSON has
  // no spelling for a `pod` this build may not understand.
  append_json_array(out, "columns", c.columns.size(), [&](Row &r, uint32_t i) {
    ColumnDesc const &d{ c.columns[i].desc };
    row_str(r, "name", string_pool_view(c.column_names, d.name));
    row_str(r, "entity", json_elem_kind_name(d.entity));
    row_str(r, "kind", json_value_kind_name(d.kind));
    row_num(r, "elem_size", d.elem_size);
    row_num(r, "elem_align", d.elem_align);
    row_num(r, "flags", d.flags);
    row_num(r, "count", column_count(c, ColumnId{ i }));
  });
  out += "\n}\n";
}

}  // namespace

int run_dump(char const *path, bool hash_only, bool as_json) {
  Network net;
  load_network(path, true, net);
  if (net.code == EXIT_UNUSABLE) { return EXIT_UNUSABLE; }

  std::string out;
  if (hash_only) {
    append_hash(out, chart_structural_hash(net.chart));
  } else if (as_json) {
    append_json(out, net.chart);
  } else {
    append_model(out, net.chart);
  }
  write_stream(out, stdout);
  return net.code;
}

}  // namespace cli
