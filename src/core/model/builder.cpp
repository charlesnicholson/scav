// The append-only builder. Rows are never removed or reordered; the one moving
// part is the shared arrays spans index (state_ids, submachine_ids, attrs),
// which are rebuilt in O(n) when an append lands inside them rather than at
// the tail. That trade is deliberate: state_ids at the 2k-state target is
// ~8KB, so a rebuild per edit is microseconds, and an indirection that
// survives mutation would cost every read forever to save it.

#include "core/model/model_internal.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

namespace {

uint32_t size32(size_t n) { return narrow_clamp<uint32_t>(n); }

bool submachine_live(Chart const &c, SubmachineId id) {
  return (id.v < c.submachines.size()) && (c.submachines[id.v].live != 0);
}

bool state_live(Chart const &c, StateId id) {
  return (id.v < c.states.size()) && (c.states[id.v].live != 0);
}

// Grows `span` by one, inserting `value` at its end and keeping every other
// span into the same array valid. Spans are disjoint and empty spans carry no
// meaningful offset, so the fix-up is one rule: a non-empty span starting at
// or past the insertion point shifts right. The target span never matches --
// its own offset is always before the insertion point by construction.
void insert_child(Chart &c, Span &span, StateId value) {
  if (span.len == 0) { span.off = size32(c.state_ids.size()); }
  uint32_t const pos{ span.off + span.len };
  bool const at_tail{ pos == c.state_ids.size() };
  c.state_ids.insert(c.state_ids.begin() + pos, value);
  // A tail append shifts nothing, and it is also the common case -- building
  // in document order is all tail appends -- so the fix-up walk is skipped
  // rather than run to discover there is nothing to do.
  if (!at_tail) {
    for (Submachine &m : c.submachines) {
      if ((m.children.len != 0) && (m.children.off >= pos)) { m.children.off += 1; }
    }
  }
  span.len += 1;
}

void insert_submachine(Chart &c, Span &span, SubmachineId value) {
  if (span.len == 0) { span.off = size32(c.submachine_ids.size()); }
  uint32_t const pos{ span.off + span.len };
  bool const at_tail{ pos == c.submachine_ids.size() };
  c.submachine_ids.insert(c.submachine_ids.begin() + pos, value);
  if (!at_tail) {
    for (State &s : c.states) {
      if ((s.submachines.len != 0) && (s.submachines.off >= pos)) {
        s.submachines.off += 1;
      }
    }
  }
  span.len += 1;
}

// Attrs hang off four owner kinds, so the fix-up walks all four. Returns the
// row's index at the moment of insertion.
uint32_t insert_attr(Chart &c, Span &span, Attr row) {
  if (span.len == 0) { span.off = size32(c.attrs.size()); }
  uint32_t const pos{ span.off + span.len };
  bool const at_tail{ pos == c.attrs.size() };
  c.attrs.insert(c.attrs.begin() + pos, row);
  if (!at_tail) {
    for (State &s : c.states) {
      if ((s.attrs.len != 0) && (s.attrs.off >= pos)) { s.attrs.off += 1; }
    }
    for (Submachine &m : c.submachines) {
      if ((m.attrs.len != 0) && (m.attrs.off >= pos)) { m.attrs.off += 1; }
    }
    for (Transition &t : c.transitions) {
      if ((t.attrs.len != 0) && (t.attrs.off >= pos)) { t.attrs.off += 1; }
    }
    if ((c.chart_attrs.len != 0) && (c.chart_attrs.off >= pos)) { c.chart_attrs.off += 1; }
  }
  span.len += 1;
  return pos;
}

// The span insert_attr grows, or null for a subject that cannot carry attrs.
// The pointer never outlives the call: rows are addressed by ordinal, and this
// is the "algorithms may hold pointers while working" case, not a stored one.
Span *attrs_span_of(Chart &c, ElemRef ref) {
  if (!chart_live(c, ref)) { return nullptr; }
  switch (ref.kind) {
    case ElemKind::State: return &c.states[ref.ordinal].attrs;
    case ElemKind::Submachine: return &c.submachines[ref.ordinal].attrs;
    case ElemKind::Transition: return &c.transitions[ref.ordinal].attrs;
    case ElemKind::Chart: return &c.chart_attrs;
    case ElemKind::Point:
    case ElemKind::PathBox:
    case ElemKind::None:
    default: return nullptr;
  }
}

AttrKeyId attr_key_intern(Chart &c, std::string_view key) {
  AttrKeyId const found{ chart_attr_key_find(c, key) };
  if (found.v != INVALID) { return found; }
  AttrKeyId const id{ size32(c.attr_key_names.size()) };
  c.attr_key_names.push_back(string_pool_add(c.attr_keys, key));
  return id;
}

}  // namespace

StateId model_append_state_row(Chart &c, State const &row) {
  StateId const id{ size32(c.states.size()) };
  c.states.push_back(row);
  columns_append_entity_row(c, ElemKind::State);
  return id;
}

SubmachineId build_chart(Chart &c, std::string_view name, std::string_view label) {
  // One root per chart. A second call would orphan everything under the first,
  // so it is a refused call rather than a replaced root.
  if (!c.submachines.empty()) { return { INVALID }; }
  c.name = string_pool_add(c.strings, name);
  c.label = string_pool_add(c.strings, label);
  SubmachineId const id{ 0 };
  c.submachines.push_back({ .owner = { INVALID },
                            .ordinal = 0,
                            .name = {},
                            .label = {},
                            .children = {},
                            .attrs = {},
                            .stmt = { INVALID },
                            .inst = { INVALID },
                            .live = 1 });
  c.root_submachine = id;
  columns_append_entity_row(c, ElemKind::Submachine);
  return id;
}

StateId build_state(Chart &c,
                    SubmachineId parent,
                    std::string_view name,
                    StateKind kind,
                    std::string_view label) {
  if (!submachine_live(c, parent)) { return { INVALID }; }
  StateId const id{ size32(c.states.size()) };
  c.states.push_back({ .name = string_pool_add(c.strings, name),
                       .label = string_pool_add(c.strings, label),
                       .parent = parent,
                       .kind = kind,
                       .submachines = {},
                       .attrs = {},
                       .stmt = { INVALID },
                       .inst = { INVALID },
                       .live = 1 });
  insert_child(c, c.submachines[parent.v].children, id);
  columns_append_entity_row(c, ElemKind::State);
  return id;
}

SubmachineId build_submachine(Chart &c,
                              StateId owner,
                              std::string_view name,
                              std::string_view label) {
  if (!state_live(c, owner)) { return { INVALID }; }
  SubmachineId const id{ size32(c.submachines.size()) };
  // Ordinal is the position among the owner's submachines, fixed at build:
  // `On:1` must keep meaning the same submachine after later appends.
  uint32_t const ordinal{ c.states[owner.v].submachines.len };
  c.submachines.push_back({ .owner = owner,
                            .ordinal = ordinal,
                            .name = string_pool_add(c.strings, name),
                            .label = string_pool_add(c.strings, label),
                            .children = {},
                            .attrs = {},
                            .stmt = { INVALID },
                            .inst = { INVALID },
                            .live = 1 });
  insert_submachine(c, c.states[owner.v].submachines, id);
  columns_append_entity_row(c, ElemKind::Submachine);
  return id;
}

TransId build_trans(Chart &c,
                    StateId src,
                    StateId dst,
                    TransKind kind,
                    std::string_view label) {
  if (!state_live(c, src) || !state_live(c, dst)) { return { INVALID }; }
  TransId const id{ size32(c.transitions.size()) };
  c.transitions.push_back({ .src = src,
                            .dst = dst,
                            .kind = kind,
                            .label = string_pool_add(c.strings, label),
                            .attrs = {},
                            .stmt = { INVALID },
                            .inst = { INVALID },
                            .live = 1 });
  columns_append_entity_row(c, ElemKind::Transition);
  return id;
}

uint32_t build_attr(Chart &c,
                    ElemRef subject,
                    std::string_view key,
                    std::string_view value) {
  if (key.empty()) { return INVALID; }
  Span *const span{ attrs_span_of(c, subject) };
  if (span == nullptr) { return INVALID; }
  Attr const row{ .key = attr_key_intern(c, key),
                  .value = string_pool_add(c.strings, value) };
  return insert_attr(c, *span, row);
}

InstId build_include(Chart &c, SubmachineId parent, std::string_view alias) {
  // An alias is a state, so it takes a state's rules: a nameless include has
  // no address and no reason to exist.
  if (alias.empty()) { return { INVALID }; }
  StateId const host{ build_state(c, parent, alias, StateKind::Normal, {}) };
  if (host.v == INVALID) { return { INVALID }; }
  InstId const id{ size32(c.includes.size()) };
  c.includes.push_back({ .alias = string_pool_add(c.strings, alias),
                         .target = { INVALID },  // the loader's to fill (P2)
                         .host = host,
                         .stmt = { INVALID } });
  return id;
}

}  // namespace scav
