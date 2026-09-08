// The four phases in a line, the portfolio of phase-2 tuples wrapped around
// the last two of them, then the geometry columns as the only output.
// Everything else here is the columns and the two hashes over them.

#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/router.h"
#include "layout/shard.h"
#include "layout/size.h"
#include "layout/wire.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_xxhash.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace scav {

SCAV_INTERNAL_BEGIN
// The inflation loop's decision and the portfolio's three pure parts,
// bracketed so a test reaches cases no chart does. The prototypes a test uses
// are its own; see scav_internal.h.
bool inflation_done(uint32_t fewest, uint32_t degraded, uint32_t unreachable, bool &keep);
uint32_t search_tuple_count(scav_profile const &p, uint32_t entity_count);
void search_tuple(scav_profile &p, DarSource &dar, uint32_t index);
uint32_t search_argmin(std::vector<Cost> const &cost, std::vector<uint8_t> const &viable);
SCAV_INTERNAL_END

namespace {

constexpr uint32_t RECT{ sizeof(scav_rect) };

// The chart-global phase-2 tuples the portfolio chooses between: two packer
// knobs and where a frame's desired ratio comes from, so eight rows (11.10).
constexpr uint32_t SEARCH_TUPLES{ 8 };

// One name and the shape it is registered under. `write_rows` copies a row per
// entity through whichever column carries the name, so the shape has to be the
// one layout would have registered or the copy runs past the column's bytes.
struct GeomShape {
  char const *name;
  ElemKind entity;
  ValueKind kind;
  uint32_t elem_size;
};

// Index into GEOM, so the writer below and the check in `layout_run` name the
// same row rather than repeating its fields.
enum GeomColumnIndex : uint32_t {
  GeomState,
  GeomBefore,
  GeomAfter,
  GeomSub,
  GeomRoute,
  GeomPort,
  GeomPoint,
  GeomPortSlot,
  GeomChart,
  GeomInputs,
  GeomGen,
  GeomCount,
};

constexpr std::array<GeomShape, GeomCount> GEOM{ {
    { .name = "scav.geom.state",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.state_before",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.state_after",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.sub",
      .entity = ElemKind::Submachine,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.route",
      .entity = ElemKind::Transition,
      .kind = ValueKind::Span,
      .elem_size = 8 },
    { .name = "scav.geom.port",
      .entity = ElemKind::Transition,
      .kind = ValueKind::Span,
      .elem_size = 8 },
    { .name = "scav.geom.point",
      .entity = ElemKind::Point,
      .kind = ValueKind::Pod,
      .elem_size = 8 },
    { .name = "scav.geom.portslot",
      .entity = ElemKind::Point,
      .kind = ValueKind::Pod,
      .elem_size = sizeof(scav_port_slot) },
    { .name = "scav.geom.chart",
      .entity = ElemKind::Chart,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.inputs",
      .entity = ElemKind::Chart,
      .kind = ValueKind::U32,
      .elem_size = 4 },
    { .name = "scav.geom.gen",
      .entity = ElemKind::Chart,
      .kind = ValueKind::U32,
      .elem_size = 4 },
} };

// Registered on first use, found thereafter; every run overwrites in place. The
// found column's shape was checked before any geometry was computed.
ColumnId geom_column(Chart &c, GeomShape const &g) {
  ColumnId const found{ column_find(c, g.name) };
  if (found.v != INVALID) { return found; }
  return column_register(c, g.name, g.entity, g.kind, g.elem_size, 4, COLUMN_DERIVED);
}

// The first name already registered under another entity, value kind, or
// element size. GeomCount when every one of them is layout's own to write.
uint32_t geom_column_clash(Chart const &c) {
  for (uint32_t i = 0; i < GeomCount; ++i) {
    ColumnId const found{ column_find(c, GEOM[i].name) };
    if (found.v == INVALID) { continue; }
    ColumnDesc const &d{ c.columns[found.v].desc };
    if ((d.entity != GEOM[i].entity) || (d.kind != GEOM[i].kind) ||
        (d.elem_size != GEOM[i].elem_size)) {
      return i;
    }
  }
  return GeomCount;
}

template <typename T>
void write_rows(Chart &c, ColumnId id, std::vector<T> const &rows) {
  if (!rows.empty()) {
    std::memcpy(column_data(c, id), rows.data(), rows.size() * sizeof(T));
  }
}

static_assert(sizeof(scav_profile) == 48 * sizeof(int32_t),
              "the profile must stay a flat block of int32 with no padding, or the "
              "inputs digest below would hash bytes whose values are unspecified");

// Every non-geometry input a golden depends on. Without it a hash names the
// numbers that came out and not the run that produced them.
uint32_t inputs_digest(scav_spaces const &s, scav_layout_opts const &o) {
  std::vector<scav_byte> b;
  // Padding is what forbids hashing a struct's bytes, and the assert above
  // proves there is none, so the copy reads all 48 knobs and nothing else.
  std::array<int32_t, sizeof(scav_profile) / sizeof(int32_t)> profile{};
  std::memcpy(profile.data(), &o.profile, sizeof(scav_profile));
  for (int32_t const field : profile) { append_i32(b, field); }

  scav_byte const *name{ nullptr };
  uint32_t name_len{ 0 };
  uint32_t version{ 0 };
  if (router_name(o.router, name, name_len) && router_version(o.router, version)) {
    append_u32(b, name_len);
    b.insert(b.end(), name, name + name_len);
    append_u32(b, version);
  }
  // The font reaches layout only as the integers it measured, so its identity
  // rides in here rather than as an argument layout would never read.
  append_u32(b, spaces_digest(s));
  return xxhash32(b.data(), b.size(), 0);
}

void write_columns(Chart &c, SizedLayout const &z, Routes const &r, uint32_t inputs) {
  write_rows(c, geom_column(c, GEOM[GeomState]), z.state);
  write_rows(c, geom_column(c, GEOM[GeomBefore]), z.before);
  write_rows(c, geom_column(c, GEOM[GeomAfter]), z.after);
  write_rows(c, geom_column(c, GEOM[GeomSub]), z.sub);
  write_rows(c, geom_column(c, GEOM[GeomRoute]), r.route);
  write_rows(c, geom_column(c, GEOM[GeomPort]), r.port);

  ColumnId const pts{ geom_column(c, GEOM[GeomPoint]) };
  column_resize(c, pts, static_cast<uint32_t>(r.points.size()));
  write_rows(c, pts, r.points);
  ColumnId const slots{ geom_column(c, GEOM[GeomPortSlot]) };
  column_resize(c, slots, static_cast<uint32_t>(r.slots.size()));
  write_rows(c, slots, r.slots);

  ColumnId const chart{ geom_column(c, GEOM[GeomChart]) };
  std::memcpy(column_data(c, chart), &z.chart, sizeof(z.chart));

  ColumnId const in{ geom_column(c, GEOM[GeomInputs]) };
  std::memcpy(column_data(c, in), &inputs, 4);

  ColumnId const gen{ geom_column(c, GEOM[GeomGen]) };
  uint32_t n{ 0 };
  std::memcpy(&n, column_data(c, gen), 4);
  ++n;
  std::memcpy(column_data(c, gen), &n, 4);
}

// Every separation raised by one increment. False when the result leaves the
// range the validator admits.
bool inflate(scav_profile &p, int32_t by) {
  p.rank_sep += by;
  p.node_sep += by;
  p.sub_sep += by;
  return profile_validate(p);
}

// One candidate: its geometry and what it took to reach.
struct Candidate {
  SizedLayout sized;
  Routes routes;
  uint32_t inflations{ 0 };
  bool viable{ false };
};

// Phases 2 and 3 for one tuple, with the spacing-inflation retry exactly as a
// lone run has it. `knobs` is the caller's profile with the tuple applied.
Candidate search_candidate(Chart const &c,
                           SplitGraph const &g,
                           SubmachineOrders const &orders,
                           scav_spaces const &s,
                           scav_profile const &knobs,
                           DarSource dar,
                           Router const &router,
                           uint32_t threads,
                           std::vector<Diagnostic> &diags) {
  Candidate out;
  if (!size_layout(c, g, orders, s, knobs, out.sized, diags, dar)) { return out; }
  out.routes = route_transitions(c, g, orders, out.sized, s, knobs, router, threads);

  // `out` carries the best attempt so far, and `done` is set from that one
  // rather than from whichever attempt was just made.
  scav_profile wider{ knobs };
  uint32_t fewest{ out.routes.degraded() };
  bool done{ out.routes.unreachable == 0 };
  // An increment of zero repeats one attempt to the cap, so it is not one.
  for (int32_t k = 0; !done && (knobs.spacing_inflation_increment > 0) &&
                      (k < knobs.spacing_inflation_cap);
       ++k) {
    if (!inflate(wider, knobs.spacing_inflation_increment)) { break; }
    SizedLayout next_sized;
    std::vector<Diagnostic> spilled;
    if (!size_layout(c, g, orders, s, wider, next_sized, spilled, dar)) { break; }
    Routes next{ route_transitions(c, g, orders, next_sized, s, wider, router, threads) };
    bool keep{ false };
    done = inflation_done(fewest, next.degraded(), next.unreachable, keep);
    if (keep) {
      fewest = next.degraded();
      out.sized = std::move(next_sized);
      out.routes = std::move(next);
      out.inflations = static_cast<uint32_t>(k) + 1;
    }
  }

  // Bounds everything laid out, not just the root submachine: a route bends into
  // a frame's padding and a path box centres on one, so both can reach past it.
  // Before the score rather than after the pick, so `area` and `aspect` price
  // the canvas that ships.
  auto const cover = [&out](int32_t x, int32_t y) {
    scav_rect &chart{ out.sized.chart };
    int32_t const right{ imax(chart.x + chart.w, x) };
    int32_t const bottom{ imax(chart.y + chart.h, y) };
    chart.x = imin(chart.x, x);
    chart.y = imin(chart.y, y);
    chart.w = right - chart.x;
    chart.h = bottom - chart.y;
  };
  for (scav_point const &at : out.routes.points) { cover(at.x, at.y); }
  for (scav_rect const &at : out.routes.placed) {
    cover(at.x, at.y);
    cover(at.x + at.w, at.y + at.h);
  }
  out.viable = true;
  return out;
}

}  // namespace

SCAV_INTERNAL_BEGIN

// Whether the loop is finished, and through `keep` whether this attempt
// replaces the best so far. The case that matters is one no chart reaches: only
// a router answering `outside_region` or `too_large` where it used to answer
// `unreachable` produces an attempt that reaches every end while degrading
// more, and the shipped one does not do that on any chart in the corpus or the
// suite.
bool inflation_done(uint32_t fewest, uint32_t degraded, uint32_t unreachable, bool &keep) {
  keep = degraded < fewest;
  // Only the attempt that is kept can end the loop, because the kept attempt is
  // the geometry that ships. One that reaches every end while degrading more
  // elsewhere is discarded, and stopping on it would leave behind exactly the
  // unreachable ends the retry existed to remove.
  return keep && (unreachable == 0);
}

// How many of the table's rows this chart runs: `portfolio_m`, halved for every
// doubling of the entity count past 512, floored at one row and capped at the
// table. So a chart under 1,024 entities gets the whole of M and either 2k
// shape gets one -- the largest chart is searched least, which is backwards for
// quality and right for latency (11.10). `ilog2` of a `uint32_t` is at most 31,
// so the shift is at most 22.
uint32_t search_tuple_count(scav_profile const &p, uint32_t entity_count) {
  uint32_t const scale{ (entity_count == 0) ? 0U : ilog2(entity_count) };
  uint32_t const shift{ (scale > 9U) ? (scale - 9U) : 0U };
  uint32_t const rows{ static_cast<uint32_t>(imax(p.portfolio_m, 1)) >> shift };
  return imin(imax(rows, 1U), SEARCH_TUPLES);
}

// Row `index` of the fixed table, as a delta from the profile as given: bit 0
// flips the scale-measure tiebreak, bit 1 the box packer, bit 2 hands each
// frame its owner's hole. Row 0 is therefore the caller's own tuple, and
// `portfolio_m` of 1 is the pipeline as it ran before the portfolio existed.
void search_tuple(scav_profile &p, DarSource &dar, uint32_t index) {
  p.sm_tiebreak ^= static_cast<int32_t>(index & 1U);
  p.trybox ^= static_cast<int32_t>((index >> 1U) & 1U);
  dar = (((index >> 2U) & 1U) != 0) ? DarSource::OwnerHole : DarSource::Profile;
}

// `argmin(Cost, index)` over the candidates, in index order: the combine is
// associative and not commutative, so the order is what makes it one value (6).
// `viable` is parallel to `cost`, and row 0 answers for a set with nothing in
// it -- the caller's own tuple, whose failure it was already told about.
uint32_t search_argmin(std::vector<Cost> const &cost, std::vector<uint8_t> const &viable) {
  uint32_t best{ 0 };
  bool found{ false };
  for (uint32_t i = 0; i < cost.size(); ++i) {
    if (viable[i] == 0) { continue; }
    if (!found || cost_less(cost[i], cost[best])) {
      best = i;
      found = true;
    }
  }
  return best;
}

SCAV_INTERNAL_END

bool layout_run(Chart &c,
                scav_spaces const &s,
                scav_layout_opts const &o,
                std::vector<scav_placed> &placed,
                std::vector<Diagnostic> &diags,
                uint32_t *inflations,
                uint32_t *tuple) {
  if (inflations != nullptr) { *inflations = 0; }
  if (tuple != nullptr) { *tuple = 0; }
  scav_profile const &p{ o.profile };
  if (!profile_validate(p)) {
    diags.push_back({ .code = DiagCode::ProfileOutOfRange,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }
  if (!spaces_validate(c, s, diags)) { return false; }

  if (geom_column_clash(c) != GeomCount) {
    diags.push_back({ .code = DiagCode::GeometryColumnClash,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }

  Router const *const router{ router_at(o.router) };
  if (router == nullptr) {
    diags.push_back({ .code = DiagCode::RouterUnknown,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }

  SplitGraph const g{ decompose(c) };
  // Once for every attempt below: phase 1 reads `sweep_count` and no extent, so
  // the inflated copies the retry loop makes order to the same rows.
  SubmachineOrders const orders{ order_submachines(c, g, s, p, o.threads) };

  // Level 2: every row of the table this chart's size admits, each its own
  // whole phase 2 and 3, collected rather than folded into a best-so-far, and
  // reduced in index order afterwards (6, 11.10). Row 0 is the profile as
  // given, so it is what a failure is reported for and what M of 1 runs alone.
  uint32_t const rows{ search_tuple_count(p, layout_entity_count(c)) };
  std::vector<Candidate> candidates(rows);
  std::vector<Cost> cost(rows);
  std::vector<uint8_t> viable(rows, 0);
  for (uint32_t i = 0; i < rows; ++i) {
    scav_profile knobs{ p };
    DarSource dar{ DarSource::Profile };
    search_tuple(knobs, dar, i);
    std::vector<Diagnostic> spilled;
    candidates[i] = search_candidate(c,
                                     g,
                                     orders,
                                     s,
                                     knobs,
                                     dar,
                                     *router,
                                     o.threads,
                                     (i == 0) ? diags : spilled);
    // A tuple that leaves the coordinate domain is no candidate, and the
    // caller's own tuple leaving it is the run's failure, as it was before
    // anything else was tried.
    if ((i == 0) && !candidates[0].viable) { return false; }
    viable[i] = candidates[i].viable ? 1U : 0U;
    // One row has nothing to rank, so it is not scored: `argmin` over one
    // candidate is that candidate, and this is what leaves a chart the scaling
    // rule gives one row costing exactly what it did before. The objective is
    // the caller's profile and not the tuple's copy, so two rows are compared
    // on one scale even where one of them inflated.
    if ((rows > 1) && (viable[i] != 0)) {
      CostTerms const t{
        cost_terms(c, g, candidates[i].sized, candidates[i].routes, s, p)
      };
      cost[i] = cost_of(t, p);
    }
  }
  uint32_t const best{ search_argmin(cost, viable) };
  SizedLayout sized{ std::move(candidates[best].sized) };
  Routes routes{ std::move(candidates[best].routes) };
  if (inflations != nullptr) { *inflations = candidates[best].inflations; }
  if (tuple != nullptr) { *tuple = best; }
  placed = routes.placed;

  // `failed` is parallel to the transitions, so one walk emits the findings in
  // ordinal order.
  for (uint32_t t = 0; t < routes.failed.size(); ++t) {
    if (routes.failed[t] == 0) { continue; }
    diags.push_back({ .code = DiagCode::RouteDegraded,
                      .subject = { .kind = ElemKind::Transition, .ordinal = t },
                      .doc = { INVALID },
                      .src = {} });
  }

  write_columns(c, sized, routes, inputs_digest(s, o));
  return true;
}

namespace {

// The column's rows, memcpy'd out so hashing never reads padding in place.
template <typename T>
std::vector<T> rows_of(Chart const &c, char const *name) {
  ColumnId const id{ column_find(c, name) };
  if (id.v == INVALID) { return {}; }
  std::vector<T> rows(column_count(c, id));
  if (!rows.empty()) {
    std::memcpy(rows.data(), column_data(c, id), rows.size() * sizeof(T));
  }
  return rows;
}

// 0, 1, or 2: decreasing, level, or increasing along one axis.
uint32_t direction_token(int32_t from, int32_t to) {
  if (to > from) { return 2U; }
  return (to < from) ? 0U : 1U;
}

}  // namespace

uint32_t layout_inputs_digest(Chart const &c) {
  ColumnId const id{ column_find(c, "scav.geom.inputs") };
  if ((id.v == INVALID) || (column_count(c, id) == 0)) { return 0; }
  uint32_t inputs{ 0 };
  std::memcpy(&inputs, column_data(c, id), 4);
  return inputs;
}

uint32_t layout_coordinate_hash(Chart const &c) {
  std::vector<scav_byte> b;
  for (char const *name : { "scav.geom.state",
                            "scav.geom.state_before",
                            "scav.geom.state_after",
                            "scav.geom.sub",
                            "scav.geom.chart" }) {
    for (scav_rect const &r : rows_of<scav_rect>(c, name)) {
      append_i32(b, r.x);
      append_i32(b, r.y);
      append_i32(b, r.w);
      append_i32(b, r.h);
    }
  }
  for (scav_point const &pt : rows_of<scav_point>(c, "scav.geom.point")) {
    append_i32(b, pt.x);
    append_i32(b, pt.y);
  }
  for (scav_port_slot const &sl : rows_of<scav_port_slot>(c, "scav.geom.portslot")) {
    append_i32(b, sl.x);
    append_i32(b, sl.y);
  }
  return xxhash32(b.data(), b.size(), 0);
}

uint32_t layout_structural_hash(Chart const &c) {
  std::vector<scav_byte> b;
  std::vector<scav_point> const points{ rows_of<scav_point>(c, "scav.geom.point") };
  for (scav_span const route : rows_of<scav_span>(c, "scav.geom.route")) {
    append_u32(b, route.len);
    // Direction tokens, not coordinates: a translation leaves these alone.
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      scav_point const &a{ points[route.off + k] };
      scav_point const &d{ points[route.off + k + 1] };
      append_u32(b, (direction_token(a.x, d.x) * 3U) + direction_token(a.y, d.y));
    }
  }
  for (scav_port_slot const &sl : rows_of<scav_port_slot>(c, "scav.geom.portslot")) {
    append_u32(b, sl.side);
    append_u32(b, sl.boundary_depth);
  }
  return xxhash32(b.data(), b.size(), chart_structural_hash(c));
}

}  // namespace scav
