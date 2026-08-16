// Type-erased byte arrays with a stride, indexed by entity ordinal. Stored,
// kept index-aligned, and passed through unread.

#include "core/model/model.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scav {

namespace {

bool column_entity_allowed(ElemKind entity) {
  switch (entity) {
    case ElemKind::State:
    case ElemKind::Submachine:
    case ElemKind::Transition:
    case ElemKind::Chart:
    case ElemKind::Point:  // length is the column's own, not an entity count
      return true;
    case ElemKind::PathBox:
    case ElemKind::None:
    default: return false;
  }
}

}  // namespace

ColumnId column_register(Chart &c,
                         std::string_view name,
                         ElemKind entity,
                         ValueKind kind,
                         uint32_t elem_size,
                         uint32_t elem_align,
                         uint32_t flags) {
  if (name.empty() || !column_entity_allowed(entity)) { return { INVALID }; }
  if ((elem_size == 0) || (elem_align == 0) || ((elem_size % elem_align) != 0)) {
    return { INVALID };
  }
  // Re-registering a name is a caller bug, not an upsert.
  if (column_find(c, name).v != INVALID) { return { INVALID }; }

  ColumnId const id{ narrow_clamp<uint32_t>(c.columns.size()) };
  ColumnDesc const desc{ .name = string_pool_add(c.column_names, name),
                         .entity = entity,
                         .kind = kind,
                         .elem_size = elem_size,
                         .elem_align = elem_align,
                         .flags = flags };
  // Sized to the rows that already exist, so registration order does not change
  // what a column covers.
  uint64_t const count{ chart_entity_count(c, entity) };
  c.columns.push_back(
      { .desc = desc,
        .bytes = std::vector<scav_byte>(narrow_clamp<size_t>(count * elem_size), 0) });
  return id;
}

ColumnId column_find(Chart const &c, std::string_view name) {
  for (uint32_t i = 0; i < c.columns.size(); ++i) {
    if (string_pool_view(c.column_names, c.columns[i].desc.name) == name) { return { i }; }
  }
  return { INVALID };
}

scav_byte *column_data(Chart &c, ColumnId id) {
  if (id.v >= c.columns.size()) { return nullptr; }
  return c.columns[id.v].bytes.data();
}

scav_byte const *column_data(Chart const &c, ColumnId id) {
  if (id.v >= c.columns.size()) { return nullptr; }
  return c.columns[id.v].bytes.data();
}

uint32_t column_count(Chart const &c, ColumnId id) {
  if (id.v >= c.columns.size()) { return 0; }
  Column const &col{ c.columns[id.v] };
  return narrow_clamp<uint32_t>(col.bytes.size() / col.desc.elem_size);
}

void model_append_column_rows(Chart &c, ElemKind entity) {
  for (Column &col : c.columns) {
    if (col.desc.entity == entity) {
      col.bytes.insert(col.bytes.end(), col.desc.elem_size, 0);
    }
  }
}

}  // namespace scav
