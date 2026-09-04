// Structural validation. Every check here is a bounds check, a liveness check,
// or a byte comparison.

#include "scav_stable_sort.h"

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

// `/ : $` are path metacharacters and `@` is the attribute sigil. A name
// holding one could be neither addressed nor reprinted.
bool name_has_metachar(std::string_view name) {
  for (char const ch : name) {
    if ((ch == '/') || (ch == ':') || (ch == '$') || (ch == '@')) { return true; }
  }
  return false;
}

// The three ways a reference can be wrong: required and missing, out of range,
// and pointing at a tombstone.

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

// Absent provenance is normal -- a code-built row has none -- but a present
// ordinal must land in its array.
void check_provenance(Validator &v, ElemRef subject, StmtId stmt, InstId inst) {
  if ((stmt.v != INVALID) && (stmt.v >= v.c->stmts.size())) {
    report(v, DiagCode::DanglingRef, subject);
  }
  if ((inst.v != INVALID) && (inst.v >= v.c->includes.size())) {
    report(v, DiagCode::DanglingRef, subject);
  }
}

// An attrs span must land in the array and every row it yields must name an
// interned key. One report per broken span, not one per bad row.
void check_attrs(Validator &v, ElemRef subject, Span attrs) {
  if (!span_in(attrs, v.c->attrs.size())) {
    report(v, DiagCode::DanglingRef, subject);
    return;
  }
  for (uint32_t i = 0; i < attrs.len; ++i) {
    Attr const &a{ v.c->attrs[attrs.off + i] };
    if ((a.key.v >= v.c->attr_key_names.size()) ||
        ((a.stmt.v != INVALID) && (a.stmt.v >= v.c->stmts.size()))) {
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
        // A tombstoned submachine keeps its slot in the span, so membership
        // is bounds-checked only.
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
    // An include has no ElemKind of its own, so every finding here names its
    // host state -- and no subject at all when the host names no row, rather
    // than an ordinal no reader can follow.
    ElemRef const subject{ (inc.host.v < c.states.size())
                               ? ElemRef{ .kind = ElemKind::State, .ordinal = inc.host.v }
                               : ElemRef{ .kind = ElemKind::None, .ordinal = INVALID } };
    if ((inc.alias.len == 0) || (inc.path.len == 0)) {
      report(v, DiagCode::MissingRequiredId, subject);
    }
    check_state_ref(v, subject, inc.host, true);
    // An unresolved target is normal. A resolved one must land in documents,
    // and its host must hold the submachine the attachment created.
    if (inc.target.v != INVALID) {
      if (inc.target.v >= c.documents.size()) {
        report(v, DiagCode::DanglingRef, subject);
      } else if ((inc.host.v < c.states.size()) &&
                 (c.states[inc.host.v].submachines.len == 0)) {
        report(v, DiagCode::MissingRequiredId, subject);
      }
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
  // An empty Chart{} fails this too: it has no valid root.
  check_submachine_ref(v, subject, c.root_submachine, true);
}

// Duplicate authored names within one submachine, alias hosts included. Sorted
// by name bytes with the ordinal as the stable tail, so report order is fixed.
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
    scav_stable_sort(named, [](Named const &a, Named const &b) {
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

// More than one initial per submachine. Reachable because each wildcard source
// synthesizes its own pseudostate rather than sharing one.
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

// Containment is stored twice -- as a back-pointer and as a span -- and this
// cross-checks the two. A link the sides disagree about, or a climb that never
// reaches a document root, is one finding on the row it was seen from.
//
// A link already reported as dangling, missing or tombstoned is skipped
// throughout, so a single broken ordinal yields a single finding.
void check_containment(Validator &v) {
  Chart const &c{ *v.c };

  // Occurrences of each row in the container its own back-pointer names. No
  // other container contributes, so the count answers "exactly once" directly.
  // Dead containers count too: a live row may sit inside a tombstoned one.
  std::vector<uint32_t> in_parent(c.states.size(), 0);
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    Span const kids{ c.submachines[i].children };
    if (!span_in(kids, c.state_ids.size())) { continue; }
    for (uint32_t k = 0; k < kids.len; ++k) {
      uint32_t const id{ c.state_ids[kids.off + k].v };
      if ((id < c.states.size()) && (c.states[id].parent.v == i)) { in_parent[id] += 1; }
    }
  }
  std::vector<uint32_t> in_owner(c.submachines.size(), 0);
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    Span const subs{ c.states[i].submachines };
    if (!span_in(subs, c.submachine_ids.size())) { continue; }
    for (uint32_t k = 0; k < subs.len; ++k) {
      uint32_t const id{ c.submachine_ids[subs.off + k].v };
      if ((id < c.submachines.size()) && (c.submachines[id].owner.v == i)) {
        in_owner[id] += 1;
      }
    }
  }

  // state -> parent -> owner -> ... , coloured so the whole forest costs one
  // pass: 0 unvisited, 1 on the current climb, 2 settled reaching, 3 settled
  // cyclic. Meeting a 1 is the cycle; a climb that ends anywhere else settles
  // to what it ended on.
  constexpr uint8_t UNVISITED{ 0 };
  constexpr uint8_t CLIMBING{ 1 };
  constexpr uint8_t REACHES{ 2 };
  constexpr uint8_t CYCLIC{ 3 };
  std::vector<uint8_t> mark(c.states.size(), UNVISITED);
  std::vector<uint32_t> climb;
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (mark[i] != UNVISITED) { continue; }
    climb.clear();
    uint32_t cur{ i };
    uint8_t settled{ REACHES };
    for (;;) {
      if (mark[cur] == CLIMBING) {
        settled = CYCLIC;
        break;
      }
      if (mark[cur] != UNVISITED) {
        settled = mark[cur];
        break;
      }
      mark[cur] = CLIMBING;
      climb.push_back(cur);
      SubmachineId const sm{ c.states[cur].parent };
      if (sm.v >= c.submachines.size()) { break; }
      StateId const owner{ c.submachines[sm.v].owner };
      // INVALID lands here too: a document root ends the climb having reached
      // one.
      if (owner.v >= c.states.size()) { break; }
      cur = owner.v;
    }
    for (uint32_t const step : climb) { mark[step] = settled; }
  }

  for (uint32_t i = 0; i < c.states.size(); ++i) {
    State const &s{ c.states[i] };
    if (s.live == 0) { continue; }
    bool bad{ mark[i] == CYCLIC };
    if (!bad && (s.parent.v < c.submachines.size()) &&
        span_in(c.submachines[s.parent.v].children, c.state_ids.size())) {
      bad = in_parent[i] != 1;
    }
    if (!bad && span_in(s.submachines, c.submachine_ids.size())) {
      for (uint32_t k = 0; k < s.submachines.len; ++k) {
        uint32_t const id{ c.submachine_ids[s.submachines.off + k].v };
        if ((id >= c.submachines.size()) || (c.submachines[id].live == 0)) { continue; }
        uint32_t const owner{ c.submachines[id].owner.v };
        if ((owner != INVALID) && (owner >= c.states.size())) { continue; }
        if (owner != i) {
          bad = true;
          break;
        }
      }
    }
    if (bad) {
      report(v,
             DiagCode::ContainmentInconsistent,
             ElemRef{ .kind = ElemKind::State, .ordinal = i });
    }
  }

  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    Submachine const &m{ c.submachines[i] };
    if (m.live == 0) { continue; }
    // One ownerless submachine is legitimate, and it is the one the chart row
    // names; a second is a document root nothing addresses.
    bool bad{ false };
    if (m.owner.v == INVALID) {
      bad = SubmachineId{ i } != c.root_submachine;
    } else if ((m.owner.v < c.states.size()) &&
               span_in(c.states[m.owner.v].submachines, c.submachine_ids.size())) {
      bad = in_owner[i] != 1;
    }
    if (!bad && span_in(m.children, c.state_ids.size())) {
      for (uint32_t k = 0; k < m.children.len; ++k) {
        uint32_t const id{ c.state_ids[m.children.off + k].v };
        if ((id >= c.states.size()) || (c.states[id].live == 0)) { continue; }
        uint32_t const parent{ c.states[id].parent.v };
        if (parent >= c.submachines.size()) { continue; }
        if (parent != i) {
          bad = true;
          break;
        }
      }
    }
    if (bad) {
      report(v,
             DiagCode::ContainmentInconsistent,
             ElemRef{ .kind = ElemKind::Submachine, .ordinal = i });
    }
  }
}

// Every statement's src must land inside its own document's text. Vacuous on a
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

// Columns must cover their entity array exactly. Columns have no ElemKind of
// their own, so the subject is the array the column failed to cover, with the
// INVALID ordinal that names a kind rather than a row.
void check_columns(Validator &v) {
  Chart const &c{ *v.c };
  for (uint32_t i = 0; i < c.columns.size(); ++i) {
    ColumnDesc const &desc{ c.columns[i].desc };
    if ((desc.entity == ElemKind::Point) || (desc.entity == ElemKind::PathBox) ||
        (desc.entity == ElemKind::None)) {
      continue;  // a point column's length is its own
    }
    if (column_count(c, ColumnId{ i }) != chart_entity_count(c, desc.entity)) {
      report(v,
             DiagCode::ColumnCountMismatch,
             ElemRef{ .kind = desc.entity, .ordinal = INVALID });
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
  check_containment(v);
  check_statements(v);
  check_columns(v);

  // A total order over the triple. Equal triples are one finding repeated, and
  // stability keeps them in walk order.
  scav_stable_sort(v.found, [](Diagnostic const &a, Diagnostic const &b) {
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
