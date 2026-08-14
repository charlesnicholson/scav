// Structural validation (§10). Everything here is a bounds check, a liveness
// check, or a byte comparison -- semantics belong to plugins, and layout is
// entitled to crash on a model that was never validated.

#include "scav_sort.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

namespace {

struct Validator {
  Chart const *c;
  std::vector<Diagnostic> found;
};

void report(Validator &v, DiagCode code, ElemRef subject) {
  v.found.push_back({ .code = code, .subject = subject, .doc = { INVALID }, .src = {} });
}

bool span_in(Span s, size_t array_len) {
  return (static_cast<uint64_t>(s.off) + s.len) <= array_len;
}

// `/ : $` are path metacharacters and `@` is the format's attribute sigil; a
// name holding one could never be addressed or reprinted.
bool name_has_metachar(std::string_view name) {
  for (char const ch : name) {
    if ((ch == '/') || (ch == ':') || (ch == '$') || (ch == '@')) { return true; }
  }
  return false;
}

// The three reference shapes, factored so every caller reports the same way:
// required-and-missing, out of range, and pointing at a tombstone.

void check_state_ref(Validator &v, ElemRef subject, StateId id, bool required) {
  if (id.v == INVALID) {
    if (required) { report(v, DiagCode::MissingRequiredId, subject); }
    return;
  }
  if (id.v >= v.c->states.size()) {
    report(v, DiagCode::DanglingRef, subject);
    return;
  }
  if (v.c->states[id.v].live == 0) { report(v, DiagCode::TombstonedTarget, subject); }
}

void check_submachine_ref(Validator &v, ElemRef subject, SubmachineId id, bool required) {
  if (id.v == INVALID) {
    if (required) { report(v, DiagCode::MissingRequiredId, subject); }
    return;
  }
  if (id.v >= v.c->submachines.size()) {
    report(v, DiagCode::DanglingRef, subject);
    return;
  }
  if (v.c->submachines[id.v].live == 0) { report(v, DiagCode::TombstonedTarget, subject); }
}

// stmt and inst are provenance: absent is normal (code-built rows), but a
// present ordinal must land in its array.
void check_provenance(Validator &v, ElemRef subject, StmtId stmt, InstId inst) {
  if ((stmt.v != INVALID) && (stmt.v >= v.c->stmts.size())) {
    report(v, DiagCode::DanglingRef, subject);
  }
  if ((inst.v != INVALID) && (inst.v >= v.c->includes.size())) {
    report(v, DiagCode::DanglingRef, subject);
  }
}

// An attrs span must land in the array, and every row it yields must name an
// interned key. One report per broken span, not per row past the end.
void check_attrs(Validator &v, ElemRef subject, Span attrs) {
  if (!span_in(attrs, v.c->attrs.size())) {
    report(v, DiagCode::DanglingRef, subject);
    return;
  }
  for (uint32_t i = 0; i < attrs.len; ++i) {
    if (v.c->attrs[attrs.off + i].key.v >= v.c->attr_key_names.size()) {
      report(v, DiagCode::DanglingRef, subject);
      return;
    }
  }
}

void check_name(Validator &v, ElemRef subject, StrRef name) {
  if (name_has_metachar(string_pool_view(v.c->strings, name))) {
    report(v, DiagCode::NameHasMetacharacter, subject);
  }
}

void check_states(Validator &v) {
  Chart const &c{ *v.c };
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    State const &s{ c.states[i] };
    if (s.live == 0) { continue; }
    ElemRef const subject{ .kind = ElemKind::State, .ordinal = i };
    check_submachine_ref(v, subject, s.parent, true);
    check_name(v, subject, s.name);
    if (!span_in(s.submachines, c.submachine_ids.size())) {
      report(v, DiagCode::DanglingRef, subject);
    } else {
      for (uint32_t k = 0; k < s.submachines.len; ++k) {
        // A tombstoned submachine keeps its slot in the span by design
        // (§7.3), so membership is bounds-checked only.
        if (c.submachine_ids[s.submachines.off + k].v >= c.submachines.size()) {
          report(v, DiagCode::DanglingRef, subject);
          break;
        }
      }
    }
    check_attrs(v, subject, s.attrs);
    check_provenance(v, subject, s.stmt, s.inst);
  }
}

void check_submachines(Validator &v) {
  Chart const &c{ *v.c };
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    Submachine const &m{ c.submachines[i] };
    if (m.live == 0) { continue; }
    ElemRef const subject{ .kind = ElemKind::Submachine, .ordinal = i };
    check_state_ref(v, subject, m.owner, false);  // INVALID = a document root
    check_name(v, subject, m.name);
    if (!span_in(m.children, c.state_ids.size())) {
      report(v, DiagCode::DanglingRef, subject);
    } else {
      for (uint32_t k = 0; k < m.children.len; ++k) {
        if (c.state_ids[m.children.off + k].v >= c.states.size()) {
          report(v, DiagCode::DanglingRef, subject);
          break;
        }
      }
    }
    check_attrs(v, subject, m.attrs);
    check_provenance(v, subject, m.stmt, m.inst);
  }
}

void check_transitions(Validator &v) {
  Chart const &c{ *v.c };
  for (uint32_t i = 0; i < c.transitions.size(); ++i) {
    Transition const &t{ c.transitions[i] };
    if (t.live == 0) { continue; }
    ElemRef const subject{ .kind = ElemKind::Transition, .ordinal = i };
    check_state_ref(v, subject, t.src, true);
    check_state_ref(v, subject, t.dst, true);
    check_attrs(v, subject, t.attrs);
    check_provenance(v, subject, t.stmt, t.inst);
  }
}

void check_includes(Validator &v) {
  Chart const &c{ *v.c };
  for (uint32_t i = 0; i < c.includes.size(); ++i) {
    Include const &inc{ c.includes[i] };
    // The subject is the host state -- an include has no ElemKind because it
    // is not drawable; its host is where a reader will look.
    ElemRef const subject{ .kind = ElemKind::State, .ordinal = inc.host.v };
    if (inc.alias.len == 0) {
      report(v,
             DiagCode::MissingRequiredId,
             ElemRef{ .kind = ElemKind::State, .ordinal = INVALID });
    }
    check_state_ref(v,
                    ElemRef{ .kind = ElemKind::State, .ordinal = inc.host.v },
                    inc.host,
                    true);
    // Unresolved is P1-normal; a resolved target must land in documents.
    if ((inc.target.v != INVALID) && (inc.target.v >= c.documents.size())) {
      report(v, DiagCode::DanglingRef, subject);
    }
    if ((inc.stmt.v != INVALID) && (inc.stmt.v >= c.stmts.size())) {
      report(v, DiagCode::DanglingRef, subject);
    }
  }
}

void check_chart_row(Validator &v) {
  Chart const &c{ *v.c };
  ElemRef const subject{ .kind = ElemKind::Chart, .ordinal = 0 };
  check_name(v, subject, c.name);
  check_attrs(v, subject, c.chart_attrs);
  // A chart with any structure needs its root; an empty Chart{} is still a
  // model with no valid root, and saying so beats special-casing empty.
  check_submachine_ref(v, subject, c.root_submachine, true);
}

// Duplicate authored names within one submachine (§10). An alias host is an
// ordinary state, so alias collisions fall out of the same walk. Sorted by
// name bytes with the ordinal as the stable tail (§6), so the report order is
// input-derived, not scan-order.
void check_duplicate_names(Validator &v) {
  Chart const &c{ *v.c };
  struct Named {
    std::string_view name;
    uint32_t ordinal;
  };
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    Submachine const &m{ c.submachines[i] };
    if (m.live == 0) { continue; }
    if (!span_in(m.children, c.state_ids.size())) { continue; }  // reported above
    std::vector<Named> named;
    for (uint32_t k = 0; k < m.children.len; ++k) {
      StateId const id{ c.state_ids[m.children.off + k] };
      if (id.v >= c.states.size()) { continue; }  // reported above
      State const &s{ c.states[id.v] };
      if ((s.live == 0) || (s.name.len == 0)) { continue; }
      named.push_back({ .name = string_pool_view(c.strings, s.name), .ordinal = id.v });
    }
    stable_sort_by(named, [](Named const &a, Named const &b) {
      if (a.name != b.name) { return a.name < b.name; }
      return a.ordinal < b.ordinal;
    });
    for (uint32_t k = 1; k < named.size(); ++k) {
      if (named[k].name == named[k - 1].name) {
        report(v,
               DiagCode::DuplicateName,
               ElemRef{ .kind = ElemKind::State, .ordinal = named[k].ordinal });
      }
    }
  }
}

// More than one initial per submachine (§10). Each wildcard source synthesizes
// its own pseudostate (§9), which is what makes this reachable.
void check_multiple_initial(Validator &v) {
  Chart const &c{ *v.c };
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    Submachine const &m{ c.submachines[i] };
    if (m.live == 0) { continue; }
    if (!span_in(m.children, c.state_ids.size())) { continue; }
    uint32_t initials{ 0 };
    for (uint32_t k = 0; k < m.children.len; ++k) {
      StateId const id{ c.state_ids[m.children.off + k] };
      if (id.v >= c.states.size()) { continue; }
      State const &s{ c.states[id.v] };
      if ((s.live == 0) || (s.kind != StateKind::Initial)) { continue; }
      ++initials;
      if (initials > 1) {
        report(v,
               DiagCode::MultipleInitial,
               ElemRef{ .kind = ElemKind::State, .ordinal = id.v });
      }
    }
  }
}

// Statement spans against their documents (§10): every statement's src must
// land inside its document's text. Populated by lowering; vacuous on a
// code-built chart, which owns no statements.
void check_statements(Validator &v) {
  Chart const &c{ *v.c };
  ElemRef const none{ .kind = ElemKind::None, .ordinal = INVALID };
  for (uint32_t d = 0; d < c.documents.size(); ++d) {
    Document const &doc{ c.documents[d] };
    if (!span_in(doc.text, c.src_bytes.size()) ||
        !span_in(doc.statements, c.stmts.size())) {
      v.found.push_back(
          { .code = DiagCode::DanglingRef, .subject = none, .doc = { d }, .src = {} });
      continue;
    }
    for (uint32_t k = 0; k < doc.statements.len; ++k) {
      Statement const &stmt{ c.stmts[doc.statements.off + k] };
      uint64_t const lo{ stmt.src.off };
      uint64_t const hi{ lo + stmt.src.len };
      if ((lo < doc.text.off) ||
          (hi > (static_cast<uint64_t>(doc.text.off) + doc.text.len))) {
        v.found.push_back({ .code = DiagCode::StatementSpanOutOfRange,
                            .subject = none,
                            .doc = { d },
                            .src = stmt.src });
      }
      if (!span_in(stmt.comments, c.comments.size())) {
        v.found.push_back({ .code = DiagCode::DanglingRef,
                            .subject = none,
                            .doc = { d },
                            .src = stmt.src });
      }
    }
  }
}

// Columns must cover their entity array exactly -- the lockstep the builder
// maintains and hand-poking can break. The subject ordinal is the ColumnId;
// columns have no ElemKind because they are not drawable entities.
void check_columns(Validator &v) {
  Chart const &c{ *v.c };
  for (uint32_t i = 0; i < c.columns.size(); ++i) {
    ColumnDesc const &desc{ c.columns[i].desc };
    if ((desc.entity == ElemKind::Point) || (desc.entity == ElemKind::PathBox) ||
        (desc.entity == ElemKind::None)) {
      continue;  // a point column's length is its own business (§11.7a)
    }
    if (column_count(c, ColumnId{ i }) != chart_entity_count(c, desc.entity)) {
      report(v,
             DiagCode::ColumnCountMismatch,
             ElemRef{ .kind = ElemKind::None, .ordinal = i });
    }
  }
}

}  // namespace

bool validate_chart(Chart const &c, std::vector<Diagnostic> &diags) {
  Validator v{ .c = &c, .found = {} };
  check_chart_row(v);
  check_states(v);
  check_submachines(v);
  check_transitions(v);
  check_includes(v);
  check_duplicate_names(v);
  check_multiple_initial(v);
  check_statements(v);
  check_columns(v);

  // The §6 artifact order. The comparator is a total order over the triple;
  // equal triples are one finding repeated, and stability keeps their
  // walk-order, which is array order.
  stable_sort_by(v.found, [](Diagnostic const &a, Diagnostic const &b) {
    if (a.code != b.code) {
      return static_cast<uint32_t>(a.code) < static_cast<uint32_t>(b.code);
    }
    if (a.subject.kind != b.subject.kind) {
      return static_cast<uint32_t>(a.subject.kind) < static_cast<uint32_t>(b.subject.kind);
    }
    return a.subject.ordinal < b.subject.ordinal;
  });

  bool const clean{ v.found.empty() };
  diags.insert(diags.end(), v.found.begin(), v.found.end());
  return clean;
}

}  // namespace scav
