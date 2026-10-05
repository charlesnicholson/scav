// `scav dump`: the model's entity rows as text or `--json`, or its structural hash
// with `--hash`; `--layout` adds geometry.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_draw.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace cli {

namespace {

// The text dump ============================================================

// Where a statement starts: document path and one-based line.
struct Loc {
  std::string_view file;
  uint32_t line;
};

// False when `stmt` names no statement in `c`, as for an element built from code.
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
  string_append_u32(out, loc.line);
  out += ')';
}

void append_indent(std::string &out, uint32_t depth) {
  for (uint32_t i = 0; i < depth; ++i) { out += "  "; }
}

void append_quoted(std::string &out, std::string_view text) {
  out += " \"";
  out += text;  // verbatim, unescaped
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

  // Every reference is bounds-checked; dump also prints models validation rejected.
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
          string_append_u32(out, m.ordinal);
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
          if (t.kind != TransKind::Default) {
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

  // The include edges, after the tree: each alias and the document it instantiates.
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

// The geometry projection ==================================================

// Appends `v` in decimal, INT64_MIN included.
void append_i64v(std::string &out, int64_t v) {
  bool const neg{ v < 0 };
  uint64_t mag{ neg ? (~static_cast<uint64_t>(v) + 1U) : static_cast<uint64_t>(v) };
  std::array<char, 20> digits{};
  uint32_t n{ 0 };
  do {
    digits[n++] = static_cast<char>('0' + static_cast<char>(mag % 10U));
    mag /= 10U;
  } while (mag != 0U);
  if (neg) { out += '-'; }
  while (n != 0U) { out += digits[--n]; }
}

// Appends a signed grid coordinate in decimal.
void append_i32v(std::string &out, int32_t v) {
  if (v < 0) {
    out += '-';
    string_append_u32(out, static_cast<uint32_t>(-static_cast<int64_t>(v)));
    return;
  }
  string_append_u32(out, static_cast<uint32_t>(v));
}

// A geometry column's rows, memcpy'd out of the type-erased bytes.
template <typename T>
std::vector<T> geom_rows(Chart const &c, char const *name) {
  ColumnId const id{ column_find(c, name) };
  if (id.v == INVALID) { return {}; }
  std::vector<T> rows(column_count(c, id));
  if (!rows.empty()) {
    std::memcpy(rows.data(), column_data(c, id), rows.size() * sizeof(T));
  }
  return rows;
}

void append_rect(std::string &out, scav_rect r) {
  append_i32v(out, r.x);
  out += ',';
  append_i32v(out, r.y);
  out += ' ';
  append_i32v(out, r.w);
  out += 'x';
  append_i32v(out, r.h);
}

// Tier-2 term names in CostTerms order, the order `cost_shares` returns.
constexpr std::array<char const *, TIER2_TERMS> TERMS{
  "bends",  "corridor",      "crossings", "excess_len", "adjacency",
  "label",  "label_near",    "aspect",    "area",       "crowding",
  "length", "transit_bends", "whitespace"
};

// Per Tier-2 term, the power of the em it is divided by before weighting: 0 counts, 1
// lengths, 2 areas.
constexpr std::array<uint32_t, TIER2_TERMS> EM_POWER{
  0, 1, 0, 1, 0, 0, 1, 1, 2, 1, 1, 0, 2
};

std::array<int64_t, TIER2_TERMS> term_values(CostTerms const &t) {
  return { t.bends,  t.corridor,      t.crossings, t.excess_len, t.adjacency,
           t.label,  t.label_near,    t.aspect,    t.area,       t.crowding,
           t.length, t.transit_bends, t.whitespace };
}

// The profile values layout reads and the spacing it derives from them, parallel to
// `profile_values`.
constexpr std::array<char const *, 34> PROFILE{ "em",
                                                "line_height",
                                                "pad",
                                                "rank_sep",
                                                "node_sep",
                                                "sub_sep",
                                                "dar_num",
                                                "dar_den",
                                                "route_clearance",
                                                "border_band",
                                                "bend_penalty",
                                                "label_leader",
                                                "loop_gap",
                                                "loop_reach",
                                                "loop_lane",
                                                "w_bends",
                                                "w_corridor",
                                                "w_crossings",
                                                "w_excess_len",
                                                "w_adjacency",
                                                "w_label",
                                                "w_label_near",
                                                "w_aspect",
                                                "w_area",
                                                "w_crowding",
                                                "w_length",
                                                "w_transit_bends",
                                                "w_whitespace",
                                                "portfolio_m",
                                                "lane_pitch",
                                                "portfolio_k",
                                                "sweep_count",
                                                "spacing_inflation_cap",
                                                "spacing_inflation_increment" };

int64_t imax64(int64_t a, int64_t b) { return (a > b) ? a : b; }

std::array<int64_t, PROFILE.size()> profile_values(scav_profile const &p) {
  return { p.font_size_grid,
           label_line_height(p),
           p.pad,
           p.rank_sep,
           p.node_sep,
           p.sub_sep,
           p.dar_num,
           p.dar_den,
           route_clearance(p),
           border_band(p),
           route_bend_penalty(p),
           label_leader(p),
           loop_gap(p),
           loop_reach(p),
           loop_lane(p),
           p.w_bends,
           p.w_corridor,
           p.w_crossings,
           p.w_excess_len,
           p.w_adjacency,
           p.w_label,
           p.w_label_near,
           p.w_aspect,
           p.w_area,
           p.w_crowding,
           p.w_length,
           p.w_transit_bends,
           p.w_whitespace,
           p.portfolio_m,
           imax64(route_clearance(p), p.font_size_grid),  // the orthogonal router's lanes
           p.portfolio_k,
           p.sweep_count,
           p.spacing_inflation_cap,
           p.spacing_inflation_increment };
}

// Appends ` name value` per state kind: `kind_min_w` and `kind_min_h`, the least interior.
template <typename Write>
void each_kind_min(scav_profile const &p, Write write) {
  for (uint32_t k = 0; k < 9; ++k) {
    std::string name{ "min_w." };
    name += syntax_state_kind_name(static_cast<StateKind>(k));
    write(name, p.kind_min_w[k]);
    name.replace(0, 5, "min_h");
    write(name, p.kind_min_h[k]);
  }
}

// The least straight run at each end of transition `t`, zero past the table.
scav_path_clear trans_clear(scav_spaces const &s, uint32_t t) {
  return (t < s.n_path_clear) ? s.path_clear[t] : scav_path_clear{};
}

// `scav_box_space::ruled` per state, 0 past the table.
uint32_t state_ruled(scav_spaces const &s, uint32_t st) {
  return (st < s.n_box_state) ? s.box_state[st].ruled : 0U;
}

void append_geometry_text(std::string &out,
                          Chart const &c,
                          CostTerms const &terms,
                          LayoutArgs const &args,
                          scav_profile const &p,
                          std::vector<scav_placed> const &placed,
                          scav_spaces const &s,
                          uint32_t row,
                          SearchPins const &pins) {
  auto const state{ geom_rows<scav_rect>(c, "scav.geom.state") };
  auto const before{ geom_rows<scav_rect>(c, "scav.geom.state_before") };
  auto const after{ geom_rows<scav_rect>(c, "scav.geom.state_after") };
  auto const lead{ geom_rows<scav_rect>(c, "scav.geom.state_lead") };
  auto const trail{ geom_rows<scav_rect>(c, "scav.geom.state_trail") };
  auto const loop{ geom_rows<scav_rect>(c, "scav.geom.state_loop") };
  auto const loop_place{ geom_rows<uint32_t>(c, "scav.geom.state_loop_place") };
  auto const sub{ geom_rows<scav_rect>(c, "scav.geom.sub") };
  auto const routes{ geom_rows<scav_span>(c, "scav.geom.route") };
  auto const points{ geom_rows<scav_point>(c, "scav.geom.point") };
  auto const port_spans{ geom_rows<scav_span>(c, "scav.geom.port") };
  auto const slots{ geom_rows<scav_port_slot>(c, "scav.geom.portslot") };

  out += "geometry structural ";
  string_append_hex32(out, layout_structural_hash(c));
  out += " coordinate ";
  string_append_hex32(out, layout_coordinate_hash(c));
  out += "\n  chart ";
  append_rect(out, geom_rows<scav_rect>(c, "scav.geom.chart")[0]);
  out += '\n';
  if (row != INVALID) {
    out += "  rests on ";
    append_layout_args(out, args, row, pins);
    out += '\n';
  }

  // The objective over those columns, printed beside the geometry it scored.
  Cost const scored{ cost_of(terms, p) };
  std::array<int64_t, TIER2_TERMS> const values{ term_values(terms) };
  std::array<int64_t, TIER2_TERMS> const shares{ cost_shares(terms, p) };
  out += "  cost t0 ";
  append_i32v(out, scored.t0_violations);
  out += " t2 ";
  append_i64v(out, scored.t2);
  out += "\n    tier0";
  std::array<int32_t, TIER0_TERMS> const t0{ tier0_terms(terms) };
  for (uint32_t i = 0; i < TIER0_TERMS; ++i) {
    out += ' ';
    out += TIER0_NAMES[i];
    out += ' ';
    append_i32v(out, t0[i]);
  }
  out += '\n';
  for (uint32_t i = 0; i < TIER2_TERMS; ++i) {
    out += "    ";
    out += TERMS[i];
    out += ' ';
    append_i64v(out, values[i]);
    out += " em_power ";
    string_append_u32(out, EM_POWER[i]);
    out += " share ";
    append_i64v(out, shares[i]);
    out += "bp\n";
  }
  out += "  profile";
  std::array<int64_t, PROFILE.size()> const prof{ profile_values(p) };
  for (uint32_t i = 0; i < PROFILE.size(); ++i) {
    out += ' ';
    out += PROFILE[i];
    out += ' ';
    append_i64v(out, prof[i]);
  }
  each_kind_min(p, [&](std::string const &name, int32_t v) {
    out += ' ';
    out += name;
    out += ' ';
    append_i32v(out, v);
  });
  out += '\n';
  for (uint32_t i = 0; i < placed.size(); ++i) {
    out += "  placed t";
    string_append_u32(out, (i < s.n_path_box) ? s.path_box[i].subject : INVALID);
    out += ' ';
    append_rect(out, placed[i]);
    out += '\n';
  }
  for (uint32_t i = 0; i < state.size(); ++i) {
    if (c.states[i].live == 0) { continue; }
    out += "  state ";
    chart_path_of(c, { i }, out);
    out += ' ';
    append_rect(out, state[i]);
    out += " before ";
    append_rect(out, before[i]);
    out += " after ";
    append_rect(out, after[i]);
    if ((i < lead.size()) && (i < trail.size()) &&
        ((lead[i].w != 0) || (trail[i].w != 0))) {
      out += " lead ";
      append_rect(out, lead[i]);
      out += " trail ";
      append_rect(out, trail[i]);
    }
    if (int32_t const r{ state_corner_radius(c.states[i].kind, state[i], p.pad) };
        r != 0) {
      out += " corner ";
      append_i32v(out, r);
    }
    if (state_ruled(s, i) != 0) {
      out += " ruled ";
      string_append_u32(out, state_ruled(s, i));
    }
    if ((i < loop.size()) && (i < loop_place.size()) &&
        ((loop[i].w != 0) || (loop[i].h != 0))) {
      out += " loop ";
      append_rect(out, loop[i]);
      out += " face ";
      string_append_u32(out, loop_place[i] / 2U);
      out += " end ";
      string_append_u32(out, loop_place[i] % 2U);
    }
    out += '\n';
  }
  std::vector<OccupiedSpan> occupied;
  layout_occupied_spans(c, p, occupied);
  for (OccupiedSpan const &o : occupied) {
    out += "  occupied ";
    chart_path_of(c, { o.obstacle }, out);
    out += " face ";
    string_append_u32(out, o.face);
    out += ' ';
    append_i32v(out, o.lo);
    out += "..";
    append_i32v(out, o.lo + o.len);
    out += '\n';
  }
  for (uint32_t m = 0; m < sub.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    out += "  sub ";
    string_append_u32(out, m);
    out += ' ';
    append_rect(out, sub[m]);
    out += '\n';
  }
  for (uint32_t t = 0; t < routes.size(); ++t) {
    if ((c.transitions[t].live == 0) || (routes[t].len == 0)) { continue; }
    out += "  route ";
    chart_path_of(c, c.transitions[t].src, out);
    out += " -> ";
    chart_path_of(c, c.transitions[t].dst, out);
    for (uint32_t k = 0; k < routes[t].len; ++k) {
      scav_point const pt{ points[routes[t].off + k] };
      out += " (";
      append_i32v(out, pt.x);
      out += ',';
      append_i32v(out, pt.y);
      out += ')';
    }
    scav_path_clear const clear{ trans_clear(s, t) };
    if ((clear.src != 0) || (clear.dst != 0)) {
      out += " clear ";
      append_i32v(out, clear.src);
      out += ' ';
      append_i32v(out, clear.dst);
    }
    for (uint32_t k = 0; k < port_spans[t].len; ++k) {
      scav_port_slot const sl{ slots[port_spans[t].off + k] };
      out += " port s";
      string_append_u32(out, sl.side);
      out += " d";
      string_append_u32(out, sl.boundary_depth);
      out += " (";
      append_i32v(out, sl.x);
      out += ',';
      append_i32v(out, sl.y);
      out += ')';
    }
    out += '\n';
  }
}

// The JSON projection ======================================================

// One array per entity array, one field per row field, ids as numbers and
// INVALID as null.

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
          out += ch;  // UTF-8 passes through unescaped
        }
        break;
    }
  }
  out += '"';
}

// An id, or `null` for INVALID.
void append_json_id(std::string &out, uint32_t id) {
  if (id == INVALID) {
    out += "null";
    return;
  }
  string_append_u32(out, id);
}

// One `{...}` object under construction; `n` counts the fields written.
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
  string_append_u32(*r.out, value);
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
    string_append_u32(*r.out, ids[at].v);
  }
  *r.out += ']';
}

// Writes a span as the list of row indices it covers.
void row_range(Row &r, std::string_view key, Span span) {
  row_key(r, key);
  *r.out += '[';
  for (uint32_t i = 0; i < span.len; ++i) {
    if (i != 0) { *r.out += ", "; }
    string_append_u32(*r.out, span.off + i);
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

void append_json_rect(std::string &out, scav_rect r) {
  out += '[';
  append_i32v(out, r.x);
  out += ", ";
  append_i32v(out, r.y);
  out += ", ";
  append_i32v(out, r.w);
  out += ", ";
  append_i32v(out, r.h);
  out += ']';
}

// The geometry as JSON arrays indexed by entity ordinal.
void append_geometry_json(std::string &out,
                          Chart const &c,
                          CostTerms const &terms,
                          LayoutArgs const &args,
                          scav_profile const &p,
                          std::vector<scav_placed> const &placed,
                          scav_spaces const &s,
                          uint32_t row,
                          SearchPins const &pins) {
  out += ",\n  \"geometry\": {\n    \"gen\": ";
  string_append_u32(out, geom_rows<uint32_t>(c, "scav.geom.gen")[0]);
  out += ",\n    \"structural_hash\": ";
  string_append_u32(out, layout_structural_hash(c));
  out += ",\n    \"coordinate_hash\": ";
  string_append_u32(out, layout_coordinate_hash(c));
  out += ",\n    \"chart\": ";
  append_json_rect(out, geom_rows<scav_rect>(c, "scav.geom.chart")[0]);
  if (row != INVALID) {
    out += ",\n    \"rests_on\": \"";
    append_layout_args(out, args, row, pins);
    out += '"';
  }

  // The objective over those columns, printed beside the geometry it scored.
  Cost const scored{ cost_of(terms, p) };
  std::array<int64_t, TIER2_TERMS> const values{ term_values(terms) };
  std::array<int64_t, TIER2_TERMS> const shares{ cost_shares(terms, p) };
  out += ",\n    \"cost\": {\n      \"t0_violations\": ";
  append_i32v(out, scored.t0_violations);
  out += ",\n      \"t2\": ";
  append_i64v(out, scored.t2);
  out += ",\n      \"tier0\": {";
  std::array<int32_t, TIER0_TERMS> const t0{ tier0_terms(terms) };
  for (uint32_t i = 0; i < TIER0_TERMS; ++i) {
    if (i != 0) { out += ", "; }
    append_json_string(out, TIER0_NAMES[i]);
    out += ": ";
    append_i32v(out, t0[i]);
  }
  out += '}';
  // Tier 2 per term: its value, its share of t2 and its em power, in CostTerms order.
  auto const tier2 = [&](char const *key, auto const &v) {
    out += ",\n      \"";
    out += key;
    out += "\": {";
    for (uint32_t i = 0; i < TIER2_TERMS; ++i) {
      if (i != 0) { out += ", "; }
      append_json_string(out, TERMS[i]);
      out += ": ";
      append_i64v(out, static_cast<int64_t>(v[i]));
    }
    out += '}';
  };
  tier2("tier2", values);
  tier2("shares_bp", shares);
  tier2("em_power", EM_POWER);
  out += "\n    }";

  out += ",\n    \"profile\": {";
  std::array<int64_t, PROFILE.size()> const prof{ profile_values(p) };
  for (uint32_t i = 0; i < PROFILE.size(); ++i) {
    if (i != 0) { out += ", "; }
    append_json_string(out, PROFILE[i]);
    out += ": ";
    append_i64v(out, prof[i]);
  }
  each_kind_min(p, [&](std::string const &name, int32_t v) {
    out += ", ";
    append_json_string(out, name);
    out += ": ";
    append_i32v(out, v);
  });
  out += "},\n    \"placed\": [";
  for (uint32_t i = 0; i < placed.size(); ++i) {
    if (i != 0) { out += ", "; }
    append_json_rect(out, placed[i]);
  }
  // The transition each placed box belongs to, or INVALID past `n_path_box`.
  out += "],\n    \"placed_subject\": [";
  for (uint32_t i = 0; i < placed.size(); ++i) {
    if (i != 0) { out += ", "; }
    string_append_u32(out, (i < s.n_path_box) ? s.path_box[i].subject : INVALID);
  }
  out += ']';

  for (char const *name : { "scav.geom.state",
                            "scav.geom.state_before",
                            "scav.geom.state_after",
                            "scav.geom.state_lead",
                            "scav.geom.state_trail",
                            "scav.geom.state_loop",
                            "scav.geom.sub" }) {
    out += ",\n    ";
    append_json_string(out, std::string_view{ name }.substr(10));  // "state", ...
    out += ": [";
    auto const rows{ geom_rows<scav_rect>(c, name) };
    for (uint32_t i = 0; i < rows.size(); ++i) {
      if (i != 0) { out += ", "; }
      append_json_rect(out, rows[i]);
    }
    out += ']';
  }

  // Per state: its loop room's [face, end], its corner radius and its bands' ruled bits.
  auto const loop_place{ geom_rows<uint32_t>(c, "scav.geom.state_loop_place") };
  out += ",\n    \"state_loop_place\": [";
  for (uint32_t i = 0; i < loop_place.size(); ++i) {
    if (i != 0) { out += ", "; }
    out += '[';
    string_append_u32(out, loop_place[i] / 2U);
    out += ", ";
    string_append_u32(out, loop_place[i] % 2U);
    out += ']';
  }
  auto const state{ geom_rows<scav_rect>(c, "scav.geom.state") };
  out += "],\n    \"state_corner\": [";
  for (uint32_t i = 0; i < state.size(); ++i) {
    if (i != 0) { out += ", "; }
    append_i32v(out, state_corner_radius(c.states[i].kind, state[i], p.pad));
  }
  out += "],\n    \"state_ruled\": [";
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (i != 0) { out += ", "; }
    string_append_u32(out, state_ruled(s, i));
  }
  out += "],\n    \"path_clear\": [";
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if (t != 0) { out += ", "; }
    scav_path_clear const clear{ trans_clear(s, t) };
    out += '[';
    append_i32v(out, clear.src);
    out += ", ";
    append_i32v(out, clear.dst);
    out += ']';
  }
  // Each inner loop's [state, face, lo, len] on a face of its state.
  std::vector<OccupiedSpan> occupied;
  layout_occupied_spans(c, p, occupied);
  out += "],\n    \"occupied\": [";
  for (uint32_t i = 0; i < occupied.size(); ++i) {
    if (i != 0) { out += ", "; }
    out += '[';
    string_append_u32(out, occupied[i].obstacle);
    out += ", ";
    string_append_u32(out, occupied[i].face);
    out += ", ";
    append_i32v(out, occupied[i].lo);
    out += ", ";
    append_i32v(out, occupied[i].len);
    out += ']';
  }
  out += ']';

  auto const routes{ geom_rows<scav_span>(c, "scav.geom.route") };
  auto const points{ geom_rows<scav_point>(c, "scav.geom.point") };
  auto const port_spans{ geom_rows<scav_span>(c, "scav.geom.port") };
  auto const slots{ geom_rows<scav_port_slot>(c, "scav.geom.portslot") };
  out += ",\n    \"route\": [";
  for (uint32_t t = 0; t < routes.size(); ++t) {
    if (t != 0) { out += ", "; }
    out += '[';
    for (uint32_t k = 0; k < routes[t].len; ++k) {
      if (k != 0) { out += ", "; }
      scav_point const pt{ points[routes[t].off + k] };
      out += '[';
      append_i32v(out, pt.x);
      out += ", ";
      append_i32v(out, pt.y);
      out += ']';
    }
    out += ']';
  }
  out += "],\n    \"port\": [";
  for (uint32_t t = 0; t < port_spans.size(); ++t) {
    if (t != 0) { out += ", "; }
    out += '[';
    for (uint32_t k = 0; k < port_spans[t].len; ++k) {
      if (k != 0) { out += ", "; }
      scav_port_slot const sl{ slots[port_spans[t].off + k] };
      out += '[';
      append_i32v(out, sl.x);
      out += ", ";
      append_i32v(out, sl.y);
      out += ", ";
      string_append_u32(out, sl.side);
      out += ", ";
      string_append_u32(out, sl.boundary_depth);
      out += ']';
    }
    out += ']';
  }
  out += "]\n  }";
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

  // Column descriptors and row counts; the column bytes are omitted.
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
}

}  // namespace

int run_dump(char const *path,
             bool hash_only,
             bool as_json,
             bool with_layout,
             bool trace,
             bool trace_search,
             LayoutArgs const &args) {
  Loaded net;
  load_and_report(path, true, net);
  if (net.code == EXIT_UNUSABLE) { return EXIT_UNUSABLE; }

  scav_layout_opts opts{};
  if (!profile_named(args.profile, opts.profile)) {
    write_error("no such profile", args.profile);
    return EXIT_UNUSABLE;
  }
  if (args.no_search) { opts.profile.portfolio_k = 0; }
  CostTerms cost{};
  // The winning row and taken pins, for `rests on`; INVALID under `--trace`.
  uint32_t won{ INVALID };
  SearchPins taken;
  std::vector<scav_placed> placed;
  Spaces spaces;
  if (with_layout) {
    // The reference builder's measurement pass, on the bundled font.
    Metrics metrics;
    if (!args.no_text && (!metrics_create(nullptr, 0, metrics) ||
                          !measure_chart(net.chart, metrics, opts.profile, spaces))) {
      write_error("cannot measure the chart with the bundled font", path);
      return EXIT_UNUSABLE;
    }
    std::vector<Diagnostic> diags;
    std::vector<char> events;
    bool const laid{ trace ? layout_trace_json(
                                 net.chart,
                                 as_spaces(spaces),
                                 opts,
                                 placed,
                                 diags,
                                 events,
                                 args.row,
                                 trace_search ? TraceScope::Search : TraceScope::Shipped)
                           : layout_run(net.chart,
                                        as_spaces(spaces),
                                        opts,
                                        placed,
                                        diags,
                                        nullptr,
                                        &won,
                                        args.row,
                                        nullptr,
                                        &taken,
                                        &args.pins) };
    // The trace goes to stdout, ahead of the model dump.
    if (trace) { write_stream(std::string{ events.begin(), events.end() }, stdout); }
    if (!diags.empty()) {
      std::string err;
      for (Diagnostic const &d : diags) { diag_append(err, net.chart, d, path); }
      write_stream(err, stderr);
    }
    if (!laid) { return EXIT_DIAGNOSED; }
    // Scores this run's columns; `label` and `label_near` read the placed boxes.
    cost = layout_cost(net.chart, opts.profile, as_spaces(spaces), placed);
  }

  std::string out;
  if (hash_only) {
    string_append_hex32(out, chart_structural_hash(net.chart));
    out += '\n';
  } else if (as_json) {
    append_json(out, net.chart);
    if (with_layout) {
      append_geometry_json(out,
                           net.chart,
                           cost,
                           args,
                           opts.profile,
                           placed,
                           as_spaces(spaces),
                           won,
                           taken);
    }
    out += "\n}\n";
  } else {
    append_model(out, net.chart);
    if (with_layout) {
      append_geometry_text(out,
                           net.chart,
                           cost,
                           args,
                           opts.profile,
                           placed,
                           as_spaces(spaces),
                           won,
                           taken);
    }
  }
  write_stream(out, stdout);
  return net.code;
}

}  // namespace cli
