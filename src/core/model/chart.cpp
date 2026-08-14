// Reads over the model, and the string-pool append everything else builds on.
// Nothing here mutates a chart.

#include "core/model/model.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

namespace {

template <typename T>
uint64_t bytes_of(std::vector<T> const &v) {
  return static_cast<uint64_t>(v.capacity()) * sizeof(T);
}

// Position of `id` among its parent's unnamed same-kind siblings, counting
// every row: liveness-independent, so tombstoning one sibling never renames
// another.
uint32_t synthetic_ordinal(Chart const &c, StateId id) {
  State const &s{ c.states[id.v] };
  Span const kids{ c.submachines[s.parent.v].children };
  uint32_t n{ 0 };
  for (uint32_t i = 0; i < kids.len; ++i) {
    StateId const sib{ c.state_ids[kids.off + i] };
    if (sib == id) { break; }
    State const &sr{ c.states[sib.v] };
    if ((sr.name.len == 0) && (sr.kind == s.kind)) { ++n; }
  }
  return n;
}

}  // namespace

// One path segment: the authored name, or `$<kind>` with an ordinal suffix
// past the first. The grammar's ident admits neither `$` nor a leading digit,
// so a synthetic name can never collide with an authored one.
void model_state_segment(Chart const &c, StateId id, std::string &out) {
  State const &s{ c.states[id.v] };
  if (s.name.len != 0) {
    out += chart_string(c, s.name);
    return;
  }
  out += '$';
  out += syntax_state_kind_name(s.kind);
  uint32_t const ordinal{ synthetic_ordinal(c, id) };
  if (ordinal != 0) { out += std::to_string(ordinal); }
}

StrRef string_pool_add(StringPool &pool, std::string_view text) {
  if (text.empty()) { return {}; }
  StrRef const ref{ str_ref(narrow_clamp<uint32_t>(pool.bytes.size()),
                            narrow_clamp<uint32_t>(text.size())) };
  pool.bytes.insert(pool.bytes.end(), text.begin(), text.end());
  return ref;
}

std::string_view chart_attr_key(Chart const &c, AttrKeyId key) {
  if (key.v >= c.attr_key_names.size()) { return {}; }
  return string_pool_view(c.attr_keys, c.attr_key_names[key.v]);
}

AttrKeyId chart_attr_key_find(Chart const &c, std::string_view key) {
  // Linear over the interned keys: a chart holds few distinct keys, and the
  // sorted index this could use is derived-scratch that P1 has no caller for.
  for (uint32_t i = 0; i < c.attr_key_names.size(); ++i) {
    if (string_pool_view(c.attr_keys, c.attr_key_names[i]) == key) { return { i }; }
  }
  return { INVALID };
}

uint32_t chart_entity_count(Chart const &c, ElemKind kind) {
  switch (kind) {
    case ElemKind::State: return narrow_clamp<uint32_t>(c.states.size());
    case ElemKind::Submachine: return narrow_clamp<uint32_t>(c.submachines.size());
    case ElemKind::Transition: return narrow_clamp<uint32_t>(c.transitions.size());
    case ElemKind::Chart: return 1;
    case ElemKind::Point:
    case ElemKind::PathBox:
    case ElemKind::None:
    default: return 0;
  }
}

bool chart_ref_valid(Chart const &c, ElemRef ref) {
  switch (ref.kind) {
    case ElemKind::State: return ref.ordinal < c.states.size();
    case ElemKind::Submachine: return ref.ordinal < c.submachines.size();
    case ElemKind::Transition: return ref.ordinal < c.transitions.size();
    case ElemKind::Chart: return ref.ordinal == 0;
    case ElemKind::Point:
    case ElemKind::PathBox:
    case ElemKind::None:
    default: return false;
  }
}

bool chart_live(Chart const &c, ElemRef ref) {
  if (!chart_ref_valid(c, ref)) { return false; }
  switch (ref.kind) {
    case ElemKind::State: return c.states[ref.ordinal].live != 0;
    case ElemKind::Submachine: return c.submachines[ref.ordinal].live != 0;
    case ElemKind::Transition: return c.transitions[ref.ordinal].live != 0;
    case ElemKind::Chart: return true;  // the chart entity has no tombstone
    case ElemKind::Point:
    case ElemKind::PathBox:
    case ElemKind::None:
    default: return false;
  }
}

Span chart_attrs_of(Chart const &c, ElemRef ref) {
  if (!chart_ref_valid(c, ref)) { return {}; }
  switch (ref.kind) {
    case ElemKind::State: return c.states[ref.ordinal].attrs;
    case ElemKind::Submachine: return c.submachines[ref.ordinal].attrs;
    case ElemKind::Transition: return c.transitions[ref.ordinal].attrs;
    case ElemKind::Chart: return c.chart_attrs;
    case ElemKind::Point:
    case ElemKind::PathBox:
    case ElemKind::None:
    default: return {};
  }
}

uint32_t chart_attr_find(Chart const &c, ElemRef subject, std::string_view key) {
  if (!chart_live(c, subject)) { return INVALID; }
  Span const span{ chart_attrs_of(c, subject) };
  for (uint32_t i = 0; i < span.len; ++i) {
    uint32_t const at{ span.off + i };
    if (chart_attr_key(c, c.attrs[at].key) == key) { return at; }
  }
  return INVALID;
}

void chart_path_of(Chart const &c, StateId id, std::string &out) {
  if (id.v >= c.states.size()) { return; }

  // Collect the ancestor chain leaf-first. Under the append-only builder an
  // owner's id is always smaller than its descendants', so the climb
  // terminates; the guard turns a hand-corrupted model into a truncated path
  // rather than a hang.
  std::vector<StateId> chain;
  chain.reserve(17);  // the depth-16 design target plus the leaf; deeper is legal
  StateId cur{ id };
  for (size_t guard = 0; guard <= c.states.size(); ++guard) {
    chain.push_back(cur);
    SubmachineId const sm{ c.states[cur.v].parent };
    if (sm.v >= c.submachines.size()) { break; }
    StateId const owner{ c.submachines[sm.v].owner };
    if ((owner.v == INVALID) || (owner.v >= c.states.size())) { break; }
    cur = owner;
  }

  for (size_t i = chain.size(); i-- > 0;) {
    StateId const step{ chain[i] };
    model_state_segment(c, step, out);
    if (i == 0) { continue; }
    // The qualifier names which of `step`'s submachines the next segment
    // descends into, and only ambiguity earns one: `On:main/Idle` when On has
    // two submachines, `On/Idle` when it has one. Row count, not live count,
    // so a tombstoned sibling submachine never changes an address.
    StateId const child{ chain[i - 1] };
    SubmachineId const sm{ c.states[child.v].parent };
    if (c.states[step.v].submachines.len > 1) {
      Submachine const &m{ c.submachines[sm.v] };
      out += ':';
      if (m.name.len != 0) {
        out += chart_string(c, m.name);
      } else {
        out += std::to_string(m.ordinal);
      }
    }
    out += '/';
  }
}

uint64_t chart_footprint(Chart const &c) {
  uint64_t total{ bytes_of(c.documents) + bytes_of(c.stmts) + bytes_of(c.comments) +
                  bytes_of(c.src_bytes) + bytes_of(c.states) + bytes_of(c.submachines) +
                  bytes_of(c.transitions) + bytes_of(c.includes) + bytes_of(c.attrs) +
                  bytes_of(c.attr_key_names) + bytes_of(c.attr_keys.bytes) +
                  bytes_of(c.columns) + bytes_of(c.column_names.bytes) +
                  bytes_of(c.state_ids) + bytes_of(c.submachine_ids) +
                  bytes_of(c.strings.bytes) };
  for (Column const &col : c.columns) { total += bytes_of(col.bytes); }
  return total;
}

}  // namespace scav
