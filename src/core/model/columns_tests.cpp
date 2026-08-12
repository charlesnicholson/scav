// Extension columns: registration, lockstep growth under entity appends, and
// the rejections that keep a column an identity.

#include "core/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <ostream>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

uint32_t read_u32(Chart const &c, ColumnId id, uint32_t row) {
  uint32_t out{ 0 };
  std::memcpy(&out, column_data(c, id) + (static_cast<size_t>(row) * 4U), 4U);
  return out;
}

void write_u32(Chart &c, ColumnId id, uint32_t row, uint32_t value) {
  std::memcpy(column_data(c, id) + (static_cast<size_t>(row) * 4U), &value, 4U);
}

}  // namespace

TEST_CASE("column: registration sizes to the rows that already exist") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  build_state(c, root, "B", StateKind::Normal, {});
  ColumnId const id{
    column_register(c, "libhsm.events", ElemKind::State, ValueKind::U32, 4, 4, 0)
  };
  REQUIRE(id.v != INVALID);
  CHECK(column_count(c, id) == 2);
  CHECK(read_u32(c, id, 0) == 0);  // zero-filled, not uninitialized
  CHECK(read_u32(c, id, 1) == 0);
}

TEST_CASE("column: entity appends grow every matching column in lockstep") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  ColumnId const per_state{
    column_register(c, "s", ElemKind::State, ValueKind::U32, 4, 4, 0)
  };
  ColumnId const per_sub{
    column_register(c, "m", ElemKind::Submachine, ValueKind::U32, 4, 4, 0)
  };
  ColumnId const per_trans{
    column_register(c, "t", ElemKind::Transition, ValueKind::U32, 4, 4, 0)
  };
  CHECK(column_count(c, per_state) == 0);
  CHECK(column_count(c, per_sub) == 1);  // the root already existed

  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  write_u32(c, per_state, a.v, 0xAAAA);
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_submachine(c, a, "inner", {});
  build_trans(c, a, b, TransKind::External, {});

  CHECK(column_count(c, per_state) == chart_entity_count(c, ElemKind::State));
  CHECK(column_count(c, per_sub) == chart_entity_count(c, ElemKind::Submachine));
  CHECK(column_count(c, per_trans) == chart_entity_count(c, ElemKind::Transition));
  // Growth appended zeroed rows and left written ones alone.
  CHECK(read_u32(c, per_state, a.v) == 0xAAAA);
  CHECK(read_u32(c, per_state, b.v) == 0);
}

TEST_CASE("column: a chart column has one row; a point column manages its own") {
  Chart c;
  build_chart(c, "c", {});
  ColumnId const per_chart{
    column_register(c, "profile", ElemKind::Chart, ValueKind::StrRef, 8, 4, 0)
  };
  ColumnId const per_point{ column_register(c,
                                            "scav.geom.point",
                                            ElemKind::Point,
                                            ValueKind::Pod,
                                            8,
                                            4,
                                            COLUMN_DERIVED) };
  CHECK(column_count(c, per_chart) == 1);
  CHECK(column_count(c, per_point) == 0);
  SubmachineId const root{ c.root_submachine };
  build_state(c, root, "A", StateKind::Normal, {});
  CHECK(column_count(c, per_chart) == 1);  // states do not grow it
  CHECK(column_count(c, per_point) == 0);  // nothing grows it but its writer
}

TEST_CASE("column: find is by name bytes and misses are INVALID") {
  Chart c;
  build_chart(c, "c", {});
  ColumnId const id{
    column_register(c, "scxml.onentry", ElemKind::State, ValueKind::StrRef, 8, 4, 0)
  };
  CHECK(column_find(c, "scxml.onentry") == id);
  CHECK(column_find(c, "scxml.onexit").v == INVALID);
  CHECK(column_find(c, "").v == INVALID);
}

TEST_CASE("column: the derived flag rides the descriptor") {
  Chart c;
  build_chart(c, "c", {});
  ColumnId const geom{ column_register(c,
                                       "scav.geom.state",
                                       ElemKind::State,
                                       ValueKind::Pod,
                                       16,
                                       4,
                                       COLUMN_DERIVED) };
  ColumnId const events{
    column_register(c, "libhsm.events", ElemKind::State, ValueKind::Span, 8, 4, 0)
  };
  CHECK((c.columns[geom.v].desc.flags & COLUMN_DERIVED) != 0);
  CHECK((c.columns[events.v].desc.flags & COLUMN_DERIVED) == 0);
}

TEST_CASE("column: names live in their own pool, apart from authored text") {
  Chart c;
  build_chart(c, "c", {});
  ColumnId const id{
    column_register(c, "libhsm.events", ElemKind::State, ValueKind::U32, 4, 4, 0)
  };
  CHECK(string_pool_view(c.column_names, c.columns[id.v].desc.name) == "libhsm.events");
  // Registering leaked nothing into the authored pool.
  CHECK(c.strings.bytes.size() == 1);  // "c", the chart name
}

TEST_CASE("column: registration rejects what would corrupt the contract") {
  Chart c;
  build_chart(c, "c", {});
  REQUIRE(column_register(c, "ok", ElemKind::State, ValueKind::U32, 4, 4, 0).v != INVALID);
  // A column is an identity: re-registration is a bug, not an upsert.
  CHECK(column_register(c, "ok", ElemKind::State, ValueKind::U32, 4, 4, 0).v == INVALID);
  CHECK(column_register(c, "", ElemKind::State, ValueKind::U32, 4, 4, 0).v == INVALID);
  CHECK(column_register(c, "x", ElemKind::None, ValueKind::U32, 4, 4, 0).v == INVALID);
  CHECK(column_register(c, "x", ElemKind::PathBox, ValueKind::U32, 4, 4, 0).v == INVALID);
  CHECK(column_register(c, "x", ElemKind::State, ValueKind::U32, 0, 4, 0).v == INVALID);
  CHECK(column_register(c, "x", ElemKind::State, ValueKind::U32, 4, 0, 0).v == INVALID);
  // Alignment must divide the stride or row N lands misaligned.
  CHECK(column_register(c, "x", ElemKind::State, ValueKind::U32, 6, 4, 0).v == INVALID);
  CHECK(c.columns.size() == 1);
}

TEST_CASE("column: accessors refuse a bad id rather than reading garbage") {
  Chart c;
  build_chart(c, "c", {});
  CHECK(column_data(c, ColumnId{ 0 }) == nullptr);
  CHECK(column_data(c, ColumnId{ INVALID }) == nullptr);
  CHECK(column_count(c, ColumnId{ 3 }) == 0);
  Chart const &cc{ c };
  CHECK(column_data(cc, ColumnId{ 0 }) == nullptr);
}
