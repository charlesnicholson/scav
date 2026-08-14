// P1's exit gate (§17): build, validate, and walk a depth-16 / 2k-state chart
// from code with no text involved; then the same chart via P0's parser,
// structurally identical.
//
// One GateSpec drives two generators kept side by side: gate_text writes the
// chart as .scav source, gate_build makes the same builder calls in the order
// lowering would make them. The structural compare is field-level -- ids,
// kinds, dereferenced names, span sequences -- and excludes provenance, which
// is exactly the part that legitimately differs between the two paths.

#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

struct GateSpec {
  uint32_t depth;
  uint32_t leaves;
  uint32_t roots;
};

// ~2.2k live states: roots * depth levels * (1 composite + leaves + 1 initial)
// plus a side submachine every fifth level.
constexpr GateSpec GATE{ .depth = 16, .leaves = 6, .roots = 16 };

bool leaf_is_choice(uint32_t j) { return (j % 3U) == 1U; }
bool leaf_has_label(uint32_t j) { return (j % 4U) == 2U; }
bool trans_is_internal(uint32_t j) { return (j % 5U) == 3U; }
bool level_has_side(uint32_t level) { return (level % 5U) == 2U; }

std::string composite_name(uint32_t root, uint32_t level) {
  if (level == 0) { return "T" + std::to_string(root); }
  return "S" + std::to_string(level);
}

std::string leaf_name(uint32_t j) { return "A" + std::to_string(j); }

// The text half. Statement order within a block: the attr, the leaves, the
// side submachine, the nested composite, then every transition -- the order
// gate_build repeats.
void text_level(GateSpec const &spec, uint32_t root, uint32_t level, std::string &out) {
  std::string const my_name{ composite_name(root, level) };
  out += "state " + my_name + (level == 0 ? " \"root\" {\n" : " {\n");
  out += "@doc = \"L" + std::to_string(level) + "\",\n";
  for (uint32_t j = 0; j < spec.leaves; ++j) {
    out += "state " + leaf_name(j);
    if (leaf_is_choice(j)) { out += " choice"; }
    if (leaf_has_label(j)) { out += " \"a leaf\""; }
    out += ",\n";
  }
  if (level_has_side(level)) {
    out += "submachine side \"s\" { state X, trans * -> X, },\n";
  }
  bool const deeper{ level + 1 < spec.depth };
  if (deeper) { text_level(spec, root, level + 1, out); }
  out += "trans * -> A0,\n";
  for (uint32_t j = 0; j < spec.leaves; ++j) {
    out += "trans ";
    if (trans_is_internal(j)) { out += "internal "; }
    out += leaf_name(j) + " -> " + leaf_name((j + 1) % spec.leaves);
    out += " \"EV_" + std::to_string(j) + "\",\n";
  }
  if (deeper) {
    out += "trans A0 -> " + composite_name(root, level + 1) + ",\n";
  } else {
    // The long edge: from the deepest submachine, the first segment climbs
    // all sixteen levels to the root composite.
    out += "trans A0 -> T" + std::to_string(root) + ",\n";
  }
  out += "},\n";
}

std::string gate_text(GateSpec const &spec) {
  std::string out{ "chart gate \"exit gate\" {\n" };
  out += "@flag,\n@ns { a, b = \"v\" },\n@list = [\"x\", \"y\"],\n";
  for (uint32_t r = 0; r < spec.roots; ++r) { text_level(spec, r, 0, out); }
  out += "}\n";
  return out;
}

// The builder half. Mirrors lowering's two passes: entities in document
// order (a state block's implicit submachine created before its children),
// then transitions in statement order, wildcards synthesized as met.
struct GateTrans {
  SubmachineId scope;
  StateId src, dst;  // src INVALID = a `*` source
  TransKind kind;
  std::string label;
};

void build_level(GateSpec const &spec,
                 Chart &c,
                 uint32_t root,
                 uint32_t level,
                 SubmachineId parent,
                 std::vector<GateTrans> &pending) {
  StateId const me{ build_state(c,
                                parent,
                                composite_name(root, level),
                                StateKind::Normal,
                                level == 0 ? "root" : "") };
  SubmachineId const impl{ build_submachine(c, me, {}, {}) };
  build_attr(c, ref(me), "doc", "L" + std::to_string(level));

  std::vector<StateId> leaves;
  leaves.reserve(spec.leaves);
  for (uint32_t j = 0; j < spec.leaves; ++j) {
    leaves.push_back(build_state(c,
                                 impl,
                                 leaf_name(j),
                                 leaf_is_choice(j) ? StateKind::Choice : StateKind::Normal,
                                 leaf_has_label(j) ? "a leaf" : ""));
  }

  if (level_has_side(level)) {
    SubmachineId const side{ build_submachine(c, me, "side", "s") };
    StateId const x{ build_state(c, side, "X", StateKind::Normal, {}) };
    pending.push_back({ .scope = side,
                        .src = { INVALID },
                        .dst = x,
                        .kind = TransKind::External,
                        .label = {} });
  }

  bool const deeper{ level + 1 < spec.depth };
  StateId next{ INVALID };
  if (deeper) {
    build_level(spec, c, root, level + 1, impl, pending);
    // The nested composite was the first state built by that call; find it by
    // name the way resolution would, so this stays a walk and not bookkeeping.
    StateId found{ INVALID };
    REQUIRE(resolve_path(c, impl, composite_name(root, level + 1), found) ==
            ResolveStatus::Ok);
    next = found;
  }

  pending.push_back({ .scope = impl,
                      .src = { INVALID },
                      .dst = leaves[0],
                      .kind = TransKind::External,
                      .label = {} });
  for (uint32_t j = 0; j < spec.leaves; ++j) {
    pending.push_back(
        { .scope = impl,
          .src = leaves[j],
          .dst = leaves[(j + 1) % spec.leaves],
          .kind = trans_is_internal(j) ? TransKind::Internal : TransKind::External,
          .label = "EV_" + std::to_string(j) });
  }
  if (deeper) {
    pending.push_back({ .scope = impl,
                        .src = leaves[0],
                        .dst = next,
                        .kind = TransKind::External,
                        .label = {} });
  } else {
    StateId top{ INVALID };
    REQUIRE(resolve_path(c, impl, "T" + std::to_string(root), top) == ResolveStatus::Ok);
    pending.push_back({ .scope = impl,
                        .src = leaves[0],
                        .dst = top,
                        .kind = TransKind::External,
                        .label = {} });
  }
}

// Wait to create wildcard pseudostates until after all authored states exist,
// in pending order -- the id assignment lowering's transition pass makes.
void gate_build(GateSpec const &spec, Chart &c) {
  SubmachineId const chart_root{ build_chart(c, "gate", "exit gate") };
  build_attr(c, chart_ref(), "flag", "true");
  build_attr(c, chart_ref(), "ns:a", "true");
  build_attr(c, chart_ref(), "ns:b", "v");
  build_attr(c, chart_ref(), "list", "x");
  build_attr(c, chart_ref(), "list", "y");

  std::vector<GateTrans> pending;
  for (uint32_t r = 0; r < spec.roots; ++r) {
    build_level(spec, c, r, 0, chart_root, pending);
  }
  for (GateTrans &t : pending) {
    if (t.src.v == INVALID) {
      t.src = build_state(c, t.scope, {}, StateKind::Initial, {});
    }
    REQUIRE(build_trans(c, t.src, t.dst, t.kind, t.label).v != INVALID);
  }
}

// Field-level structural equality. Provenance (stmt, inst) and the front-end
// slice (documents, stmts, comments, src_bytes) are excluded: they are what
// legitimately differs between a code-built chart and a lowered one. Raw
// state_ids order is not compared either -- only the per-span sequences, which
// are what the order of a shared array means.
void check_same_chart(Chart const &a, Chart const &b) {
  CHECK(chart_string(a, a.name) == chart_string(b, b.name));
  CHECK(chart_string(a, a.label) == chart_string(b, b.label));
  CHECK(a.root_submachine == b.root_submachine);

  auto const same_attrs = [&](Span sa, Span sb, char const *what) {
    INFO("attrs of " << what);
    REQUIRE(sa.len == sb.len);
    for (uint32_t i = 0; i < sa.len; ++i) {
      Attr const &ra{ a.attrs[sa.off + i] };
      Attr const &rb{ b.attrs[sb.off + i] };
      CHECK(chart_attr_key(a, ra.key) == chart_attr_key(b, rb.key));
      CHECK(chart_string(a, ra.value) == chart_string(b, rb.value));
    }
  };

  REQUIRE(a.states.size() == b.states.size());
  for (uint32_t i = 0; i < a.states.size(); ++i) {
    INFO("state " << i);
    State const &sa{ a.states[i] };
    State const &sb{ b.states[i] };
    CHECK(chart_string(a, sa.name) == chart_string(b, sb.name));
    CHECK(chart_string(a, sa.label) == chart_string(b, sb.label));
    CHECK(sa.kind == sb.kind);
    CHECK(sa.parent == sb.parent);
    CHECK(sa.live == sb.live);
    REQUIRE(sa.submachines.len == sb.submachines.len);
    for (uint32_t k = 0; k < sa.submachines.len; ++k) {
      CHECK(a.submachine_ids[sa.submachines.off + k] ==
            b.submachine_ids[sb.submachines.off + k]);
    }
    same_attrs(sa.attrs, sb.attrs, "state");
  }

  REQUIRE(a.submachines.size() == b.submachines.size());
  for (uint32_t i = 0; i < a.submachines.size(); ++i) {
    INFO("submachine " << i);
    Submachine const &ma{ a.submachines[i] };
    Submachine const &mb{ b.submachines[i] };
    CHECK(ma.owner == mb.owner);
    CHECK(ma.ordinal == mb.ordinal);
    CHECK(chart_string(a, ma.name) == chart_string(b, mb.name));
    CHECK(chart_string(a, ma.label) == chart_string(b, mb.label));
    CHECK(ma.live == mb.live);
    REQUIRE(ma.children.len == mb.children.len);
    for (uint32_t k = 0; k < ma.children.len; ++k) {
      CHECK(a.state_ids[ma.children.off + k] == b.state_ids[mb.children.off + k]);
    }
    same_attrs(ma.attrs, mb.attrs, "submachine");
  }

  REQUIRE(a.transitions.size() == b.transitions.size());
  for (uint32_t i = 0; i < a.transitions.size(); ++i) {
    INFO("transition " << i);
    Transition const &ta{ a.transitions[i] };
    Transition const &tb{ b.transitions[i] };
    CHECK(ta.src == tb.src);
    CHECK(ta.dst == tb.dst);
    CHECK(ta.kind == tb.kind);
    CHECK(ta.live == tb.live);
    CHECK(chart_string(a, ta.label) == chart_string(b, tb.label));
    same_attrs(ta.attrs, tb.attrs, "transition");
  }

  REQUIRE(a.includes.size() == b.includes.size());
  for (uint32_t i = 0; i < a.includes.size(); ++i) {
    CHECK(chart_string(a, a.includes[i].alias) == chart_string(b, b.includes[i].alias));
    CHECK(a.includes[i].host == b.includes[i].host);
    CHECK(a.includes[i].target == b.includes[i].target);
  }

  same_attrs(a.chart_attrs, b.chart_attrs, "chart");
}

}  // namespace

TEST_CASE(
    "exit gate: a depth-16 / 2k-state chart, code-built and text-lowered,"
    " is structurally identical") {
  // The code path: build, validate, walk. No text involved.
  Chart built;
  gate_build(GATE, built);
  std::vector<Diagnostic> diags;
  REQUIRE_MESSAGE(validate_chart(built, diags),
                  diag_message(diags.empty() ? DiagCode::Ok : diags[0].code));

  uint32_t const live{ [&] {
    uint32_t n{ 0 };
    for (State const &s : built.states) {
      if (s.live != 0) { ++n; }
    }
    return n;
  }() };
  CHECK(live >= 2000);

  // Walk: the deepest leaf's address is 17 segments -- 16 composites down
  // plus the leaf -- and resolves back to the row that printed it. Composites
  // at side levels hold two submachines, so their segments carry the `:0`
  // qualifier chart_path_of prints for an unnamed implicit submachine.
  std::string const deep_path{ [&] {
    std::string p{ "T0" };
    if (level_has_side(0)) { p += ":0"; }
    for (uint32_t level = 1; level < GATE.depth; ++level) {
      p += "/S" + std::to_string(level);
      if (level_has_side(level)) { p += ":0"; }
    }
    return p + "/A0";
  }() };
  StateId deep{ INVALID };
  REQUIRE(resolve_path(built, built.root_submachine, deep_path, deep) ==
          ResolveStatus::Ok);
  CHECK(path(built, deep) == deep_path);

  // The text path: parse and lower the same chart.
  std::string const text{ gate_text(GATE) };
  Parsed const p{ parse(text, "gate.scav") };
  REQUIRE_MESSAGE(p.ok, diag_message(first_code(p.diags)));
  Chart lowered;
  std::vector<Diagnostic> lower_diags;
  REQUIRE(lower_document(lowered, p.pd, lower_diags));
  REQUIRE(validate_chart(lowered, diags));

  check_same_chart(built, lowered);

  // Columns register against either chart alike -- the extension boundary
  // works on a lowered chart exactly as on a built one.
  ColumnId const col{
    column_register(lowered, "gate.marks", ElemKind::State, ValueKind::U32, 4, 4, 0)
  };
  REQUIRE(col.v != INVALID);
  CHECK(column_count(lowered, col) == lowered.states.size());
}
