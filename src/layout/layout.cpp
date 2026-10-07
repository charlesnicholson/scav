// Lays out a chart: phases 2 and 3 per search row, Level 1 search and kicks on each row,
// then the winner's geometry columns. Also the coordinate and structural hashes.

#include "layout/candidate_memo.h"
#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/geom.h"
#include "layout/memo.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/router.h"
#include "layout/shard.h"
#include "layout/size.h"
#include "layout/trace.h"
#include "layout/trace_stream.h"
#include "layout/wire.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_hash_map.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_rnd.h"
#include "scav_stable_sort.h"
#include "scav_thread.h"
#include "scav_vec.h"
#include "scav_xxhash.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace scav {

#ifdef SCAV_TESTING
void layout_test_prefix_shortcut(bool on);
void layout_test_prefix_verify(bool on);
uint64_t layout_test_prefix_used();
uint64_t layout_test_prefix_mismatches();
void layout_test_skip_noop_faces(bool on);
uint64_t layout_test_noop_faces();
void layout_test_cull(bool on, bool verify);
std::array<uint64_t, TRACE_MOVES> layout_test_culled();
uint64_t layout_test_cull_mismatches();
void layout_test_label_bound(bool on, bool verify);
uint64_t layout_test_label_bound_skipped();
uint64_t layout_test_label_bound_labelled();
uint64_t layout_test_label_bound_mismatches();
void layout_test_search_memo(bool on);
void layout_test_search_memo_verify(bool on);
void layout_test_no_search(bool on);
uint32_t layout_test_search_memo_hits();
uint32_t layout_test_search_memo_mismatches();
void layout_test_candidate_memo(bool on, bool verify);
void layout_test_candidate_memo_budget(uint64_t bytes);
uint32_t layout_test_candidate_memos_built();
void layout_test_candidate_memo_empty(uint32_t every);
uint64_t layout_test_candidate_memo_deduped();
uint64_t layout_test_candidate_memo_drawn();
uint64_t layout_test_candidate_memo_faced();
uint64_t layout_test_candidate_memo_mismatches();
void layout_test_route_bound(bool on, bool verify);
void layout_test_cost_stop(bool on);
uint64_t layout_test_cost_stopped();
uint64_t layout_test_route_bound_pruned();
uint64_t layout_test_route_bound_checked();
uint64_t layout_test_route_bound_mismatches();
void layout_test_row_alias(bool on);
std::vector<Cost> const &layout_test_schedule_first();
std::vector<Cost> const &layout_test_schedule_second();
std::vector<Cost> const &layout_test_schedule_kept();
void layout_test_dont_look_verify(bool on);
uint64_t layout_test_dont_look_checked();
uint64_t layout_test_dont_look_mismatches();
void layout_test_degrade(bool on);
uint64_t layout_test_degraded();
uint64_t layout_test_taken_degraded();
void layout_test_bound_replay(bool on);
uint64_t layout_test_bound_replayed();
uint64_t layout_test_bound_replay_mismatches();
#endif

SCAV_INTERNAL_BEGIN
// Test-visible; tests declare their own prototypes (see scav_internal.h).
bool inflation_done(uint32_t fewest, uint32_t degraded, uint32_t unreachable, bool &keep);
uint32_t search_tuple_count(scav_profile const &p);
void search_table(scav_profile const &p, std::vector<uint32_t> &rows);
uint32_t search_move_budget(scav_profile const &p);
Row search_row(scav_profile const &p, uint32_t index);
void search_changes(Chart const &c,
                    SizedLayout const &was,
                    Routes const &was_routes,
                    SizedLayout const &now,
                    Routes const &now_routes,
                    std::vector<uint8_t> &frame,
                    std::vector<uint8_t> &route,
                    std::vector<uint8_t> &resized);
uint32_t search_argmin(std::vector<Cost> const &cost, std::vector<uint8_t> const &viable);
// Orders `rest`, kick indices, by `cost` plus `jit`, ties keeping their order.
void kick_order(std::vector<uint32_t> &rest,
                std::vector<Cost> const &cost,
                std::vector<int64_t> const &jit);
// Writes to `key` every input of a Level 1 search: the objective, the row, the budget,
// `refold`, the seed pins in order, and `scope`.
void search_key(scav_profile const &objective,
                Row const &row,
                uint32_t budget,
                bool refold,
                SearchPins const &seed,
                std::vector<uint8_t> const *scope,
                std::vector<uint32_t> &key);
SCAV_INTERNAL_END

namespace {

constexpr uint32_t RECT{ sizeof(scav_rect) };

// A geometry column's shape; `write_rows` requires an existing column to match it.
struct GeomShape {
  char const *name;
  ElemKind entity;
  ValueKind kind;
  uint32_t elem_size;
};

// Index into `GEOM`.
enum GeomColumnIndex : uint32_t {
  GeomState,
  GeomBefore,
  GeomAfter,
  GeomLead,
  GeomTrail,
  GeomLoop,
  GeomLoopPlace,
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
    { .name = "scav.geom.state_lead",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.state_trail",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.state_loop",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.state_loop_place",
      .entity = ElemKind::State,
      .kind = ValueKind::U32,
      .elem_size = 4 },
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

// Finds or registers `g`'s column; an existing one has passed `geom_column_clash`.
ColumnId geom_column(Chart &c, GeomShape const &g) {
  ColumnId const found{ column_find(c, g.name) };
  if (found.v != INVALID) { return found; }
  return column_register(c, g.name, g.entity, g.kind, g.elem_size, 4, COLUMN_DERIVED);
}

// Index of the first `GEOM` name registered with another entity, value kind or element
// size; `GeomCount` when all match.
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

static_assert(sizeof(scav_profile) == 53 * sizeof(int32_t),
              "the profile must stay a flat block of int32 with no padding, or the "
              "inputs digest below would hash bytes whose values are unspecified");

// Hash of every non-geometry layout input: profile, router name and version, spaces.
uint32_t inputs_digest(scav_spaces const &s, scav_layout_opts const &o) {
  std::vector<scav_byte> b;
  std::array<int32_t, sizeof(scav_profile) / sizeof(int32_t)> profile{};
  std::memcpy(profile.data(), &o.profile, sizeof(scav_profile));
  for (int32_t const field : profile) { append_i32(b, field); }

  scav_byte const *name{ nullptr };
  uint32_t name_len{ 0 };
  uint32_t version{ 0 };
  if (router_name(o.router, name, name_len) && router_version(o.router, version)) {
    append_u32(b, name_len);
    vec_insert(b, b.end(), name, name + name_len);
    append_u32(b, version);
  }
  // The spaces digest stands in for the font.
  append_u32(b, spaces_digest(s));
  return xxhash32(b.data(), b.size(), 0);
}

void write_columns(Chart &c, SizedLayout const &z, Routes const &r, uint32_t inputs) {
  write_rows(c, geom_column(c, GEOM[GeomState]), z.state);
  write_rows(c, geom_column(c, GEOM[GeomBefore]), z.before);
  write_rows(c, geom_column(c, GEOM[GeomAfter]), z.after);
  write_rows(c, geom_column(c, GEOM[GeomLead]), z.lead);
  write_rows(c, geom_column(c, GEOM[GeomTrail]), z.trail);
  write_rows(c, geom_column(c, GEOM[GeomLoop]), z.loop);
  std::vector<uint32_t> place(z.loop_place.size());
  for (size_t i = 0; i < place.size(); ++i) { place[i] = z.loop_place[i]; }
  write_rows(c, geom_column(c, GEOM[GeomLoopPlace]), place);
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

// Raises rank, node and sub separation by `by`; false when `p` then fails validation.
bool inflate(scav_profile &p, int32_t by) {
  p.rank_sep += by;
  p.node_sep += by;
  p.sub_sep += by;
  return profile_validate(p);
}

struct Candidate {
  SizedLayout sized;
  Routes routes;
  uint32_t inflations{ 0 };
  bool viable{ false };
  // The caller's pins with the facing pass's reversals and sides applied; laying out
  // from them again turns no port.
  SearchPins laid;
  scav_rect sized_chart{};  // `sized.chart` before the routes and labels covered it
  bool retried{ false };    // the spacing retry sized the laid ordering again
};

bool same_rect(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

// True when two candidates place every state and route point alike.
bool same_geometry(Candidate const &a, Candidate const &b) {
  return std::ranges::equal(a.sized.state, b.sized.state, same_rect) &&
         std::ranges::equal(a.routes.points, b.routes.points, same);
}

// A cross-border side the facing pass has given to `node`.
struct FacingTaken {
  uint32_t node, side;
};

// Fills `out` for legs whose port faces away from its far end or is on a walled face: the
// port takes a cross border its state sees, else its edge reverses. `taken` is scratch.
void facing_flips(Facing &out,
                  std::vector<FacingTaken> &taken,
                  Chart const &c,
                  SplitGraph const &g,
                  SubmachineOrders const &o,
                  SizedLayout const &z,
                  scav_spaces const &s) {
  out.reverses.clear();
  out.sides.clear();
  taken.clear();
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if ((m >= o.sub_nodes.size()) || (m >= z.sub.size())) { continue; }
    Span const span{ o.sub_nodes[m] };
    Span const espan{ o.sub_edges[m] };
    scav_rect const frame{ z.sub[m] };
    // True when the frame's ranks run down the page, its rank borders top and bottom.
    bool const down{ (m < o.sub_down.size()) && (o.sub_down[m] != 0) };
    uint32_t const owner{ c.submachines[m].owner.v };
    auto const lined = [&](uint32_t face) {
      return (owner < c.states.size()) && face_lined(s, owner, face);
    };
    // True when `face` of the frame is lined or faces a live sibling region.
    auto const walled = [&](uint32_t face) {
      if (lined(face)) { return true; }
      if (owner >= c.states.size()) { return false; }
      Span const subs{ c.states[owner].submachines };
      for (uint32_t k = 0; k < subs.len; ++k) {
        uint32_t const sib{ c.submachine_ids[subs.off + k].v };
        if ((sib == m) || (sib >= z.sub.size()) || (c.submachines[sib].live == 0)) {
          continue;
        }
        scav_rect const &q{ z.sub[sib] };
        bool const beside{ (q.y < (frame.y + frame.h)) && (frame.y < (q.y + q.h)) };
        bool const stacked{ (q.x < (frame.x + frame.w)) && (frame.x < (q.x + q.w)) };
        if (((face == 0) && beside && ((q.x + q.w) <= frame.x)) ||
            ((face == 1) && beside && (q.x >= (frame.x + frame.w))) ||
            ((face == 2) && stacked && ((q.y + q.h) <= frame.y)) ||
            ((face == 3) && stacked && (q.y >= (frame.y + frame.h)))) {
          return true;
        }
      }
      return false;
    };
    // The state node `seg` reaches in this frame, past any bends; INVALID if none.
    auto const joined = [&](uint32_t seg) {
      for (uint32_t k = 0; k < espan.len; ++k) {
        OrderEdge const &e{ o.edges[espan.off + k] };
        if (e.segment != seg) { continue; }
        for (uint32_t const end : { e.src, e.dst }) {
          if (o.nodes[end].kind == OrderKind::State) { return end; }
        }
      }
      return INVALID;
    };
    // `node`'s low (`first`) or high cross-border side; INVALID when another state blocks
    // its view of the frame edge, the side is lined, or `node` holds it already.
    auto const seen = [&](uint32_t node, bool first) {
      scav_rect const nb{ z.state[o.nodes[node].subject] };
      Wide const lo{ down ? nb.y : nb.x };
      Wide const hi{ lo + (down ? nb.h : nb.w) };
      Wide edge{ first ? frame.y : (Wide{ frame.y } + frame.h) };
      Wide near{ first ? nb.y : (Wide{ nb.y } + nb.h) };
      if (down) {
        edge = first ? frame.x : (Wide{ frame.x } + frame.w);
        near = first ? nb.x : (Wide{ nb.x } + nb.w);
      }
      for (uint32_t k = 0; k < span.len; ++k) {
        OrderNode const &other{ o.nodes[span.off + k] };
        if (((span.off + k) == node) || (other.kind != OrderKind::State)) { continue; }
        scav_rect const r{ z.state[other.subject] };
        Wide const rlo{ down ? r.y : r.x };
        Wide const rhi{ rlo + (down ? r.h : r.w) };
        Wide const clo{ down ? r.x : r.y };
        Wide const chi{ clo + (down ? r.w : r.h) };
        bool const between{ first ? ((chi > edge) && (clo < near))
                                  : ((clo < edge) && (chi > near)) };
        if (between && (rhi > lo) && (rlo < hi)) { return INVALID; }
      }
      uint32_t const side{ (down ? 0U : 2U) + (first ? 0U : 1U) };
      if (walled(side)) { return INVALID; }
      for (FacingTaken const &had : taken) {
        if ((had.node == node) && (had.side == side)) { return INVALID; }
      }
      return side;
    };
    for (uint32_t k = 0; k < span.len; ++k) {
      OrderNode const &nd{ o.nodes[span.off + k] };
      if ((nd.kind != OrderKind::Boundary) || (nd.subject >= g.segments.size()) ||
          (o.seg_cross[nd.subject] == 0)) {
        continue;
      }
      uint32_t const node{ joined(nd.subject) };
      uint32_t const side{ o.seg_side[nd.subject] };
      if (node != INVALID) { vec_push_back(taken, { .node = node, .side = side }); }
    }
    for (uint32_t k = 0; k < span.len; ++k) {
      uint32_t const at{ span.off + k };
      OrderNode const &nd{ o.nodes[at] };
      if (nd.kind != OrderKind::Boundary) { continue; }
      uint32_t const seg{ nd.subject };
      if ((seg >= g.segments.size()) || (o.seg_sided[seg] != 0)) { continue; }
      TransId const t{ g.segments[seg].trans };
      if ((t.v >= c.transitions.size()) || (t.v >= g.trans_segments.size())) { continue; }
      uint32_t const leg{ seg - g.trans_segments[t.v].off };
      uint32_t const rank_face{ down ? 2U : 0U };
      bool const on_leading{ o.seg_side[seg] == rank_face };
      if (o.seg_port[seg] == INVALID) {
        // An inner-face end goes off a lined rank border onto the other one, if unlined.
        uint32_t const onto{ rank_face + (on_leading ? 1U : 0U) };
        if (lined(rank_face + (on_leading ? 0U : 1U)) && !lined(onto)) {
          vec_push_back(out.reverses, { .trans = t, .leg = leg });
          trace_emit({ .kind = TraceKind::PortTurned,
                       .frame = m,
                       .port = { .seg = seg, .trans = t.v, .leg = leg, .side = onto } });
        }
        continue;
      }
      Transition const &tr{ c.transitions[t.v] };
      // True when the leg exits the frame through the port; the far end is then `tr.dst`.
      bool const leaves{ g.segments[seg].dst_port == o.seg_port[seg] };
      StateId const far{ leaves ? tr.dst : tr.src };
      if (far.v >= z.state.size()) { continue; }
      scav_rect const &r{ z.state[far.v] };
      Wide const far_at{ down ? (Wide{ r.y } + (r.h / 2)) : (Wide{ r.x } + (r.w / 2)) };
      Wide const mid_at{ down ? (Wide{ frame.y } + (frame.h / 2))
                              : (Wide{ frame.x } + (frame.w / 2)) };
      Wide const far_across{ down ? (Wide{ r.x } + (r.w / 2))
                                  : (Wide{ r.y } + (r.h / 2)) };
      Wide const mid_across{ down ? (Wide{ frame.x } + (frame.w / 2))
                                  : (Wide{ frame.y } + (frame.h / 2)) };
      auto const beyond = [](Wide a, Wide b, int32_t extent) {
        return imax(((a > b) ? (a - b) : (b - a)) - (extent / 2), Wide{ 0 });
      };
      Wide const beyond_at{ beyond(far_at, mid_at, down ? frame.h : frame.w) };
      Wide const beyond_across{ beyond(far_across, mid_across, down ? frame.w : frame.h) };
      // `direct`: the port is on a state's border and the segment's other end is a state.
      bool const on_state{ g.ports[o.seg_port[seg]].state.v != INVALID };
      uint32_t const other{ leaves ? g.segments[seg].src_port : g.segments[seg].dst_port };
      bool const direct{ on_state && (other == INVALID) };
      uint32_t const node{ (direct && (beyond_across > beyond_at)) ? joined(seg)
                                                                   : INVALID };
      uint32_t const side{ (node != INVALID) ? seen(node, far_across < mid_across)
                                             : INVALID };
      if (side != INVALID) {
        vec_push_back(taken, { .node = node, .side = side });
        vec_push_back(out.sides,
                      { .trans = t, .leg = leg, .end = leaves ? 1U : 0U, .face = side });
        trace_emit({ .kind = TraceKind::PortTurned,
                     .frame = m,
                     .port = { .seg = seg, .trans = t.v, .leg = leg, .side = side } });
        continue;
      }
      bool wants_leading{ far_at < mid_at };
      if (on_state && walled(rank_face + (on_leading ? 0U : 1U))) {
        // A port on a walled face moves to the other rank border, else to a cross border
        // the joined state sees, else stays.
        uint32_t const j{ joined(seg) };
        uint32_t cross{ INVALID };
        for (bool const first : { true, false }) {
          if ((cross == INVALID) && (j != INVALID)) { cross = seen(j, first); }
        }
        if (walled(rank_face + (on_leading ? 1U : 0U)) && (cross == INVALID)) {
          trace_emit({ .kind = TraceKind::PortWalled,
                       .frame = m,
                       .port = { .seg = seg,
                                 .trans = t.v,
                                 .leg = leg,
                                 .side = rank_face + (on_leading ? 0U : 1U) } });
        }
        if (walled(rank_face + (on_leading ? 1U : 0U)) && (cross != INVALID)) {
          vec_push_back(taken, { .node = j, .side = cross });
          vec_push_back(
              out.sides,
              { .trans = t, .leg = leg, .end = leaves ? 1U : 0U, .face = cross });
          trace_emit({ .kind = TraceKind::PortTurned,
                       .frame = m,
                       .port = { .seg = seg, .trans = t.v, .leg = leg, .side = cross } });
          continue;
        }
        wants_leading = !on_leading;
      }
      if (on_leading == wants_leading) { continue; }
      if (on_state && walled(rank_face + (wants_leading ? 0U : 1U))) { continue; }
      vec_push_back(out.reverses, { .trans = t, .leg = leg });
      uint32_t const along{ (down ? 2U : 0U) + (wants_leading ? 0U : 1U) };
      trace_emit({ .kind = TraceKind::PortTurned,
                   .frame = m,
                   .port = { .seg = seg, .trans = t.v, .leg = leg, .side = along } });
    }
  }
}

// A candidate's phases 1 and 2, facing pass and first routing. Phases 1 and 2 and the
// facing pass read no box-end pin; candidates differing only in those share them.
struct Prefix {
  SubmachineOrders laid;
  std::vector<std::vector<uint32_t>> bends;  // `laid`'s, as `segment_bends` writes them
  SizedLayout sized;
  Facing flips;
  Routes routes;  // as phase 3 first routes it, over `sized`
  bool ok{ false };
};

// Applies the facing pass's reversals and sides to `turned`; a reversal it names again
// is dropped.
void turn_pins(SearchPins &turned, Facing const &flips) {
  for (ReversePin const &f : flips.reverses) {
    auto const had{ std::ranges::find_if(turned.reverses, [&f](ReversePin const &r) {
      return (r.trans == f.trans) && (r.leg == f.leg);
    }) };
    if (had != turned.reverses.end()) {
      turned.reverses.erase(had);
    } else {
      vec_push_back(turned.reverses, f);
    }
  }
  vec_insert(turned.ends, turned.ends.end(), flips.sides.begin(), flips.sides.end());
}

// Scratch one candidate fills and discards beside its `Candidate`.
struct CandidateScratch {
  SubmachineOrders facing;
  SizedLayout again;
  Facing flips;
  std::vector<FacingTaken> taken;
};

// Grows `sized.chart` to cover every route point and placed label box; saves the
// uncovered chart in `sized_chart`.
void cover_chart(Candidate &out) {
  auto const cover = [&out](int32_t x, int32_t y) {
    scav_rect &chart{ out.sized.chart };
    int32_t const right{ imax(chart.x + chart.w, x) };
    int32_t const bottom{ imax(chart.y + chart.h, y) };
    chart.x = imin(chart.x, x);
    chart.y = imin(chart.y, y);
    chart.w = right - chart.x;
    chart.h = bottom - chart.y;
  };
  out.sized_chart = out.sized.chart;
  for (scav_point const &at : out.routes.points) { cover(at.x, at.y); }
  for (scav_rect const &at : out.routes.placed) {
    cover(at.x, at.y);
    cover(at.x + at.w, at.y + at.h);
  }
}

// Labels `cand`'s unlabelled routes as `search_candidate` would, then re-covers the chart.
void label_candidate(Candidate &cand,
                     Chart const &c,
                     SplitGraph const &g,
                     scav_spaces const &s,
                     scav_profile const &knobs) {
  cand.sized.chart = cand.sized_chart;  // the chart `route_transitions` saw
  label_routes(cand.routes, c, g, cand.sized, s, knobs);
  cover_chart(cand);
}

// The caches a candidate reads and writes, each optional.
struct CandidateReuse {
  RouteCache const *reuse{ nullptr };  // the incumbent's routes, read
  RouteCache *fill{ nullptr };         // this candidate's routes, written
  Prefix *prefix{ nullptr };           // receives the prefix
  Prefix const *from{ nullptr };       // a prefix whose pins differ only in faces
  CandidateScratch *scratch{ nullptr };
  RouteStop *stop{ nullptr };  // where routing may stop
};

// Sizes `orders`, turns ports to face their routes, then orders and sizes again, writing
// `out.sized`, `out.laid`, `sc.flips` and `use`; false when the first sizing fails.
bool lay_facing(Candidate &out,
                CandidateScratch &sc,
                Chart const &c,
                SplitGraph const &g,
                SubmachineOrders const &orders,
                scav_spaces const &s,
                Row const &row,
                uint32_t threads,
                std::vector<Diagnostic> &diags,
                SearchPins const *pins,
                SubmachineOrders const *&use) {
  scav_profile const &knobs{ row.knobs };
  use = &orders;
  if (!size_layout(c,
                   g,
                   orders,
                   s,
                   knobs,
                   out.sized,
                   diags,
                   row.dar,
                   row.pack,
                   row.fold)) {
    out.routes = Routes{};
    out.laid = SearchPins{};
    return false;
  }
  SearchPins &turned{ out.laid };
  if (pins != nullptr) {
    turned = *pins;  // copy-assigned, keeping `turned`'s storage
  } else {
    turned = SearchPins{};
  }
  Facing &flips{ sc.flips };
  facing_flips(flips, sc.taken, c, g, orders, out.sized, s);
  if (!flips.reverses.empty() || !flips.sides.empty()) {
    turn_pins(turned, flips);
    order_submachines(sc.facing, c, g, s, knobs, threads, turned);
    SizedLayout &again{ sc.again };
    std::vector<Diagnostic> spilled;
    if (size_layout(c,
                    g,
                    sc.facing,
                    s,
                    knobs,
                    again,
                    spilled,
                    row.dar,
                    row.pack,
                    row.fold)) {
      use = &sc.facing;
      std::swap(out.sized, again);
    }
  }
  return true;
}

#ifdef SCAV_TESTING
// Test switch: armed, the first routing records its drawing and every routing of it counts
// a net outside its region. The counters tally those routings and moves taken onto one.
bool test_degrade{ false };
Mutex test_degrade_lock;
std::vector<int32_t> test_degrade_drawing;  // state rects then route points
std::atomic<uint64_t> test_degraded{ 0 };
std::atomic<uint64_t> test_taken_degraded{ 0 };

// Counts a net of `out` outside its region where `out` routes the recorded drawing.
void degrade_for_test(Candidate &out) {
  if (!test_degrade) { return; }
  std::vector<int32_t> words;
  for (scav_rect const &r : out.sized.state) {
    vec_insert(words, words.end(), { r.x, r.y, r.w, r.h });
  }
  for (scav_point const &p : out.routes.points) {
    vec_insert(words, words.end(), { p.x, p.y });
  }
  ScopedLock const held{ test_degrade_lock };
  if (test_degrade_drawing.empty()) {
    test_degrade_drawing = std::move(words);
  } else if (words != test_degrade_drawing) {
    return;
  }
  ++out.routes.outside_region;
  ++test_degraded;
}
#endif

// Phase 3 for `out` over `laid` and `out.sized`: routes, retries at wider spacing while a
// route is unreachable, and covers the chart.
void lay_routes(Candidate &out,
                Chart const &c,
                SplitGraph const &g,
                SubmachineOrders const &laid,
                scav_spaces const &s,
                Row const &row,
                Router const &router,
                uint32_t threads,
                SearchPins const *pins,
                CandidateReuse const &with,
                bool labels) {
  scav_profile const &knobs{ row.knobs };
  out.retried = false;
  route_transitions(out.routes,
                    c,
                    g,
                    laid,
                    out.sized,
                    s,
                    knobs,
                    router,
                    threads,
                    with.reuse,
                    with.fill,
                    pins,
                    labels,
                    with.stop);
#ifdef SCAV_TESTING
  degrade_for_test(out);
#endif
  if ((with.stop != nullptr) && with.stop->stopped) {
    out.viable = true;
    return;
  }
  if (with.prefix != nullptr) { with.prefix->routes = out.routes; }

  // Spacing retry: `out` holds the best attempt so far, `fewest` its degraded count.
  scav_profile wider{ knobs };
  uint32_t fewest{ out.routes.degraded() };
  bool done{ out.routes.unreachable == 0 };
  // A zero increment disables the retry.
  for (int32_t k = 0; !done && (knobs.spacing_inflation_increment > 0) &&
                      (k < knobs.spacing_inflation_cap);
       ++k) {
    if (!inflate(wider, knobs.spacing_inflation_increment)) { break; }
    out.retried = true;
    SizedLayout next_sized;
    std::vector<Diagnostic> spilled;
    if (!size_layout(c,
                     g,
                     laid,
                     s,
                     wider,
                     next_sized,
                     spilled,
                     row.dar,
                     row.pack,
                     row.fold)) {
      break;
    }
    Routes next{ route_transitions(c,
                                   g,
                                   laid,
                                   next_sized,
                                   s,
                                   wider,
                                   router,
                                   threads,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   labels) };
    bool keep{ false };
    done = inflation_done(fewest, next.degraded(), next.unreachable, keep);
    if (keep) {
      fewest = next.degraded();
      out.sized = std::move(next_sized);
      out.routes = std::move(next);
      out.inflations = static_cast<uint32_t>(k) + 1;
      trace_emit(
          { .kind = TraceKind::SpacingInflated,
            .inflate = { .node_sep = wider.node_sep, .rank_sep = wider.rank_sep } });
    }
  }

  cover_chart(out);
  out.viable = true;
}

// Runs phases 2 and 3 for `row` into `out`, reusing `out`'s and `with.scratch`'s storage;
// `with.from` supplies phases 1 and 2 in place of `orders`.
void search_candidate(Candidate &out,
                      Chart const &c,
                      SplitGraph const &g,
                      SubmachineOrders const &orders,
                      scav_spaces const &s,
                      Row const &row,
                      Router const &router,
                      uint32_t threads,
                      std::vector<Diagnostic> &diags,
                      SearchPins const *pins,
                      CandidateReuse const &with,
                      bool labels) {
  Prefix *const prefix{ with.prefix };
  Prefix const *const from{ with.from };
  CandidateScratch own;
  CandidateScratch &sc{ (with.scratch != nullptr) ? *with.scratch : own };
  out.inflations = 0;
  out.viable = false;
  out.sized_chart = {};
  out.retried = false;
  SubmachineOrders const *use{ &orders };
  if (from != nullptr) {
    if (!from->ok) {
      out = Candidate{};
      return;
    }
    out.sized = from->sized;
    out.laid = (pins != nullptr) ? *pins : SearchPins{};
    turn_pins(out.laid, from->flips);
    use = &from->laid;
  } else {
    if (!lay_facing(out, sc, c, g, orders, s, row, threads, diags, pins, use)) {
      if (prefix != nullptr) { prefix->ok = false; }
      return;
    }
    if (prefix != nullptr) {
      prefix->laid = *use;
      std::vector<uint32_t> reversed;
      segment_bends(*use,
                    static_cast<uint32_t>(g.segments.size()),
                    reversed,
                    prefix->bends);
      prefix->sized = out.sized;
      prefix->flips = sc.flips;
      prefix->ok = true;
    }
  }
  lay_routes(out, c, g, *use, s, row, router, threads, pins, with, labels);
}

Candidate search_candidate(Chart const &c,
                           SplitGraph const &g,
                           SubmachineOrders const &orders,
                           scav_spaces const &s,
                           Row const &row,
                           Router const &router,
                           uint32_t threads,
                           std::vector<Diagnostic> &diags,
                           SearchPins const *pins,
                           CandidateReuse const &with = {}) {
  Candidate out;
  search_candidate(out, c, g, orders, s, row, router, threads, diags, pins, with, true);
  return out;
}

}  // namespace

SCAV_INTERNAL_BEGIN

// True when the inflation loop stops; `keep` is set when this attempt degrades
// fewer routes than the best so far.
bool inflation_done(uint32_t fewest, uint32_t degraded, uint32_t unreachable, bool &keep) {
  keep = degraded < fewest;
  // Stops only on a kept attempt that reaches every end.
  return keep && (unreachable == 0);
}

SCAV_INTERNAL_END

namespace {

// A Level 1 search's result: the best candidate reached, its cost, and its pins.
struct Improved {
  Candidate best;
  Cost cost{};
  SearchPins held;
  bool viable{ false };  // the start laid out
};

// `Orient` is a kick only.
enum class MoveKind : uint32_t { Rank, Cut, Reverse, Face, Side, Fold, Orient, Loop };
static_assert((static_cast<uint16_t>(MoveKind::Rank) == TRACE_MOVE_RANK) &&
                  (static_cast<uint16_t>(MoveKind::Cut) == TRACE_MOVE_CUT) &&
                  (static_cast<uint16_t>(MoveKind::Reverse) == TRACE_MOVE_REVERSE) &&
                  (static_cast<uint16_t>(MoveKind::Face) == TRACE_MOVE_FACE) &&
                  (static_cast<uint16_t>(MoveKind::Side) == TRACE_MOVE_SIDE) &&
                  (static_cast<uint16_t>(MoveKind::Fold) == TRACE_MOVE_FOLD) &&
                  (static_cast<uint16_t>(MoveKind::Orient) == TRACE_MOVE_ORIENT) &&
                  (static_cast<uint16_t>(MoveKind::Loop) == TRACE_MOVE_LOOP) &&
                  ((static_cast<uint32_t>(MoveKind::Loop) + 1) == TRACE_MOVES),
              "the trace names a move by this enum's ordinal");
// One Level 1 move or kick; `kind` names which of its pins is set. `Face` is an end pin
// at a box end, `Side` one at a port end.
struct Move {
  RankPin pin{};
  ChainCut leg{};
  EndPin end_pin{};
  FoldPin fold{};
  OrientPin orient{};
  LoopPin loop{};
  MoveKind kind{ MoveKind::Rank };
};

// Appends `m`'s pin to `into`; a reversal already pinned is lifted instead.
void add_move(SearchPins &into, Move const &m) {
  switch (m.kind) {
    case MoveKind::Cut: vec_push_back(into.cuts, m.leg); break;
    case MoveKind::Reverse: {
      for (size_t k = 0; k < into.reverses.size(); ++k) {
        ReversePin const &had{ into.reverses[k] };
        if ((had.trans.v == m.leg.trans.v) && (had.leg == m.leg.leg)) {
          into.reverses.erase(into.reverses.begin() + static_cast<std::ptrdiff_t>(k));
          return;
        }
      }
      vec_push_back(into.reverses, { .trans = m.leg.trans, .leg = m.leg.leg });
      break;
    }
    case MoveKind::Face:
    case MoveKind::Side: vec_push_back(into.ends, m.end_pin); break;
    case MoveKind::Fold: vec_push_back(into.folds, m.fold); break;
    case MoveKind::Orient: vec_push_back(into.orients, m.orient); break;
    case MoveKind::Loop: vec_push_back(into.loops, m.loop); break;
    case MoveKind::Rank: vec_push_back(into.ranks, m.pin); break;
  }
}

// A scored move's cost and term shares.
struct Scored {
  Cost cost{};
  std::array<int32_t, TIER2_TERMS> share{};
  bool viable{ false };
  bool inflated{ false };
  bool degraded{ false };  // a net fell back to a straight line
  bool stopped{ false };   // `cost` is the Tier 2 its count stopped at, a bound
};

void put_pins(SearchPins const &p, std::vector<uint32_t> &w) {
  vec_push_back(w, static_cast<uint32_t>(p.ranks.size()));
  for (RankPin const &r : p.ranks) { vec_insert(w, w.end(), { r.state.v, r.rank }); }
  vec_push_back(w, static_cast<uint32_t>(p.cuts.size()));
  for (ChainCut const &k : p.cuts) { vec_insert(w, w.end(), { k.trans.v, k.leg }); }
  vec_push_back(w, static_cast<uint32_t>(p.reverses.size()));
  for (ReversePin const &r : p.reverses) { vec_insert(w, w.end(), { r.trans.v, r.leg }); }
  vec_push_back(w, static_cast<uint32_t>(p.ends.size()));
  for (EndPin const &e : p.ends) {
    vec_insert(w, w.end(), { e.trans.v, e.leg, e.end, e.face });
  }
  vec_push_back(w, static_cast<uint32_t>(p.orients.size()));
  for (OrientPin const &o : p.orients) { vec_push_back(w, o.frame.v); }
  vec_push_back(w, static_cast<uint32_t>(p.folds.size()));
  for (FoldPin const &f : p.folds) {
    vec_insert(w, w.end(), { f.frame.v, f.mode, f.layer });
  }
  vec_push_back(w, static_cast<uint32_t>(p.loops.size()));
  for (LoopPin const &l : p.loops) {
    vec_insert(w, w.end(), { l.state.v, l.face, l.end });
  }
}

bool same_pins(SearchPins const &a, SearchPins const &b) {
  std::vector<uint32_t> wa;
  std::vector<uint32_t> wb;
  put_pins(a, wa);
  put_pins(b, wb);
  return wa == wb;
}

// Whether two rows size every ordering alike by construction: the same knobs and tuple.
bool same_row(Row const &a, Row const &b) {
  return (std::memcmp(&a.knobs, &b.knobs, sizeof(scav_profile)) == 0) &&
         (a.dar == b.dar) && (a.pack == b.pack) && (a.fold == b.fold);
}

#ifdef SCAV_TESTING

// Whether two candidates are one drawing.
bool same_drawing(Candidate const &a, Candidate const &b) {
  return (a.viable == b.viable) && (a.inflations == b.inflations) &&
         same_rect(a.sized.chart, b.sized.chart) &&
         same_rect(a.sized_chart, b.sized_chart) && same_geometry(a, b) &&
         std::ranges::equal(a.sized.loop, b.sized.loop, same_rect) &&
         std::ranges::equal(a.routes.placed, b.routes.placed, same_rect);
}

// Whether two candidates are one drawing on one set of pins.
bool same_candidate(Candidate const &a, Candidate const &b) {
  return same_drawing(a, b) && same_pins(a.laid, b.laid);
}

bool same_cost(Cost const &a, Cost const &b) {
  return (a.t0_violations == b.t0_violations) && (a.t1_hints == b.t1_hints) &&
         (a.t2 == b.t2);
}

bool same_scored(Scored const &a, Scored const &b) {
  return (a.viable == b.viable) && (a.inflated == b.inflated) &&
         (a.degraded == b.degraded) && same_cost(a.cost, b.cost) && (a.share == b.share);
}
#endif

#ifdef SCAV_TESTING
// Test switches: score face moves from the incumbent's prefix; verify each against a whole
// lay-out. The counters tally uses and mismatches.
bool test_prefix_shortcut{ true };
bool test_prefix_verify{ false };
Mutex test_prefix_lock;
std::atomic<uint64_t> test_prefix_used{ 0 };
std::atomic<uint64_t> test_prefix_mismatches{ 0 };

// Test switches: score moves through the candidate memo; verify each move it answers
// against a lay-out without it. The counters tally answers and mismatches.
bool test_candidate_memo{ true };
bool test_candidate_memo_verify{ false };
uint32_t test_candidate_memo_empty{ 0 };  // empty it before every this-many labellings
std::atomic<uint32_t> test_candidate_memo_labellings{ 0 };
uint64_t test_candidate_memo_budget{ CandidateMemo::BUDGET };
std::atomic<uint32_t> test_candidate_memos_built{ 0 };  // candidate memos layout_run built
Mutex test_candidate_memo_lock;
std::atomic<uint64_t> test_candidate_memo_deduped{ 0 };
std::atomic<uint64_t> test_candidate_memo_drawn{ 0 };
std::atomic<uint64_t> test_candidate_memo_faced{ 0 };
std::atomic<uint64_t> test_candidate_memo_mismatches{ 0 };

// Test switches: leave unrouted a move whose route bound reaches the incumbent; check each
// bound against its move routed. The counters tally pruned moves, checks and failures.
bool test_route_bound{ true };
bool test_cost_stop{ true };  // stop a routed move's count at the incumbent
std::atomic<uint64_t> test_cost_stopped{ 0 };
bool test_route_bound_verify{ false };
Mutex test_route_bound_lock;
std::atomic<uint64_t> test_route_bound_pruned{ 0 };
std::atomic<uint64_t> test_route_bound_checked{ 0 };
std::atomic<uint64_t> test_route_bound_mismatches{ 0 };
#endif

// True when every path box fits `chart`; a placed box then lies inside it.
bool labels_fit(scav_spaces const &s, scav_rect const &chart) {
  for (uint32_t i = 0; (s.path_box != nullptr) && (i < s.n_path_box); ++i) {
    if ((s.path_box[i].w > chart.w) || (s.path_box[i].h > chart.h)) { return false; }
  }
  return true;
}

// Without `labelled`, scores a candidate routed unlabelled as a lower bound on its cost:
// label terms unpriced, the chart at its least extent.
Scored scored_of(Chart const &c,
                 SplitGraph const &g,
                 CostContext const &scoring,
                 scav_spaces const &s,
                 scav_profile const &objective,
                 Candidate const &cand,
                 bool labelled = true,
                 int64_t stop_at = -1) {  // 0 or more: stop once Tier 2 reaches it
  Scored out;
  if (!cand.viable) { return out; }
  out.viable = true;
  if (cand.inflations != 0) {
    out.inflated = true;
    return out;
  }
  if (cand.routes.degraded() != 0) {
    out.degraded = true;
    return out;
  }
  // A bound zeroes aspect while a path box could still grow the chart.
  bool const aspect{ labelled || labels_fit(s, cand.sized_chart) };
  CostStop stop{ .t2 = stop_at, .aspect = aspect };
  CostTerms terms{ cost_terms(scoring,
                              c,
                              g,
                              cand.sized,
                              cand.routes,
                              s,
                              objective,
                              nullptr,
                              nullptr,
                              (stop_at >= 0) ? &stop : nullptr) };
  if (!aspect) { terms.aspect = 0; }
  if (stop.stopped) {
    out.cost = { .t0_violations = 0, .t1_hints = 0, .t2 = cost_of(terms, objective).t2 };
    out.stopped = true;
    return out;
  }
  out.cost = cost_of(terms, objective);
  std::array<int64_t, TIER2_TERMS> const share{ cost_shares(terms, objective) };
  for (uint32_t k = 0; k < TIER2_TERMS; ++k) {
    out.share[k] = static_cast<int32_t>(share[k]);
  }
  return out;
}

// The candidate a move is scored on, in the scoring thread's scratch.
struct Routed {
  Candidate *cand{ nullptr };
};

// A candidate the bound pass routed, kept to be labelled.
struct KeptCandidate {
  Candidate cand;
  Cost bound{};
  uint32_t index{ INVALID };
  uint32_t entry{ INVALID };  // its score entry
};

// Per-thread scratch for scoring one move; a move runs on one thread.
struct MoveScratch {
  SearchPins pins;              // the move's pins, valid until its scoring returns
  std::vector<int32_t> shares;  // the move's route bound's bends per transition
  SubmachineOrders moved;
  Candidate whole, face;
  CandidateScratch keep;
  std::vector<uint32_t> faces;  // the move's box-end faces
  std::vector<uint32_t> reversed;
  std::vector<std::vector<uint32_t>> bends;  // the laid ordering's, per segment
};

// A search's view of the candidate memo; a null `table` scores every move in full.
struct MemoAccess {
  CandidateMemo *table{ nullptr };
  uint32_t row{ 0 };          // the search row's `row_word`
  uint32_t profile{ 0 };      // the search row's `profile_word`
  uint32_t drawn{ INVALID };  // the incumbent's drawing
  // The incumbent's phase-1 ordering, laid ordering and drawing, encoded.
  CandidateMemo::Blocks const *ordered{ nullptr };
  CandidateMemo::Blocks const *laid{ nullptr };
  CandidateMemo::Blocks const *drawing{ nullptr };
  Cost incumbent{};     // the round's incumbent
  bool prune{ false };  // a move whose bound reaches `incumbent` scores as it
  bool defer{ false };  // a move whose drawing another thread is routing is left unscored
};

// How one scored move met the memo.
struct MemoUse {
  uint32_t entry{ INVALID };  // its score entry
  bool deduped{ false };      // the score memo answered it
  bool drawn{ false };        // the answer came by its drawing
  bool labelled{ false };     // the answer is the labelled score
  bool faced{ false };        // the facing memo answered its facing pass
  bool pruned{ false };       // its route bound reached the incumbent
  bool deferred{ false };     // left unscored while another thread routed its drawing
  bool stopped{ false };      // routed, its Tier 2 then reached the incumbent
  bool relaid{ false };       // its bound laid out afresh for its don't-look bit
};

MemoScore memo_score(Scored const &s) {
  return { .cost = s.cost,
           .viable = s.viable,
           .inflated = s.inflated,
           .degraded = s.degraded };
}

Scored scored_from(MemoScore const &m) {
  Scored out;
  out.cost = m.cost;
  out.viable = m.viable;
  out.inflated = m.inflated;
  out.degraded = m.degraded;
  return out;
}

MoveScratch &move_scratch() {
  thread_local MoveScratch s;
  return s;
}

// `cost_bound` of a laid ordering with `bends` sized as `z` under the move's faces, routed
// by `router`; `seated` and `per_trans` as `cost_bound` takes them.
CostTerms route_bound(Chart const &c,
                      SplitGraph const &g,
                      std::vector<std::vector<uint32_t>> const &bends,
                      SizedLayout const &z,
                      std::vector<uint32_t> const &faces,
                      scav_profile const &objective,
                      Row const &row,
                      Router const &router,
                      bool seated,
                      std::vector<int32_t> *per_trans = nullptr) {
  return cost_bound(c,
                    g,
                    bends,
                    z,
                    faces,
                    objective,
                    route_clearance(row.knobs),
                    router,
                    seated,
                    per_trans);
}

// The seated bound is computed where the unseated one reaches this many tenths of the
// incumbent's Tier 2.
constexpr int64_t SEATED_FROM_TENTHS{ 7 };

#ifdef SCAV_TESTING
// Turns of transition `t`'s polyline in `r`, counted as `cost_terms` counts bends.
uint32_t route_turns(Routes const &r, uint32_t t) {
  scav_span const at{ r.route[t] };
  uint32_t out{ 0 };
  for (uint32_t k = 0; (k + 2) < at.len; ++k) {
    scav_point const *p{ r.points.data() + at.off + k };
    out += (direction(p[0], p[1]) != direction(p[1], p[2])) ? 1U : 0U;
  }
  return out;
}

// Tallies a check of `bound` and its `shares` against `cand` routed with Tier 0 zero, no
// degraded net and no inflation: each term, each transition's bends, and the sum.
void check_route_bound(Chart const &c,
                       SplitGraph const &g,
                       CostContext const &scoring,
                       scav_spaces const &s,
                       scav_profile const &objective,
                       Candidate const &cand,
                       CostTerms const &bound,
                       std::vector<int32_t> const &shares) {
  bool holds{ true };
  if (cand.viable && (cand.inflations == 0) && (cand.routes.degraded() == 0)) {
    CostTerms const t{ cost_terms(scoring, c, g, cand.sized, cand.routes, s, objective) };
    Cost const cost{ cost_of(t, objective) };
    holds = (cost.t0_violations != 0) ||
            ((t.area >= bound.area) && (t.whitespace >= bound.whitespace) &&
             (t.adjacency >= bound.adjacency) && (t.length >= bound.length) &&
             (t.bends >= bound.bends) && (cost.t2 >= cost_of(bound, objective).t2));
    for (uint32_t tr = 0; (cost.t0_violations == 0) && (tr < shares.size()); ++tr) {
      holds = holds && (static_cast<int64_t>(route_turns(cand.routes, tr)) >= shares[tr]);
    }
  }
  ScopedLock const held{ test_route_bound_lock };
  ++test_route_bound_checked;
  test_route_bound_mismatches += holds ? 0U : 1U;
}
#endif

// `score_move` through `memo.table`: a laid ordering or drawing seen under the same faces
// takes its stored score, and a phase-1 ordering seen its facing pass's turns.
Scored score_memoized(Chart const &c,
                      SplitGraph const &g,
                      CostContext const &scoring,
                      scav_spaces const &s,
                      scav_profile const &objective,
                      Row const &row,
                      Router const &router,
                      SearchPins const &pins,
                      RouteCache const *reuse,
                      Prefix const *from,
                      bool labels,
                      Routed *routed,
                      MemoAccess const &memo,
                      MemoUse &use) {
  MoveScratch &sc{ move_scratch() };
  CandidateMemo &table{ *memo.table };
  Candidate &cand{ (from != nullptr) ? sc.face : sc.whole };
  cand.inflations = 0;
  cand.viable = false;
  cand.sized_chart = {};
  cand.retried = false;
  Scored out;
  table.box_faces(&pins, sc.faces);
  int64_t stored_bound{ -1 };  // the entry's route bound Tier 2, -1 while unset
  // Takes `got`'s entry and the score it answers with, if any; true when one does.
  auto const take = [&](CandidateMemo::Recalled const &got) {
    use.entry = got.entry;
    use.deduped = got.found;
    use.labelled = got.labelled;
    stored_bound = got.route_bound;
    if (got.found) { out = scored_from(got.score); }
    return got.found;
  };
  // Scores the move as its route bound where Tier 2 `t2` reaches the incumbent; true then.
  auto const prune_at = [&](int64_t t2) {
    Cost const bound{ .t0_violations = 0, .t1_hints = 0, .t2 = t2 };
    if (!memo.prune || (t2 < 0) || cost_less(bound, memo.incumbent)) { return false; }
    out = Scored{};
    out.cost = bound;
    out.viable = true;
    use.pruned = true;
    return true;
  };
  CostTerms bound{};                          // the move's route bound where `bounded`
  std::vector<int32_t> &shares{ sc.shares };  // its bends per transition
  bool bounded{ false };
  // A move routed against a Tier-0-free incumbent stops once it reaches it.
  bool stops{ memo.prune && (memo.incumbent.t0_violations == 0) &&
              (memo.incumbent.t1_hints == 0) };
#ifdef SCAV_TESTING
  stops = stops && test_cost_stop;
#endif
  // Prunes by the stored route bound, else by `bends` sized as `z`'s, which it stores.
  auto const pruned_by = [&](std::vector<std::vector<uint32_t>> const &bends,
                             SizedLayout const &z) {
    if (prune_at(stored_bound)) { return true; }
    bool const fresh{ memo.prune && (stored_bound < 0) &&
                      (memo.incumbent.t0_violations == 0) };
    bool seat_all{ false };
#ifdef SCAV_TESTING
    seat_all = test_route_bound_verify;
#endif
    if (!fresh && !stops && !seat_all) { return false; }
    bound = route_bound(c, g, bends, z, sc.faces, objective, row, router, false, &shares);
    int64_t t2{ cost_of(bound, objective).t2 };
    if (seat_all || ((t2 < memo.incumbent.t2) &&
                     ((t2 * 10) >= (memo.incumbent.t2 * SEATED_FROM_TENTHS)))) {
      bound = route_bound(c, g, bends, z, sc.faces, objective, row, router, true, &shares);
      t2 = cost_of(bound, objective).t2;
    }
    bounded = true;
    if (fresh || seat_all) { table.set_route_bound(use.entry, t2); }
    return prune_at(t2);
  };
  // Looks up drawing `drawn` under the move's faces; true when a stored score answers.
  auto const recall_drawn = [&](uint32_t drawn) {
    if (drawn == INVALID) {
      use.entry = INVALID;
      return false;
    }
    use.drawn = take(table.find_score(drawn, sc.faces, labels));
    return use.drawn;
  };
  SubmachineOrders const *laid{ nullptr };
  bool sized{ false };
  if (from != nullptr) {
    if (!from->ok) {
      cand = Candidate{};
      return out;
    }
    if (recall_drawn(memo.drawn) || pruned_by(from->bends, from->sized)) { return out; }
    cand.sized = from->sized;
    cand.laid = pins;
    turn_pins(cand.laid, from->flips);
    laid = &from->laid;
  } else {
    order_submachines(sc.moved, c, g, s, objective, 1, pins);
    uint32_t const arranged{ table.arrangement(sc.moved, memo.ordered) };
    Facing &flips{ sc.keep.flips };
    FacingFound const found{ (arranged != INVALID)
                                 ? table.find_facing(memo.row, arranged, sc.moved, flips)
                                 : FacingFound::Absent };
    use.faced = found != FacingFound::Absent;
    if (found == FacingFound::Failed) {
      cand.routes = Routes{};
      cand.laid = SearchPins{};
      return out;
    }
    laid = &sc.moved;
    if (found == FacingFound::Turned) {
      cand.laid = pins;
      if (!flips.reverses.empty() || !flips.sides.empty()) {
        turn_pins(cand.laid, flips);
        order_submachines(sc.keep.facing, c, g, s, row.knobs, 1, cand.laid);
        laid = &sc.keep.facing;
      }
    } else {
      std::vector<Diagnostic> spilled;
      bool const ok{
        lay_facing(cand, sc.keep, c, g, sc.moved, s, row, 1, spilled, &pins, laid)
      };
      if (arranged != INVALID) {
        table.store_facing(memo.row, arranged, sc.moved, ok ? &flips : nullptr);
      }
      if (!ok) { return out; }
      sized = true;
    }
    uint32_t key{ INVALID };  // the laid ordering's key
    // Looks up the laid ordering under the move's faces; true when its linked score
    // answers.
    auto const recall_laid = [&]() {
      uint32_t const at{ (laid == &sc.moved) ? arranged
                                             : table.arrangement(*laid, memo.laid) };
      key = INVALID;
      use.entry = INVALID;
      if ((arranged == INVALID) || (at == INVALID)) { return false; }
      CandidateMemo::Linked const linked{ table.find_ordering(memo.row, at, sc.faces) };
      key = linked.key;
      return take(table.recall(linked.entry, labels));
    };
    if (recall_laid() || prune_at(stored_bound)) { return out; }
    // A turned ordering that does not size leaves the phase-1 ordering laid.
    std::vector<Diagnostic> spilled;
    bool ok{ sized || size_layout(c,
                                  g,
                                  *laid,
                                  s,
                                  row.knobs,
                                  cand.sized,
                                  spilled,
                                  row.dar,
                                  row.pack,
                                  row.fold) };
    if (!ok && (laid != &sc.moved)) {
      laid = &sc.moved;
      if (recall_laid() || prune_at(stored_bound)) { return out; }
      ok = size_layout(c,
                       g,
                       *laid,
                       s,
                       row.knobs,
                       cand.sized,
                       spilled,
                       row.dar,
                       row.pack,
                       row.fold);
    }
    if (!ok) {
      cand.routes = Routes{};
      cand.laid = SearchPins{};
      return out;
    }
    segment_bends(*laid, static_cast<uint32_t>(g.segments.size()), sc.reversed, sc.bends);
    bool const drawn{ recall_drawn(
        table.drawing(*laid, cand.sized, sc.bends, memo.profile, memo.drawing)) };
    table.link(key, use.entry);
    if (drawn || pruned_by(sc.bends, cand.sized)) { return out; }
  }
  MemoScore known;
  switch (table.claim(use.entry, labels, known)) {
    case Claim::Scored:
      use.deduped = true;
      use.labelled = labels;
      out = scored_from(known);
      return out;
    case Claim::Busy:
      if (memo.defer) {
        use.deferred = true;
        return out;
      }
      break;
    case Claim::Taken: break;
  }
  CostTerms floor{ bound };
  floor.bends = 0;
  RouteStop stop{ .bends = &shares,
                  .floor = cost_of(floor, objective).t2,
                  .per_bend = objective.w_bends,
                  .at = memo.incumbent.t2 };
  bool const staged{ stops && bounded && router.rectilinear() };
  lay_routes(cand,
             c,
             g,
             *laid,
             s,
             row,
             router,
             1,
             &pins,
             { .reuse = reuse, .stop = staged ? &stop : nullptr },
             labels);
  if (stop.stopped) {
    out = Scored{};
    out.cost = { .t0_violations = 0, .t1_hints = 0, .t2 = stop.reached };
    out.viable = true;
    out.stopped = true;
    use.stopped = true;
    table.release(use.entry, labels, stop.reached);
    return out;
  }
  out =
      scored_of(c, g, scoring, s, objective, cand, labels, stops ? memo.incumbent.t2 : -1);
#ifdef SCAV_TESTING
  if (test_route_bound_verify && bounded) {
    check_route_bound(c, g, scoring, s, objective, cand, bound, shares);
  }
#else
  (void)bounded;
#endif
  if (cand.retried) {
    table.set_retried(use.entry);
    use.entry = INVALID;
  } else if (out.stopped) {
    use.stopped = true;
    table.release(use.entry, labels, out.cost.t2);
  } else {
    table.set_score(use.entry, labels, memo_score(out));
  }
  if (routed != nullptr) { *routed = { .cand = &cand }; }
  return out;
}

// Scores `pins` through phases 1 to 3, or from `memo.table`, which leaves `routed` null;
// without `labels`, the bound. `from` supplies a face move's phases 1 and 2.
Scored score_move(Chart const &c,
                  SplitGraph const &g,
                  CostContext const &scoring,
                  scav_spaces const &s,
                  scav_profile const &objective,
                  Row const &row,
                  Router const &router,
                  SearchPins const &pins,
                  RouteCache const *reuse,
                  Prefix const *from,
                  bool labels,
                  Routed *routed,
                  MemoAccess const &memo,
                  MemoUse &use) {
  use = MemoUse{};
  if (routed != nullptr) { *routed = Routed{}; }
  MoveScratch &sc{ move_scratch() };
  auto const whole = [&]() -> Candidate const & {
    order_submachines(sc.moved, c, g, s, objective, 1, pins);
    std::vector<Diagnostic> spilled;
    search_candidate(sc.whole,
                     c,
                     g,
                     sc.moved,
                     s,
                     row,
                     router,
                     1,
                     spilled,
                     &pins,
                     { .reuse = reuse, .scratch = &sc.keep },
                     labels);
    return sc.whole;
  };
  // Under a trace sink every move is scored whole, tracing its phases.
  bool shortcut{ (from != nullptr) && (trace_sink() == nullptr) };
  bool memoized{ (memo.table != nullptr) && (trace_sink() == nullptr) };
#ifdef SCAV_TESTING
  shortcut = shortcut && test_prefix_shortcut;
  memoized = memoized && test_candidate_memo && !test_prefix_verify;
#endif
  if (memoized) {
    Scored const out{ score_memoized(c,
                                     g,
                                     scoring,
                                     s,
                                     objective,
                                     row,
                                     router,
                                     pins,
                                     reuse,
                                     shortcut ? from : nullptr,
                                     labels,
                                     routed,
                                     memo,
                                     use) };
#ifdef SCAV_TESTING
    if (shortcut) {
      ScopedLock const held{ test_prefix_lock };
      ++test_prefix_used;
    }
    {
      ScopedLock const held{ test_candidate_memo_lock };
      test_candidate_memo_deduped += use.deduped ? 1U : 0U;
      test_candidate_memo_drawn += use.drawn ? 1U : 0U;
      test_candidate_memo_faced += use.faced ? 1U : 0U;
    }
    if (use.pruned || use.stopped) {
      ScopedLock const held{ test_route_bound_lock };
      test_route_bound_pruned += use.pruned ? 1U : 0U;
      test_cost_stopped += use.stopped ? 1U : 0U;
    }
    if (test_route_bound_verify && use.stopped) {
      // A stopped move laid out whole scores at or above its bound.
      SubmachineOrders const moved{ order_submachines(c, g, s, objective, 1, pins) };
      std::vector<Diagnostic> spilled;
      Candidate fresh;
      search_candidate(fresh,
                       c,
                       g,
                       moved,
                       s,
                       row,
                       router,
                       1,
                       spilled,
                       &pins,
                       { .reuse = reuse },
                       labels);
      Scored const want{ scored_of(c, g, scoring, s, objective, fresh, labels) };
      bool const holds{ !want.viable || want.inflated || want.degraded ||
                        !cost_less(want.cost, out.cost) };
      ScopedLock const held{ test_route_bound_lock };
      ++test_route_bound_checked;
      test_route_bound_mismatches += holds ? 0U : 1U;
    }
    if (test_route_bound_verify && use.pruned) {
      // A pruned move laid out whole scores at or above its bound.
      SubmachineOrders const moved{ order_submachines(c, g, s, objective, 1, pins) };
      std::vector<Diagnostic> spilled;
      Candidate fresh;
      search_candidate(fresh,
                       c,
                       g,
                       moved,
                       s,
                       row,
                       router,
                       1,
                       spilled,
                       &pins,
                       { .reuse = reuse },
                       false);
      Scored const want{ scored_of(c, g, scoring, s, objective, fresh, false) };
      bool const holds{ !want.viable || want.inflated || want.degraded ||
                        !cost_less(want.cost, out.cost) };
      ScopedLock const held{ test_route_bound_lock };
      ++test_route_bound_checked;
      test_route_bound_mismatches += holds ? 0U : 1U;
    }
    if (test_candidate_memo_verify && !use.pruned && !use.stopped && !use.deferred &&
        (use.deduped || use.faced)) {
      SubmachineOrders const moved{ order_submachines(c, g, s, objective, 1, pins) };
      std::vector<Diagnostic> spilled;
      Candidate fresh;
      bool const labelled{ use.deduped ? use.labelled : labels };
      search_candidate(fresh,
                       c,
                       g,
                       moved,
                       s,
                       row,
                       router,
                       1,
                       spilled,
                       &pins,
                       { .reuse = reuse },
                       labelled);
      Scored const want{ scored_of(c, g, scoring, s, objective, fresh, labelled) };
      if ((want.viable != out.viable) || (want.inflated != out.inflated) ||
          (want.degraded != out.degraded) ||
          (want.viable && !want.inflated && !want.degraded &&
           !same_cost(want.cost, out.cost))) {
        ScopedLock const held{ test_candidate_memo_lock };
        ++test_candidate_memo_mismatches;
      }
    }
#endif
    return out;
  }
  if (!shortcut) {
    Scored const out{ scored_of(c, g, scoring, s, objective, whole(), labels) };
    if (routed != nullptr) { *routed = { .cand = &sc.whole }; }
    return out;
  }
  if (routed != nullptr) { *routed = { .cand = &sc.face }; }
  std::vector<Diagnostic> spilled;
  Candidate const &cand{ sc.face };
  search_candidate(sc.face,
                   c,
                   g,
                   from->laid,
                   s,
                   row,
                   router,
                   1,
                   spilled,
                   &pins,
                   { .reuse = reuse, .from = from, .scratch = &sc.keep },
                   labels);
  Scored const out{ scored_of(c, g, scoring, s, objective, cand, labels) };
#ifdef SCAV_TESTING
  {
    ScopedLock const held{ test_prefix_lock };
    ++test_prefix_used;
  }
  if (test_prefix_verify) {
    Candidate const &full{ whole() };
    Scored const want{ scored_of(c, g, scoring, s, objective, full, labels) };
    if (!same_scored(want, out) || !same_candidate(full, cand)) {
      ScopedLock const held{ test_prefix_lock };
      ++test_prefix_mismatches;
    }
  }
#endif
  return out;
}

#ifdef SCAV_TESTING
// Test switch: leave no-op faces unscored; the counter tallies those left.
bool test_skip_noop_faces{ true };
Mutex test_noop_lock;
std::atomic<uint64_t> test_noop_faces{ 0 };
#endif

// True when a face with no router effect may go unscored.
bool skipping_noop_faces() {
#ifdef SCAV_TESTING
  if (!test_skip_noop_faces) { return false; }
  ScopedLock const held{ test_noop_lock };
  ++test_noop_faces;
#endif
  return true;
}

#ifdef SCAV_TESTING
// Test switches: leave moves that change nothing unoffered; score each anyway against the
// incumbent. The counters tally those left, per move kind, and those that differ.
bool test_cull{ true };
bool test_cull_verify{ false };
Mutex test_cull_lock;
std::array<std::atomic<uint64_t>, TRACE_MOVES> test_culled{};
std::atomic<uint64_t> test_cull_mismatches{ 0 };
#endif

// True when a move of `kind` that changes nothing goes unoffered.
bool culling(MoveKind kind) {
#ifdef SCAV_TESTING
  if (!test_cull) { return false; }
  ScopedLock const held{ test_cull_lock };
  ++test_culled[static_cast<uint32_t>(kind)];
#else
  (void)kind;
#endif
  return true;
}

#ifdef SCAV_TESTING
// Test switches: score labelled rounds by bound, and verify them whole. Counters tally
// skipped and labelled candidates, and disagreeing rounds or candidates.
bool test_label_bound{ true };
bool test_label_bound_verify{ false };
Mutex test_label_bound_lock;
std::atomic<uint64_t> test_label_bound_skipped{ 0 };
std::atomic<uint64_t> test_label_bound_labelled{ 0 };
std::atomic<uint64_t> test_label_bound_mismatches{ 0 };
#endif

// True when a candidate's bound may still beat `incumbent`.
bool may_win(Scored const &bound, Cost const &incumbent) {
  return bound.viable && !bound.inflated && !bound.degraded &&
         cost_less(bound.cost, incumbent);
}

// A move's identity across rounds: kind, subject and parameters. Exact while ranks, legs
// and fold layers stay below 2^24.
uint64_t move_key(Move const &m) {
  uint64_t subject{ 0 };
  uint64_t param{ 0 };
  uint64_t end_face{ 0 };
  switch (m.kind) {
    case MoveKind::Rank:
      subject = m.pin.state.v;
      param = m.pin.rank;
      break;
    case MoveKind::Cut:
    case MoveKind::Reverse:
      subject = m.leg.trans.v;
      param = m.leg.leg;
      break;
    case MoveKind::Face:
    case MoveKind::Side:
      subject = m.end_pin.trans.v;
      param = m.end_pin.leg;
      end_face = (uint64_t{ m.end_pin.end } * 4U) + m.end_pin.face;
      break;
    case MoveKind::Fold:
      subject = m.fold.frame.v;
      param = (uint64_t{ m.fold.layer } * 4U) + m.fold.mode;
      break;
    case MoveKind::Orient: subject = m.orient.frame.v; break;
    case MoveKind::Loop:
      subject = m.loop.state.v;
      end_face = (uint64_t{ m.loop.end } * 4U) + m.loop.face;
      break;
  }
  return static_cast<uint64_t>(m.kind) | ((end_face & 7U) << 3U) |
         ((param & 0x3FF'FFFFU) << 6U) | (subject << 32U);
}

constexpr int64_t JITTER_PER_MILLE{ 2 };  // the jitter's span, per mille of Tier 2

// `JITTER_PER_MILLE` per mille of `incumbent`'s Tier 2, at least 1.
int64_t jitter_span(Cost const &incumbent) {
  return imax(int64_t{ 1 }, (incumbent.t2 * JITTER_PER_MILLE) / 1000);
}

// A hash of `seed` and `key` below `span`; 0 when `seed` is 0.
int64_t jitter(uint32_t seed, uint64_t key, int64_t span) {
  if (seed == 0) { return 0; }
  return static_cast<int64_t>(splitmix64(splitmix64(seed) + key) %
                              static_cast<uint64_t>(span));
}

// `c` with `jit` added to its Tier 2: what a jittered search ranks improving moves by.
Cost jittered(Cost c, int64_t jit) {
  c.t2 += jit;
  return c;
}

// Candidates per labelled round, least bound first, that keep their routes.
constexpr uint32_t KEPT_ROUTES{ 128 };

// Swaps `routed` (candidate `index`) into `kept` if among the `KEPT_ROUTES` least by
// (bound, index); the first `kept_n` slots are in use.
void keep_least(std::vector<KeptCandidate> &kept,
                uint32_t &kept_n,
                uint32_t index,
                uint32_t entry,
                Cost const &bound,
                Routed const &routed) {
  auto const before = [](Cost const &a, uint32_t ai, KeptCandidate const &b) {
    return cost_less(a, b.bound) || (!cost_less(b.bound, a) && (ai < b.index));
  };
  uint32_t slot{ kept_n };
  if (kept_n < KEPT_ROUTES) {
    if (kept_n == kept.size()) { vec_push_back(kept, KeptCandidate{}); }
    ++kept_n;
  } else {
    slot = 0;
    for (uint32_t k = 1; k < kept_n; ++k) {
      if (before(kept[slot].bound, kept[slot].index, kept[k])) { slot = k; }
    }
    if (!before(bound, index, kept[slot])) { return; }
  }
  KeptCandidate &k{ kept[slot] };
  std::swap(k.cand, *routed.cand);  // the thread's scratch takes the slot's storage
  k.bound = bound;
  k.index = index;
  k.entry = entry;
}

// True when cost `a` of candidate `i` beats `incumbent` and ranks before the pick `b` of
// candidate `pick` by cost plus jitter, ties to the lower index; INVALID `pick` is none.
bool ranks_before(Cost const &a,
                  int64_t a_jit,
                  uint32_t i,
                  Cost const &b,
                  int64_t b_jit,
                  uint32_t pick,
                  Cost const &incumbent) {
  if (!cost_less(a, incumbent)) { return false; }
  if (pick == INVALID) { return true; }
  Cost const x{ jittered(a, a_jit) };
  Cost const y{ jittered(b, b_jit) };
  return cost_less(x, y) || ((i < pick) && !cost_less(y, x));
}

// Scores `n` moves by `score(i, defer)` in parallel, deferring any whose drawing another
// thread is routing, then scores those `deferred(i)` names undeferred.
template <typename Score, typename Deferred>
void score_round(uint32_t n,
                 uint32_t threads,
                 std::vector<uint32_t> &again,
                 Score const &score,
                 Deferred const &deferred) {
  parallel_for(n, threads, [&](uint32_t i) { score(i, true); });
  again.clear();
  for (uint32_t i = 0; i < n; ++i) {
    if (deferred(i)) { vec_push_back(again, i); }
  }
  parallel_for(static_cast<uint32_t>(again.size()), threads, [&](uint32_t k) {
    score(again[k], false);
  });
}

// The improving candidate of least exact cost plus `jit` (lowest index among equals), else
// INVALID; `got` holds each bound, and `exact(i)` runs in bound order while one can win.
template <typename Exact>
uint32_t least_by_bound(uint32_t n,
                        Cost const &incumbent,
                        std::vector<int64_t> const &jit,
                        std::vector<Scored> &got,
                        std::vector<uint32_t> &order,
                        Exact const &exact) {
  order.clear();
  uint32_t open{ 0 };
  for (uint32_t i = 0; i < n; ++i) {
    if (!got[i].viable || got[i].inflated || got[i].degraded) { continue; }
    ++open;
    if (may_win(got[i], incumbent)) { vec_push_back(order, i); }
  }
  auto const jit_of = [&jit](uint32_t i) { return (i < jit.size()) ? jit[i] : 0; };
  scav_stable_sort(order, [&](uint32_t a, uint32_t b) {
    return cost_less(jittered(got[a].cost, jit_of(a)), jittered(got[b].cost, jit_of(b)));
  });
  uint32_t win{ INVALID };
  auto const beats = [&](uint32_t i) {
    Cost const &pick{ (win != INVALID) ? got[win].cost : incumbent };
    return ranks_before(got[i].cost, jit_of(i), i, pick, jit_of(win), win, incumbent);
  };
  uint32_t at{ 0 };
  for (; (at < order.size()) && beats(order[at]); ++at) {
    got[order[at]] = exact(order[at]);
    if (beats(order[at])) { win = order[at]; }
  }
#ifdef SCAV_TESTING
  {
    ScopedLock const held{ test_label_bound_lock };
    test_label_bound_skipped += open - at;
    test_label_bound_labelled += at;
  }
#else
  (void)open;
#endif
  return win;
}

#ifdef SCAV_TESTING
// Scores every candidate of a bounded round whole and tallies a round whose pick or its
// cost differs, or whose bound exceeds a whole score.
template <typename Score>
void verify_bounded_round(uint32_t n,
                          uint32_t threads,
                          Cost const &incumbent,
                          std::vector<int64_t> const &jit,
                          std::vector<Scored> const &got,
                          uint32_t win,
                          Score const &score) {
  std::vector<Scored> full(n);
  parallel_for(n, threads, [&](uint32_t i) { full[i] = score(i, true); });
  auto const jit_of = [&jit](uint32_t i) { return (i < jit.size()) ? jit[i] : 0; };
  uint32_t want{ INVALID };
  Cost best{ incumbent };
  bool same{ true };
  for (uint32_t i = 0; i < n; ++i) {
    same = same && (full[i].viable == got[i].viable) &&
           (full[i].inflated == got[i].inflated) && (full[i].degraded == got[i].degraded);
    if (!full[i].viable || full[i].inflated || full[i].degraded) { continue; }
    same = same && !cost_less(full[i].cost, got[i].cost);
    if (ranks_before(full[i].cost, jit_of(i), i, best, jit_of(want), want, incumbent)) {
      best = full[i].cost;
      want = i;
    }
  }
  same = same && (want == win);
  same = same && ((win == INVALID) ||
                  (!cost_less(got[win].cost, best) && !cost_less(best, got[win].cost)));
  if (!same) {
    ScopedLock const held{ test_label_bound_lock };
    ++test_label_bound_mismatches;
  }
}
#endif

#ifdef SCAV_TESTING
// Test switch: where a search stops, lay out each move of its last enumerated round whole;
// the counters tally those checked and those that beat the incumbent.
bool test_dont_look_verify{ false };
Mutex test_dont_look_lock;
std::atomic<uint64_t> test_dont_look_checked{ 0 };
std::atomic<uint64_t> test_dont_look_mismatches{ 0 };

// Test switch: score each bounded round's moves again after its labelling and set their
// bits again; the counters tally moves scored again and bits that differ.
bool test_bound_replay{ false };
std::atomic<uint64_t> test_bound_replayed{ 0 };
std::atomic<uint64_t> test_bound_replay_mismatches{ 0 };
#endif

// One layout's search counts, added to by its threads; a null `to` counts nothing.
struct RunStats {
  SearchStats *to{ nullptr };
  Mutex lock;
};

// Adds `add` to `run`'s counts; `memo_bytes` takes the larger.
void stats_add(RunStats *run, SearchStats const &add) {
  if ((run == nullptr) || (run->to == nullptr)) { return; }
  ScopedLock const held{ run->lock };
  SearchStats &to{ *run->to };
  for (uint32_t k = 0; k < TRACE_MOVES; ++k) {
    to.offered[k] += add.offered[k];
    to.deduped[k] += add.deduped[k];
    to.taken[k] += add.taken[k];
    to.culled[k] += add.culled[k];
    to.skipped[k] += add.skipped[k];
    to.pruned[k] += add.pruned[k];
    to.stopped[k] += add.stopped[k];
  }
  to.drawn += add.drawn;
  to.faced += add.faced;
  to.searches += add.searches;
  to.recalled += add.recalled;
  to.aliased += add.aliased;
  to.deferred += add.deferred;
  to.relaid += add.relaid;
  to.memo_bytes = imax(to.memo_bytes, add.memo_bytes);
}

// Suspends this thread's trace sink for its scope.
struct TraceMuted {
  LayoutTrace *const was{ trace_sink() };
  TraceMuted() { trace_sink_set(nullptr); }
  ~TraceMuted() { trace_sink_set(was); }
  TraceMuted(TraceMuted const &) = delete;
  TraceMuted &operator=(TraceMuted const &) = delete;
};

// Level 1: each round takes the cheapest strictly improving move, ties to enumeration
// order, offering `budget` moves per kind at most; culled, it skips moves its bits mark.
Improved run_search(Chart const &c,
                    SplitGraph const &g,
                    scav_spaces const &s,
                    scav_profile const &objective,
                    Row const &row,
                    Router const &router,
                    uint32_t threads,
                    uint32_t budget,
                    bool refold,
                    SearchPins const &seed,
                    std::vector<uint8_t> const *scope,
                    CandidateMemo *memo,
                    RunStats *stats) {
  Improved out;
  SearchStats counted;  // this search's counts, added to the sink on return
  counted.searches = 1;
  // `scope`: per-submachine flags, null for all; moves are offered only in flagged frames.
  auto const in_scope = [scope](uint32_t frame) {
    return (scope == nullptr) || ((frame < scope->size()) && ((*scope)[frame] != 0));
  };
  // `held` starts as `seed` and gains each move taken.
  SearchPins &held{ out.held };
  held = seed;
  SubmachineOrders here{ order_submachines(c, g, s, objective, threads, held) };

  // The incumbent's route cache: read by every candidate of a round, written between
  // rounds.
  RouteCache base;
  Prefix incumbent;
  {
    std::vector<Diagnostic> spilled;
    out.best = search_candidate(c,
                                g,
                                here,
                                s,
                                row,
                                router,
                                threads,
                                spilled,
                                &held,
                                { .fill = &base, .prefix = &incumbent });
  }
  out.viable = out.best.viable;
  if (!out.viable) {
    stats_add(stats, counted);
    return out;
  }
  CostContext const scoring{ cost_context(c, g) };
  std::vector<uint8_t> party;  // set where the incumbent's route bends or is charged
  out.cost = cost_of(
      cost_terms(scoring, c, g, out.best.sized, out.best.routes, s, objective, &party),
      objective);

  // The candidate memo, off under a trace sink; it holds each incumbent's score.
  MemoAccess access;
  if ((memo != nullptr) && (trace_sink() == nullptr)) {
    access.table = memo;
    access.row = memo->row_word(row);
    access.profile = memo->profile_word(row.knobs);
    access.prune = true;
#ifdef SCAV_TESTING
    access.prune = test_route_bound;
#endif
  }
  // The incumbent's encodings, which each candidate's frames are numbered against.
  CandidateMemo::Blocks ordered_blocks;
  CandidateMemo::Blocks laid_blocks;
  CandidateMemo::Blocks drawn_blocks;
  if (access.table != nullptr) {
    access.ordered = &ordered_blocks;
    access.laid = &laid_blocks;
    access.drawing = &drawn_blocks;
  }
  std::vector<uint32_t> held_faces;
  auto const remember_incumbent = [&]() {
    access.drawn = INVALID;
    if ((access.table == nullptr) || !incumbent.ok) { return; }
    CandidateMemo &table{ *access.table };
    (void)table.arrangement(here, nullptr, &ordered_blocks);
    uint32_t const arranged{ table.arrangement(incumbent.laid, nullptr, &laid_blocks) };
    access.drawn = table.drawing(incumbent.laid,
                                 incumbent.sized,
                                 access.profile,
                                 nullptr,
                                 &drawn_blocks);
    if ((access.drawn == INVALID) || out.best.retried) { return; }
    table.box_faces(&held, held_faces);
    CandidateMemo::Recalled const at{ table.find_score(access.drawn, held_faces, true) };
    table.set_score(
        at.entry,
        true,
        { .cost = out.cost, .viable = true, .degraded = out.best.routes.degraded() != 0 });
    if (arranged != INVALID) {
      table.link(table.find_ordering(access.row, arranged, held_faces).key, at.entry);
    }
  };
  remember_incumbent();

  // Copies `base_pins` plus `m` into the scoring thread's own pins.
  auto const with_here = [](SearchPins const &base_pins,
                            Move const &m) -> SearchPins const & {
    SearchPins &into{ move_scratch().pins };
    into = base_pins;  // copy-assignment keeps each vector's storage
    add_move(into, m);
    return into;
  };

  // Moves offered per kind, each capped at `budget`.
  uint32_t cut_scored{ 0 };
  uint32_t rev_scored{ 0 };
  uint32_t face_scored{ 0 };
  uint32_t side_scored{ 0 };
  uint32_t pin_scored{ 0 };
  uint32_t fold_scored{ 0 };
  uint32_t loop_scored{ 0 };
  std::vector<Move> round;
  std::vector<Scored> got;
  std::vector<MemoUse> uses;  // parallel to `round`: how each move's first scoring went
  std::vector<uint32_t> order;
  std::vector<uint32_t> again;  // a round's deferred moves
  // A labelled round's least-bound candidates, routed unlabelled; the first `kept_n` are
  // in use.
  std::vector<KeptCandidate> kept;
  uint32_t kept_n{ 0 };
  Mutex kept_lock;
  // A labelled round routes every candidate, then labels only those whose bound can still
  // win; a traced round scores each whole.
  bool const has_labels{ (s.path_box != nullptr) && (s.n_path_box != 0) };
  bool bounded{ has_labels && (trace_sink() == nullptr) };
#ifdef SCAV_TESTING
  bounded = bounded && test_label_bound;
#endif
  std::vector<uint8_t> chained;
  std::vector<uint32_t> pin_rank;  // per state: the rank its last pin in `held` names
#ifdef SCAV_TESTING
  std::vector<Move> culled;  // the round's unoffered moves, under `test_cull_verify`
#endif
  // True when `m`, which changes nothing, goes unoffered and uncharged.
  auto const cull = [&](Move const &m) {
    if (!culling(m.kind)) { return false; }
    ++counted.culled[static_cast<uint32_t>(m.kind)];
#ifdef SCAV_TESTING
    if (test_cull_verify) { vec_push_back(culled, m); }
#endif
    return true;
  };
  // Enumerates a round's moves into `round`, charging each to its kind's counter.
  auto const enumerate = [&]() {
    round.clear();

    // Cut moves: segments phase 1 chained through bends, less those already cut.
    vec_assign(chained, g.segments.size(), 0);
    for (OrderNode const &nd : here.nodes) {
      if ((nd.kind == OrderKind::Bend) && (nd.subject < chained.size())) {
        chained[nd.subject] = 1;
      }
    }
    for (ChainCut const &already : held.cuts) {
      if (already.trans.v >= g.trans_segments.size()) { continue; }
      Span const segs{ g.trans_segments[already.trans.v] };
      if (already.leg < segs.len) { chained[segs.off + already.leg] = 0; }
    }
    for (uint32_t seg = 0; (seg < chained.size()) && (cut_scored < budget); ++seg) {
      if ((chained[seg] == 0) || !in_scope(g.segments[seg].frame.v)) { continue; }
      TransId const t{ g.segments[seg].trans };
      if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
      ++cut_scored;
      vec_push_back(round,
                    { .leg = { .trans = t, .leg = seg - g.trans_segments[t.v].off },
                      .kind = MoveKind::Cut });
    }

    // Reverse moves: each cyclic segment without a reverse pin, and each initial's one-leg
    // edge either way; reversed, an initial follows its state.
    for (uint32_t seg = 0; (seg < g.segments.size()) && (rev_scored < budget); ++seg) {
      if (!in_scope(g.segments[seg].frame.v)) { continue; }
      TransId const t{ g.segments[seg].trans };
      if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
      bool const initial{ (g.trans_segments[t.v].len == 1) &&
                          (c.states[c.transitions[t.v].src.v].kind ==
                           StateKind::Initial) };
      if ((here.seg_cyclic[seg] == 0) && !initial) { continue; }
      uint32_t const leg{ seg - g.trans_segments[t.v].off };
      bool already{ false };
      for (ReversePin const &had : held.reverses) {
        already = already || ((had.trans.v == t.v) && (had.leg == leg));
      }
      if (already && !initial) { continue; }
      ++rev_scored;
      vec_push_back(round,
                    { .leg = { .trans = t, .leg = leg }, .kind = MoveKind::Reverse });
    }

    // End moves, skipping pinned ends: a box end to each face, a face with no router
    // effect counted unscored; a state-border port to each unlined side but its own.
    std::vector<uint8_t> const &faceable{ base.faceable };
    SubmachineOrders const &laid{ incumbent.laid };
    for (uint32_t seg = 0;
         (seg < g.segments.size()) && ((face_scored < budget) || (side_scored < budget));
         ++seg) {
      if (!in_scope(g.segments[seg].frame.v)) { continue; }
      TransId const t{ g.segments[seg].trans };
      if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
      uint32_t const leg{ seg - g.trans_segments[t.v].off };
      for (uint32_t end = 0; end < 2; ++end) {
        if (std::ranges::any_of(held.ends, [&](EndPin const &had) {
              return (had.trans.v == t.v) && (had.leg == leg) && (had.end == end);
            })) {
          continue;
        }
        uint32_t const port{ (end == 0) ? g.segments[seg].src_port
                                        : g.segments[seg].dst_port };
        if (port == INVALID) {
          bool const straight{ (t.v >= party.size()) || (party[t.v] == 0) };
          if (straight) { continue; }  // straight, unpriced
          size_t const at{ (size_t{ 2 } * seg) + end };
          uint32_t const effective{ (at < faceable.size()) ? uint32_t{ faceable[at] }
                                                           : 0xFU };
          for (uint32_t f = 0; (f < 4) && (face_scored < budget); ++f) {
            ++face_scored;
            if ((((effective >> f) & 1U) == 0) && skipping_noop_faces()) { continue; }
            vec_push_back(round,
                          { .end_pin = { .trans = t, .leg = leg, .end = end, .face = f },
                            .kind = MoveKind::Face });
          }
          continue;
        }
        if (!incumbent.ok || (port != laid.seg_port[seg]) ||
            (laid.seg_node[seg] == INVALID) || (g.ports[port].state.v == INVALID)) {
          continue;
        }
        uint32_t const now{ laid.seg_side[seg] };
        for (uint32_t side = 0; (side < 4) && (side_scored < budget); ++side) {
          if ((side == now) || face_lined(s, g.ports[port].state.v, side)) { continue; }
          ++side_scored;
          vec_push_back(round,
                        { .end_pin = { .trans = t, .leg = leg, .end = end, .face = side },
                          .kind = MoveKind::Side });
        }
      }
    }

    // Rank moves: each live, non-initial state to every other rank of its frame; the rank
    // its last pin names is culled.
    vec_assign(pin_rank, c.states.size(), INVALID);
    for (RankPin const &had : held.ranks) {
      if (had.state.v < pin_rank.size()) { pin_rank[had.state.v] = had.rank; }
    }
    for (uint32_t st = 0; (st < c.states.size()) && (pin_scored < budget); ++st) {
      if ((c.states[st].live == 0) || (here.state_node[st] == INVALID) ||
          (c.states[st].kind == StateKind::Initial)) {
        continue;
      }
      uint32_t const at{ here.nodes[here.state_node[st]].rank };
      uint32_t const frame{ c.states[st].parent.v };
      if ((frame >= here.sub_ranks.size()) || !in_scope(frame)) { continue; }
      uint32_t const ranks{ here.sub_ranks[frame] };
      for (uint32_t r = 0; (r < ranks) && (pin_scored < budget); ++r) {
        if (r == at) { continue; }
        Move const m{ .pin = { .state = StateId{ st }, .rank = r } };
        if ((r == pin_rank[st]) && cull(m)) { continue; }
        ++pin_scored;
        vec_push_back(round, m);
      }
    }
    // Fold moves under `refold`: a folded frame moves its cut before each other rank.
    std::vector<uint8_t> const &drawn_folded{ out.best.sized.folded };
    for (uint32_t m = 0; refold && (m < here.sub_ranks.size()); ++m) {
      if ((c.submachines[m].live == 0) || (here.sub_down[m] != 0) ||
          (m >= drawn_folded.size()) || (drawn_folded[m] == 0) || !in_scope(m)) {
        continue;
      }
      uint32_t const now{ here.sub_fold_cut[m] };
      for (uint32_t r = 1; (r < here.sub_ranks[m]) && (fold_scored < budget); ++r) {
        if (r == now) { continue; }
        ++fold_scored;
        vec_push_back(
            round,
            { .fold = { .frame = SubmachineId{ m }, .mode = FOLD_ALWAYS, .layer = r },
              .kind = MoveKind::Fold });
      }
    }
    // Loop moves: each state with a loop room to each other placement on an anchored face;
    // on an uninflated incumbent, a placement that leaves the room where it is is culled.
    std::vector<uint8_t> const &drawn_place{ out.best.sized.loop_place };
    std::vector<scav_rect> const &drawn_loop{ out.best.sized.loop };
    for (uint32_t st = 0; (st < drawn_loop.size()) && (loop_scored < budget); ++st) {
      if ((drawn_loop[st].w == 0) || (st >= drawn_place.size()) ||
          !in_scope(c.states[st].parent.v)) {
        continue;
      }
      for (uint32_t k = 0; (k < 8) && (loop_scored < budget); ++k) {
        if ((k == drawn_place[st]) || !loop_anchored(s, st, k / 2)) { continue; }
        Move const m{ .loop = { .state = StateId{ st }, .face = k / 2, .end = k % 2 },
                      .kind = MoveKind::Loop };
        if ((out.best.inflations == 0) && loop_room_unmoved(c, out.best.sized, st, k) &&
            cull(m)) {
          continue;
        }
        ++loop_scored;
        vec_push_back(round, m);
      }
    }
#ifdef SCAV_TESTING
    // Each culled move, laid out whole, draws and scores as the incumbent.
    parallel_for(static_cast<uint32_t>(culled.size()), threads, [&](uint32_t i) {
      SearchPins pins{ held };
      add_move(pins, culled[i]);
      SubmachineOrders const moved{ order_submachines(c, g, s, objective, 1, pins) };
      std::vector<Diagnostic> spilled;
      Candidate fresh;
      search_candidate(fresh,
                       c,
                       g,
                       moved,
                       s,
                       row,
                       router,
                       1,
                       spilled,
                       &pins,
                       { .reuse = &base },
                       true);
      Scored const want{ scored_of(c, g, scoring, s, objective, fresh) };
      if (!same_drawing(fresh, out.best) ||
          (!want.inflated && !same_cost(want.cost, out.cost))) {
        ScopedLock const lock{ test_cull_lock };
        ++test_cull_mismatches;
      }
    });
    culled.clear();
#endif
  };
  bool const culled_search{ objective.search_cull != 0 };
  // The culled search's don't-look bits: per move key, 1 where its last score could not
  // beat the incumbent then.
  HashMap<uint64_t, uint8_t> dont_look;
  std::vector<uint8_t> dont_look_now;  // parallel to `round`
  std::vector<Move> skipped;           // the round's moves its bits left unscored
  bool rescan{ false };                // the next pass scores `skipped`
  // What the last move taken changed: per frame, per transition, per state's extent.
  std::vector<uint8_t> frame_changed;
  std::vector<uint8_t> route_changed;
  std::vector<uint8_t> resized;
  // True when what `m` reads changed: its frame, a face or side move's route, and a loop
  // move's state extent.
  auto const touched = [&](Move const &m) {
    auto const flagged = [](std::vector<uint8_t> const &v, uint32_t i) {
      return (i >= v.size()) || (v[i] != 0);
    };
    auto const leg_frame = [&](TransId t, uint32_t leg) {
      if (t.v >= g.trans_segments.size()) { return INVALID; }
      Span const segs{ g.trans_segments[t.v] };
      return (leg < segs.len) ? g.segments[segs.off + leg].frame.v : INVALID;
    };
    switch (m.kind) {
      case MoveKind::Rank: return flagged(frame_changed, c.states[m.pin.state.v].parent.v);
      case MoveKind::Cut:
      case MoveKind::Reverse:
        return flagged(frame_changed, leg_frame(m.leg.trans, m.leg.leg));
      case MoveKind::Face:
      case MoveKind::Side: return flagged(route_changed, m.end_pin.trans.v);
      case MoveKind::Fold: return flagged(frame_changed, m.fold.frame.v);
      case MoveKind::Loop:
        return flagged(frame_changed, c.states[m.loop.state.v].parent.v) ||
               flagged(resized, m.loop.state.v);
      case MoveKind::Orient: break;
    }
    return true;
  };
  uint32_t const jitter_seed{ static_cast<uint32_t>(objective.jitter_seed) };
  std::vector<int64_t> jit;  // parallel to `round` under a jitter seed, else empty
#ifdef SCAV_TESTING
  std::vector<Move> whole;  // the last enumerated round, under `test_dont_look_verify`
  auto const verify_optimum = [&]() {
    if (!test_dont_look_verify) { return; }
    parallel_for(static_cast<uint32_t>(whole.size()), threads, [&](uint32_t i) {
      SearchPins pins{ held };
      add_move(pins, whole[i]);
      SubmachineOrders const moved{ order_submachines(c, g, s, objective, 1, pins) };
      std::vector<Diagnostic> spilled;
      Candidate fresh;
      search_candidate(fresh,
                       c,
                       g,
                       moved,
                       s,
                       row,
                       router,
                       1,
                       spilled,
                       &pins,
                       { .reuse = &base },
                       true);
      if (may_win(scored_of(c, g, scoring, s, objective, fresh), out.cost)) {
        ScopedLock const lock{ test_dont_look_lock };
        ++test_dont_look_mismatches;
      }
    });
    ScopedLock const lock{ test_dont_look_lock };
    test_dont_look_checked += whole.size();
  };
#endif
  while (rescan || (cut_scored < budget) || (rev_scored < budget) ||
         (face_scored < budget) || (side_scored < budget) || (pin_scored < budget) ||
         (loop_scored < budget) || (refold && (fold_scored < budget))) {
    // Enumerates, scores in parallel and reduces in order; a round that takes nothing
    // after its bits skipped moves is followed by a round of just those.
    if (rescan) {
      round.swap(skipped);
      skipped.clear();
      rescan = false;
    } else {
      enumerate();
#ifdef SCAV_TESTING
      if (test_dont_look_verify) { vec_assign(whole, round.begin(), round.end()); }
#endif
      skipped.clear();
      uint32_t looked{ 0 };
      for (uint32_t i = 0; culled_search && (i < round.size()); ++i) {
        uint8_t const *const bit{ dont_look.find(move_key(round[i])) };
        if ((bit != nullptr) && (*bit != 0) && !touched(round[i])) {
          vec_push_back(skipped, round[i]);
        } else {
          round[looked++] = round[i];
        }
      }
      if (culled_search) { vec_resize(round, looked); }
    }
    if (round.empty()) {
      if (skipped.empty()) {
#ifdef SCAV_TESTING
        verify_optimum();
#endif
        break;
      }
      rescan = true;
      continue;
    }

    access.incumbent = out.cost;
    // Scores move `i` on this thread: `fresh` without the memo, `defer` leaving it
    // unscored while another thread routes its drawing.
    auto const score = [&](uint32_t i,
                           bool labels,
                           MemoUse &use,
                           Routed *routed,
                           bool fresh = false,
                           bool defer = false) {
      MemoAccess met{ fresh ? MemoAccess{} : access };
      met.defer = defer;
      return score_move(c,
                        g,
                        scoring,
                        s,
                        objective,
                        row,
                        router,
                        with_here(held, round[i]),
                        &base,
                        (round[i].kind == MoveKind::Face) ? &incumbent : nullptr,
                        labels,
                        routed,
                        met,
                        use);
    };
    auto const deferred = [&](uint32_t i) {
      counted.deferred += uses[i].deferred ? 1U : 0U;
      return uses[i].deferred;
    };
    // 1 when move `i`'s unlabelled bound cannot beat the incumbent. `known` is that bound,
    // or where `labelled` a labelled score; one that cannot win has the bound laid out.
    auto const bound_idle =
        [&](uint32_t i, Scored const &known, bool labelled, MemoUse &use) -> uint8_t {
      if (may_win(known, out.cost)) { return 0; }
      if (!labelled) { return 1; }
      TraceMuted const quiet;
      MemoUse fresh;
      Scored const bound{ score(i, false, fresh, nullptr, true) };
      if (access.table != nullptr) {
        access.table->set_score(use.entry, false, memo_score(bound));
      }
      use.deduped = false;
      use.relaid = true;
      return may_win(bound, out.cost) ? 0U : 1U;
    };
    uint32_t const n{ static_cast<uint32_t>(round.size()) };
    vec_assign(got, n, {});
    vec_assign(uses, n, {});
    if (culled_search) { vec_assign(dont_look_now, n, uint8_t{ 0 }); }
    jit.clear();
    for (uint32_t i = 0; (jitter_seed != 0) && (i < n); ++i) {
      vec_push_back(jit, jitter(jitter_seed, move_key(round[i]), jitter_span(out.cost)));
    }
    auto const jit_of = [&jit](uint32_t i) { return (i < jit.size()) ? jit[i] : 0; };
    uint32_t win{ INVALID };
    if (bounded) {
      // The first candidates in bound order are labelled on kept routes, their own or
      // those of their score entry; any after them are laid out again.
#ifdef SCAV_TESTING
      uint32_t calls{ 0 };
#endif
      auto const bound = [&](uint32_t i, bool defer) {
        Routed routed;
        got[i] = score(i, false, uses[i], &routed, false, defer);
        Scored const &scored{ got[i] };
        if (uses[i].deferred) { return; }
        if (culled_search) {
          bool const labelled{ uses[i].deduped && uses[i].labelled };
          dont_look_now[i] = bound_idle(i, scored, labelled, uses[i]);
        }
        if (may_win(scored, out.cost) && (routed.cand != nullptr)) {
          ScopedLock const lock{ kept_lock };
          keep_least(kept, kept_n, i, uses[i].entry, scored.cost, routed);
        }
      };
      auto const exact = [&](uint32_t i) {
#ifdef SCAV_TESTING
        ++calls;
#endif
        uint32_t const e{ uses[i].entry };
        MemoScore known;
        if ((access.table != nullptr) && access.table->score(e, true, known)) {
          return scored_from(known);
        }
        uint32_t k{ 0 };
        while ((k < kept_n) && (kept[k].index != i) &&
               ((e == INVALID) || (kept[k].entry != e))) {
          ++k;
        }
        if (k == kept_n) {
#ifdef SCAV_TESTING
          // A candidate the bound pass routed is kept while within `KEPT_ROUTES`.
          if ((calls <= KEPT_ROUTES) && !uses[i].deduped) {
            ScopedLock const lock{ test_label_bound_lock };
            ++test_label_bound_mismatches;
          }
#endif
          MemoUse use;
          return score(i, true, use, nullptr);
        }
        Candidate &cand{ kept[k].cand };
        label_candidate(cand, c, g, s, row.knobs);
        Scored const scored{ scored_of(c, g, scoring, s, objective, cand) };
        if (access.table != nullptr) {
          access.table->set_score(e, true, memo_score(scored));
        }
#ifdef SCAV_TESTING
        if (test_label_bound_verify) {
          Routed full;
          MemoUse use;
          Scored const want{ score(i, true, use, &full, true) };
          bool const twin{ kept[k].index != i };
          if (!same_scored(scored, want) || (!twin && !same_candidate(cand, *full.cand))) {
            ScopedLock const lock{ test_label_bound_lock };
            ++test_label_bound_mismatches;
          }
        }
#endif
        return scored;
      };
      score_round(n, threads, again, bound, deferred);
#ifdef SCAV_TESTING
      if ((test_candidate_memo_empty != 0) && (access.table != nullptr)) {
        bool due{ false };
        {
          ScopedLock const locked{ test_candidate_memo_lock };
          due = ((++test_candidate_memo_labellings) % test_candidate_memo_empty) == 0;
        }
        if (due) { access.table->empty(); }
      }
#endif
      win = least_by_bound(n, out.cost, jit, got, order, exact);
      kept_n = 0;
#ifdef SCAV_TESTING
      for (uint32_t i = 0; test_bound_replay && culled_search && (i < n); ++i) {
        MemoUse use;
        Scored const rescored{ score(i, false, use, nullptr) };
        uint8_t const bit{ bound_idle(i, rescored, use.deduped && use.labelled, use) };
        ScopedLock const locked{ test_dont_look_lock };
        ++test_bound_replayed;
        test_bound_replay_mismatches += (bit != dont_look_now[i]) ? 1U : 0U;
      }
      if (test_label_bound_verify) {
        verify_bounded_round(n,
                             threads,
                             out.cost,
                             jit,
                             got,
                             win,
                             [&](uint32_t i, bool labels) {
                               MemoUse use;
                               return score(i, labels, use, nullptr, true);
                             });
      }
#endif
    } else {
      auto const labelled = [&](uint32_t i, bool defer) {
        got[i] = score(i, true, uses[i], nullptr, false, defer);
        if (culled_search && !uses[i].deferred) {
          dont_look_now[i] = bound_idle(i, got[i], has_labels, uses[i]);
        }
      };
      score_round(n, threads, again, labelled, deferred);
    }
    for (uint32_t i = 0; i < n; ++i) {
      auto const kind{ static_cast<uint32_t>(round[i].kind) };
      ++counted.offered[kind];
      counted.deduped[kind] += uses[i].deduped ? 1U : 0U;
      counted.pruned[kind] += uses[i].pruned ? 1U : 0U;
      counted.stopped[kind] += uses[i].stopped ? 1U : 0U;
      counted.drawn += uses[i].drawn ? 1U : 0U;
      counted.faced += uses[i].faced ? 1U : 0U;
      counted.relaid += uses[i].relaid ? 1U : 0U;
    }

    // Reduces in enumeration order and emits the trace in that order.
    Move take{};
    Cost best{ out.cost };
    uint32_t took{ win };
    bool found{ false };
    if (win != INVALID) {
      take = round[win];
      best = got[win].cost;
      found = true;
    }
    for (uint32_t i = 0; !bounded && (i < n); ++i) {
      Move const &m{ round[i] };
      Scored const &sc{ got[i] };
      MoveVerdict verdict{ MoveVerdict::NotBetter };
      if (!sc.viable) {
        verdict = MoveVerdict::NotViable;
      } else if (sc.inflated) {
        // A move whose lay-out needed inflated spacing or degraded a net is never taken.
        verdict = MoveVerdict::Inflated;
      } else if (sc.degraded) {
        verdict = MoveVerdict::Degraded;
      } else if (ranks_before(sc.cost, jit_of(i), i, best, jit_of(took), took, out.cost)) {
        verdict = MoveVerdict::Taken;
        best = sc.cost;
        take = m;
        took = i;
        found = true;
      }
      uint32_t moved_trans{ INVALID };
      uint32_t moved_leg{ 0 };
      if ((m.kind == MoveKind::Face) || (m.kind == MoveKind::Side)) {
        moved_trans = m.end_pin.trans.v;
        moved_leg = m.end_pin.leg;
      } else if ((m.kind != MoveKind::Rank) && (m.kind != MoveKind::Loop)) {
        moved_trans = m.leg.trans.v;
        moved_leg = m.leg.leg;
      }
      bool const refolded{ m.kind == MoveKind::Fold };
      bool const placed{ m.kind == MoveKind::Loop };
      uint32_t moved_rank{ refolded ? m.fold.layer : 0U };
      if (m.kind == MoveKind::Rank) { moved_rank = m.pin.rank; }
      uint32_t moved_state{ (m.kind == MoveKind::Rank) ? m.pin.state.v : INVALID };
      moved_state = placed ? m.loop.state.v : moved_state;
      trace_emit(
          { .kind = TraceKind::CandidateScored,
            .pass = static_cast<uint16_t>(verdict),
            .frame = refolded ? m.fold.frame.v : INVALID,
            .score = { .row = INVALID,
                       .state = moved_state,
                       .rank = moved_rank,
                       .trans = moved_trans,
                       .leg = moved_leg,
                       .move = static_cast<uint16_t>(m.kind),
                       .end = static_cast<uint16_t>(placed ? m.loop.end : m.end_pin.end),
                       .face = placed ? m.loop.face : m.end_pin.face,
                       .t0 = sc.viable ? sc.cost.t0_violations : 0,
                       .t2 = sc.viable ? sc.cost.t2 : 0 } });
      // Under a trace sink, a viable move's term shares follow its score.
      if ((trace_sink() != nullptr) && sc.viable) {
        TraceEvent e{ .kind = TraceKind::CandidateTerms, .terms = {} };
        for (uint32_t k = 0; k < TIER2_TERMS; ++k) { e.terms.share[k] = sc.share[k]; }
        trace_emit(e);
      }
    }

    for (uint32_t i = 0; culled_search && (i < n); ++i) {
      uint64_t const key{ move_key(round[i]) };
      uint8_t *const bit{ dont_look.find(key) };
      if (bit != nullptr) {
        *bit = dont_look_now[i];
      } else {
        dont_look.insert(key, dont_look_now[i]);
      }
    }
    if (!found) {
      if (skipped.empty()) {
#ifdef SCAV_TESTING
        verify_optimum();
#endif
        break;
      }
      rescan = true;
      continue;
    }
    for (Move const &m : skipped) { ++counted.skipped[static_cast<uint32_t>(m.kind)]; }
    skipped.clear();
    ++counted.taken[static_cast<uint32_t>(take.kind)];
    add_move(held, take);
    order_submachines(here, c, g, s, objective, threads, held);
    // Re-lays the new incumbent from `held`, reusing the routes of every frame the move
    // left unchanged or only shifted.
    RouteCache const was{ std::move(base) };
    base = RouteCache{};
    Candidate prior;
    if (culled_search) { prior = std::move(out.best); }
    std::vector<Diagnostic> spilled;
    out.best = search_candidate(c,
                                g,
                                here,
                                s,
                                row,
                                router,
                                threads,
                                spilled,
                                &held,
                                { .reuse = &was, .fill = &base, .prefix = &incumbent });
    out.cost = best;
#ifdef SCAV_TESTING
    if (out.best.routes.degraded() != 0) {
      ScopedLock const locked{ test_degrade_lock };
      ++test_taken_degraded;
    }
#endif
    (void)cost_terms(scoring, c, g, out.best.sized, out.best.routes, s, objective, &party);
    remember_incumbent();
    if (culled_search) {
      search_changes(c,
                     prior.sized,
                     prior.routes,
                     out.best.sized,
                     out.best.routes,
                     frame_changed,
                     route_changed,
                     resized);
    }
  }
  stats_add(stats, counted);
  return out;
}

// The inverse of `put_pins`, reading from `at` and advancing it.
SearchPins get_pins(int32_t const *w, uint32_t &at) {
  auto const next = [&]() { return static_cast<uint32_t>(w[at++]); };
  SearchPins p;
  vec_resize(p.ranks, next());
  for (RankPin &r : p.ranks) { r = { .state = StateId{ next() }, .rank = next() }; }
  vec_resize(p.cuts, next());
  for (ChainCut &k : p.cuts) { k = { .trans = TransId{ next() }, .leg = next() }; }
  vec_resize(p.reverses, next());
  for (ReversePin &r : p.reverses) { r = { .trans = TransId{ next() }, .leg = next() }; }
  vec_resize(p.ends, next());
  for (EndPin &e : p.ends) {
    e = { .trans = TransId{ next() }, .leg = next(), .end = next(), .face = next() };
  }
  vec_resize(p.orients, next());
  for (OrientPin &o : p.orients) { o = { .frame = SubmachineId{ next() } }; }
  vec_resize(p.folds, next());
  for (FoldPin &f : p.folds) {
    f = { .frame = SubmachineId{ next() }, .mode = next(), .layer = next() };
  }
  vec_resize(p.loops, next());
  for (LoopPin &l : p.loops) {
    l = { .state = StateId{ next() }, .face = next(), .end = next() };
  }
  return p;
}

// Deletes the candidate memo it holds on scope exit.
struct HeldMemo {
  CandidateMemo *const memo;
  explicit HeldMemo(CandidateMemo *m) : memo(m) {}
  HeldMemo(HeldMemo const &) = delete;
  HeldMemo &operator=(HeldMemo const &) = delete;
  ~HeldMemo() { delete memo; }
};

// A layout's search results by `search_key`, shared across threads; `lock` guards each
// lookup or insert.
struct SearchMemo {
  Mutex lock;
  Memo table{ size_t{ 1 } << 24 };
};

#ifdef SCAV_TESTING
// Test switches: run searches through the memo, and verify each hit against a fresh
// search; the counters tally hits and mismatches.
bool test_search_memo{ true };
bool test_search_memo_verify{ false };
bool test_no_search{ false };  // forces a zero move budget
bool test_row_alias{ true };   // rows whose canonical rows match search once
std::atomic<uint32_t> test_search_memo_hits{ 0 };
std::atomic<uint32_t> test_search_memo_mismatches{ 0 };
// Per row of this thread's last searched layout: each schedule's cost, and the one kept.
thread_local std::vector<Cost> test_schedule_first, test_schedule_second,
    test_schedule_kept;

bool same_result(Improved const &a, Improved const &b) {
  return (a.viable == b.viable) && same_cost(a.cost, b.cost) &&
         same_pins(a.held, b.held) && same_candidate(a.best, b.best);
}
#endif

// `run_search` through `memo`: a hit restores the reached pins and cost and re-lays the
// drawing from the pins. A traced run always searches.
Improved search_moves(Chart const &c,
                      SplitGraph const &g,
                      scav_spaces const &s,
                      scav_profile const &objective,
                      Row const &row,
                      Router const &router,
                      uint32_t threads,
                      uint32_t budget,
                      bool refold,
                      SearchPins const &seed,
                      std::vector<uint8_t> const *scope,
                      SearchMemo *memo,
                      CandidateMemo *candidates,
                      RunStats *stats) {
  auto const search = [&]() {
    return run_search(c,
                      g,
                      s,
                      objective,
                      row,
                      router,
                      threads,
                      budget,
                      refold,
                      seed,
                      scope,
                      candidates,
                      stats);
  };
  if ((memo == nullptr) || (trace_sink() != nullptr)) { return search(); }
  std::vector<uint32_t> key;
  search_key(objective, row, budget, refold, seed, scope, key);
  std::vector<int32_t> value;
  bool hit{ false };
  {
    ScopedLock const held{ memo->lock };
    int32_t const *at{ nullptr };
    uint32_t len{ 0 };
    hit = memo->table.find(key, at, len);
    if (hit) { vec_assign(value, at, at + len); }
#ifdef SCAV_TESTING
    test_search_memo_hits += hit ? 1U : 0U;
#endif
  }
  if (hit) {
    SearchStats one;
    one.recalled = 1;
    stats_add(stats, one);
    Improved out;
    uint32_t at{ 0 };
    out.viable = value[at++] != 0;
    out.cost.t0_violations = value[at++];
    uint32_t const t1_hi{ static_cast<uint32_t>(value[at++]) };
    uint32_t const t1_lo{ static_cast<uint32_t>(value[at++]) };
    uint32_t const t2_hi{ static_cast<uint32_t>(value[at++]) };
    uint32_t const t2_lo{ static_cast<uint32_t>(value[at++]) };
    out.cost.t1_hints = static_cast<int64_t>((uint64_t{ t1_hi } << 32U) | t1_lo);
    out.cost.t2 = static_cast<int64_t>((uint64_t{ t2_hi } << 32U) | t2_lo);
    out.held = get_pins(value.data(), at);
    SubmachineOrders const here{
      order_submachines(c, g, s, objective, threads, out.held)
    };
    std::vector<Diagnostic> spilled;
    out.best = search_candidate(c, g, here, s, row, router, threads, spilled, &out.held);
#ifdef SCAV_TESTING
    if (test_search_memo_verify && !same_result(out, search())) {
      ScopedLock const held{ memo->lock };
      ++test_search_memo_mismatches;
    }
#endif
    return out;
  }
  Improved out{ search() };
  std::vector<uint32_t> words{
    out.viable ? 1U : 0U,
    static_cast<uint32_t>(out.cost.t0_violations),
    static_cast<uint32_t>(static_cast<uint64_t>(out.cost.t1_hints) >> 32U),
    static_cast<uint32_t>(static_cast<uint64_t>(out.cost.t1_hints)),
    static_cast<uint32_t>(static_cast<uint64_t>(out.cost.t2) >> 32U),
    static_cast<uint32_t>(static_cast<uint64_t>(out.cost.t2))
  };
  put_pins(out.held, words);
  vec_assign(value, words.begin(), words.end());
  {
    ScopedLock const held{ memo->lock };
    int32_t const *at{ nullptr };
    uint32_t len{ 0 };
    if (!memo->table.find(key, at, len)) { memo->table.insert(key, value); }
  }
  return out;
}

// A search event for Level 2 row `row` reaching `cost`, naming no kick.
TraceEvent search_event(TraceKind kind, uint16_t pass, uint32_t row, Cost const &cost) {
  return { .kind = kind,
           .pass = pass,
           .search = { .row = row,
                       .of = INVALID,
                       .move = 0,
                       .trans = INVALID,
                       .leg = 0,
                       .t0 = cost.t0_violations,
                       .framed_t0 = 0,
                       .t2 = cost.t2,
                       .framed = 0 } };
}

// Flags each frame that encloses a frame `frames` flags and is not flagged itself.
std::vector<uint8_t> enclosing_frames(Chart const &c, std::vector<uint8_t> const &frames) {
  std::vector<uint8_t> out(frames.size(), 0);
  for (uint32_t m = 0; m < frames.size(); ++m) {
    if (frames[m] == 0) { continue; }
    uint32_t at{ m };
    while ((at < c.submachines.size()) && (c.submachines[at].owner.v < c.states.size())) {
      at = c.states[c.submachines[at].owner.v].parent.v;
      if ((at < out.size()) && (frames[at] == 0)) { out[at] = 1; }
    }
  }
  return out;
}

}  // namespace

SCAV_INTERNAL_BEGIN

// Rows searched: `portfolio_m`, clamped to [1, LAYOUT_SEARCH_ROWS].
uint32_t search_tuple_count(scav_profile const &p) {
  return imin(static_cast<uint32_t>(imax(p.portfolio_m, 1)), LAYOUT_SEARCH_ROWS);
}

uint32_t search_move_budget(scav_profile const &p) {
  return static_cast<uint32_t>(imax(p.portfolio_k, 0));
}

constexpr uint32_t ROW_COMPACTION{ 2 };  // the row index bit that sets compaction

// The table rows a search lays out: the first `portfolio_m`, less the compaction rows
// under the culled search.
void search_table(scav_profile const &p, std::vector<uint32_t> &rows) {
  rows.clear();
  for (uint32_t i = 0; i < search_tuple_count(p); ++i) {
    if ((p.search_cull == 0) || ((i & ROW_COMPACTION) == 0)) { vec_push_back(rows, i); }
  }
}

// Row `index` as a delta from `p`: bit 0 flips the box packer, bit 1 sets compaction,
// bit 2 the owner's hole, bit 3 always-fold; row 0 is `p`'s own tuple.
Row search_row(scav_profile const &p, uint32_t index) {
  Row out{ .knobs = p };
  out.knobs.trybox ^= static_cast<int32_t>(index & 1U);
  out.pack = (((index >> 1U) & 1U) != 0) ? Compaction::On : Compaction::Off;
  out.dar = (((index >> 2U) & 1U) != 0) ? DarSource::OwnerHole : DarSource::Profile;
  out.fold = (((index >> 3U) & 1U) != 0) ? Fold::Always : Fold::Scale;
  return out;
}

void kick_order(std::vector<uint32_t> &rest,
                std::vector<Cost> const &cost,
                std::vector<int64_t> const &jit) {
  scav_insertion_sort(rest.data(), rest.data() + rest.size(), [&](uint32_t a, uint32_t b) {
    return cost_less(jittered(cost[a], jit[a]), jittered(cost[b], jit[b]));
  });
}

// The viable row of least cost, lowest index among equals; 0 when none is viable.
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

void search_key(scav_profile const &objective,
                Row const &row,
                uint32_t budget,
                bool refold,
                SearchPins const &seed,
                std::vector<uint8_t> const *scope,
                std::vector<uint32_t> &key) {
  static_assert((sizeof(scav_profile) % sizeof(uint32_t)) == 0);
  std::array<uint32_t, sizeof(scav_profile) / sizeof(uint32_t)> words{};
  key.clear();
  for (scav_profile const *const at : { &objective, &row.knobs }) {
    std::memcpy(words.data(), at, sizeof(scav_profile));
    vec_insert(key, key.end(), words.begin(), words.end());
  }
  vec_insert(key,
             key.end(),
             { static_cast<uint32_t>(row.dar),
               static_cast<uint32_t>(row.pack),
               static_cast<uint32_t>(row.fold),
               budget,
               refold ? 1U : 0U });
  put_pins(seed, key);
  vec_push_back(key, (scope == nullptr) ? 0U : 1U);
  if (scope != nullptr) {
    vec_push_back(key, static_cast<uint32_t>(scope->size()));
    for (uint8_t const f : *scope) { vec_push_back(key, f); }
  }
}

// Flags what differs between two incumbents in chart coordinates: per frame, a state or
// loop room in it; per transition, its points; per state, its extent.
void search_changes(Chart const &c,
                    SizedLayout const &was,
                    Routes const &was_routes,
                    SizedLayout const &now,
                    Routes const &now_routes,
                    std::vector<uint8_t> &frame,
                    std::vector<uint8_t> &route,
                    std::vector<uint8_t> &resized) {
  auto const at = [](std::vector<scav_rect> const &v, uint32_t i) {
    return (i < v.size()) ? v[i] : scav_rect{};
  };
  vec_assign(frame, c.submachines.size(), uint8_t{ 0 });
  vec_assign(resized, c.states.size(), uint8_t{ 0 });
  for (uint32_t st = 0; st < c.states.size(); ++st) {
    scav_rect const a{ at(was.state, st) };
    scav_rect const b{ at(now.state, st) };
    resized[st] = ((a.w != b.w) || (a.h != b.h)) ? 1U : 0U;
    uint32_t const f{ c.states[st].parent.v };
    if ((f < frame.size()) &&
        (!same_rect(a, b) || !same_rect(at(was.loop, st), at(now.loop, st)))) {
      frame[f] = 1;
    }
  }
  auto const span = [](Routes const &r, uint32_t t) {
    return (t < r.route.size()) ? r.route[t] : scav_span{};
  };
  vec_assign(route, c.transitions.size(), uint8_t{ 0 });
  for (uint32_t t = 0; t < route.size(); ++t) {
    scav_span const a{ span(was_routes, t) };
    scav_span const b{ span(now_routes, t) };
    bool same{ a.len == b.len };
    for (uint32_t k = 0; same && (k < a.len); ++k) {
      scav_point const p{ was_routes.points[a.off + k] };
      scav_point const q{ now_routes.points[b.off + k] };
      same = (p.x == q.x) && (p.y == q.y);
    }
    route[t] = same ? 0U : 1U;
  }
}

SCAV_INTERNAL_END

bool layout_run(Chart &c,
                scav_spaces const &s,
                scav_layout_opts const &o,
                std::vector<scav_placed> &placed,
                std::vector<Diagnostic> &diags,
                uint32_t *inflations,
                uint32_t *tuple,
                uint32_t row,
                uint32_t *moves,
                SearchPins *taken,
                SearchPins const *pins,
                SearchStats *stats) {
  MemoRun const scope;
  RunStats counts;
  counts.to = stats;
  if (inflations != nullptr) { *inflations = 0; }
  if (tuple != nullptr) { *tuple = 0; }
  scav_profile const &p{ o.profile };
  if (!profile_validate(p)) {
    vec_push_back(diags,
                  { .code = DiagCode::ProfileOutOfRange,
                    .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                    .doc = { INVALID },
                    .src = {} });
    return false;
  }
  if (!spaces_validate(c, s, diags)) { return false; }

  if (geom_column_clash(c) != GeomCount) {
    vec_push_back(diags,
                  { .code = DiagCode::GeometryColumnClash,
                    .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                    .doc = { INVALID },
                    .src = {} });
    return false;
  }

  Router const *const router{ router_at(o.router) };
  if (router == nullptr) {
    vec_push_back(diags,
                  { .code = DiagCode::RouterUnknown,
                    .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                    .doc = { INVALID },
                    .src = {} });
    return false;
  }

  SplitGraph const g{ decompose(c) };
  // Phase 1 for the seed pins, once; every Level 2 row starts from this ordering.
  SearchPins const seed{ (pins != nullptr) ? *pins : SearchPins{} };
  SubmachineOrders const orders{ order_submachines(c, g, s, p, o.threads, seed) };

  // Level 2: lays out each admitted row; a pinned `row` is a table of one. `table` holds
  // each searched row's index in the table.
  bool const pinned{ row != INVALID };
  std::vector<uint32_t> table;
  if (pinned) {
    vec_push_back(table, row);
  } else {
    search_table(p, table);
  }
  auto const rows{ static_cast<uint32_t>(table.size()) };
  // A row's canonical form takes row 0's values for knobs no sizing reads; a row whose
  // form matches an earlier row's lays out and searches as that row, its `alias`.
  RowReads reads{ size_row_reads(c, orders) };
#ifdef SCAV_TESTING
  if (!test_row_alias) {
    reads = { .trybox = true, .pack = true, .dar = true, .fold = true };
  }
#endif
  std::vector<Row> canonical(rows);
  std::vector<uint32_t> alias(rows);
  SearchStats aliased;
  for (uint32_t i = 0; i < rows; ++i) {
    canonical[i] = size_row_canonical(search_row(p, table[i]), reads, p);
    alias[i] = i;
    for (uint32_t j = 0; (j < i) && (alias[i] == i); ++j) {
      if (same_row(canonical[j], canonical[i])) { alias[i] = j; }
    }
    aliased.aliased += (alias[i] != i) ? 1U : 0U;
  }
  stats_add(&counts, aliased);
  auto const row_of = [&](uint32_t i) { return canonical[i]; };
  bool const culled_search{ p.search_cull != 0 };
  std::vector<Candidate> candidates(rows);
  std::vector<Cost> cost(rows);
  std::vector<uint8_t> viable(rows, 0);
  // Rows run in parallel on one thread each; a lone row takes the caller's threads.
  std::vector<std::vector<Diagnostic>> spilled(rows);
  CostContext const scoring{ cost_context(c, g) };
  parallel_for(rows, (rows > 1) ? o.threads : 1U, [&](uint32_t i) {
    if (alias[i] != i) { return; }
    candidates[i] = search_candidate(c,
                                     g,
                                     orders,
                                     s,
                                     row_of(i),
                                     *router,
                                     (rows > 1) ? 1U : o.threads,
                                     spilled[i],
                                     &seed);
    viable[i] = candidates[i].viable ? 1U : 0U;
    // Scores each viable row under the caller's profile `p`.
    if (viable[i] != 0) {
      CostTerms const t{
        cost_terms(scoring, c, g, candidates[i].sized, candidates[i].routes, s, p)
      };
      cost[i] = cost_of(t, p);
    }
  });
  for (uint32_t i = 0; i < rows; ++i) {
    if (alias[i] == i) { continue; }
    candidates[i] = candidates[alias[i]];
    cost[i] = cost[alias[i]];
    viable[i] = viable[alias[i]];
  }
  // Only row 0's findings reach `diags`; row 0 is `p`'s own tuple or the pinned row.
  vec_insert(diags, diags.end(), spilled[0].begin(), spilled[0].end());
  // A row leaving the coordinate domain is unviable; an unviable row 0 fails the run.
  if (!candidates[0].viable) { return false; }

  uint32_t budget{ search_move_budget(p) };
#ifdef SCAV_TESTING
  if (test_no_search) { budget = 0; }
#endif
  std::vector<SearchPins> held(rows, seed);
  SearchMemo memo;
  SearchMemo *memo_at{ &memo };
  // Every search of this layout scores its moves through one candidate memo, on the heap.
  uint64_t memo_budget{ CandidateMemo::BUDGET };
  bool memoized{ budget != 0 };
#ifdef SCAV_TESTING
  memo_budget = test_candidate_memo_budget;
  memoized = memoized && test_candidate_memo;
  if (!test_search_memo) { memo_at = nullptr; }
#endif
  HeldMemo const move_memo{ memoized ? new CandidateMemo{ c, g, memo_budget } : nullptr };
  CandidateMemo *const move_memo_at{ move_memo.memo };
#ifdef SCAV_TESTING
  if (memoized) {
    ScopedLock const held_lock{ test_candidate_memo_lock };
    ++test_candidate_memos_built;
  }
  test_search_memo_hits = 0;
  test_search_memo_mismatches = 0;
#endif
  // Searches each viable row `which` flags from its `held` pins, once per alias and pins;
  // a viable result replaces the row's candidate, cost and pins.
  auto const search_rows = [&](std::vector<uint8_t> const &which, bool refold) {
    std::vector<uint32_t> active;
    std::vector<uint32_t> twin(rows, INVALID);  // per row, the `active` slot it repeats
    for (uint32_t i = 0; i < rows; ++i) {
      if ((viable[i] == 0) || (which[i] == 0)) { continue; }
      for (uint32_t k = 0; (k < active.size()) && (twin[i] == INVALID); ++k) {
        uint32_t const j{ active[k] };
        if ((alias[j] == alias[i]) && same_pins(held[j], held[i])) { twin[i] = k; }
      }
      if (twin[i] == INVALID) {
        twin[i] = static_cast<uint32_t>(active.size());
        vec_push_back(active, i);
      }
    }
    std::vector<Improved> done(active.size());
    uint32_t const n{ static_cast<uint32_t>(active.size()) };
    parallel_for(n, o.threads, [&](uint32_t k) {
      done[k] = search_moves(c,
                             g,
                             s,
                             p,
                             row_of(active[k]),
                             *router,
                             o.threads,
                             budget,
                             refold,
                             held[active[k]],
                             nullptr,
                             memo_at,
                             move_memo_at,
                             &counts);
    });
    for (uint32_t i = 0; i < rows; ++i) {
      if ((twin[i] == INVALID) || !done[twin[i]].viable) { continue; }
      Improved const &got{ done[twin[i]] };
      RowPass const pass{ refold ? RowPass::Refold : RowPass::First };
      trace_outline_emit(search_event(TraceKind::RowSearched,
                                      static_cast<uint16_t>(pass),
                                      table[i],
                                      got.cost));
      candidates[i] = got.best;
      cost[i] = got.cost;
      held[i] = got.held;
    }
  };

  // Level 1: searches every viable row to convergence; rows rank by what each reaches.
  if (budget != 0) { search_rows(std::vector<uint8_t>(rows, 1), false); }
  std::vector<uint8_t> const &eligible{ viable };

  // Iterated local search from row `best`: each kick restarts a search, kept where it
  // converges below the incumbent; winning kicks in distinct frames combine.
  auto const kick = [&](uint32_t best) {
    Row const best_row{ row_of(best) };
    // Searches row `best` from `start`, its moves confined to `within`'s frames if given.
    auto const search_from = [&](SearchPins const &start,
                                 std::vector<uint8_t> const *within) {
      return search_moves(c,
                          g,
                          s,
                          p,
                          best_row,
                          *router,
                          o.threads,
                          budget,
                          false,
                          start,
                          within,
                          memo_at,
                          move_memo_at,
                          &counts);
    };
    // Searches `start` in the `redo` frames, then in turn in the frames enclosing them and
    // in the `redo` frames again, until a search improves nothing. `framed` gets the first
    // search's cost.
    auto const search_kicked =
        [&](SearchPins const &start, std::vector<uint8_t> const &redo, Cost &framed) {
          std::vector<uint8_t> const around{ enclosing_frames(c, redo) };
          Improved out{ search_from(start, &redo) };
          framed = out.cost;
          bool outward{ true };
          while (out.viable) {
            Improved next{ search_from(out.held, outward ? &around : &redo) };
            if (!next.viable || !cost_less(next.cost, out.cost)) { break; }
            out = std::move(next);
            outward = !outward;
          }
          return out;
        };
    auto const frame_of_leg = [&](TransId t, uint32_t leg) {
      if (t.v >= g.trans_segments.size()) { return INVALID; }
      Span const segs{ g.trans_segments[t.v] };
      return (leg < segs.len) ? g.segments[segs.off + leg].frame.v : INVALID;
    };
    auto const outside = [&](uint32_t frame, std::vector<uint8_t> const &redo) {
      return (frame >= redo.size()) || (redo[frame] == 0);
    };
    // `from` less every rank, cut, end, fold and loop pin in a `redo` frame; orientations
    // and reversals stay.
    auto const warm = [&](SearchPins const &from, std::vector<uint8_t> const &redo) {
      SearchPins out;
      out.reverses = from.reverses;
      out.orients = from.orients;
      for (EndPin const &e : from.ends) {
        if (outside(frame_of_leg(e.trans, e.leg), redo)) { vec_push_back(out.ends, e); }
      }
      for (RankPin const &r : from.ranks) {
        uint32_t const f{ (r.state.v < c.states.size()) ? c.states[r.state.v].parent.v
                                                        : INVALID };
        if (outside(f, redo)) { vec_push_back(out.ranks, r); }
      }
      for (ChainCut const &k : from.cuts) {
        if (outside(frame_of_leg(k.trans, k.leg), redo)) { vec_push_back(out.cuts, k); }
      }
      for (FoldPin const &fp : from.folds) {
        if (outside(fp.frame.v, redo)) { vec_push_back(out.folds, fp); }
      }
      for (LoopPin const &lp : from.loops) {
        uint32_t const f{ (lp.state.v < c.states.size()) ? c.states[lp.state.v].parent.v
                                                         : INVALID };
        if (outside(f, redo)) { vec_push_back(out.loops, lp); }
      }
      return out;
    };
    auto const take = [&](Improved &&won, KickHow how) {
      trace_outline_emit(search_event(TraceKind::KickTaken,
                                      static_cast<uint16_t>(how),
                                      table[best],
                                      won.cost));
      candidates[best] = std::move(won.best);
      cost[best] = won.cost;
      held[best] = std::move(won.held);
    };
    // Rounds of reversal, orient and fold kicks until none improves, then one settling
    // search.
    bool kicked{ false };
    uint32_t kick_scored{ 0 };  // kicks offered, capped at `budget`
    for (;;) {
      SubmachineOrders const here{ order_submachines(c, g, s, p, o.threads, held[best]) };
      std::vector<uint8_t> turned(g.segments.size(), 0);
      for (OrderEdge const &e : here.edges) {
        if ((e.reversed != 0) && (e.segment < turned.size())) { turned[e.segment] = 1; }
      }
      // Reversal kicks: each cyclic segment phase 1 left unreversed.
      std::vector<Move> kicks;
      std::vector<uint32_t> kick_frame;
      for (uint32_t seg = 0; seg < g.segments.size(); ++seg) {
        if ((here.seg_cyclic[seg] == 0) || (turned[seg] != 0)) { continue; }
        TransId const t{ g.segments[seg].trans };
        if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
        uint32_t const leg{ seg - g.trans_segments[t.v].off };
        if (std::ranges::any_of(held[best].reverses, [t, leg](ReversePin const &had) {
              return (had.trans.v == t.v) && (had.leg == leg);
            })) {
          continue;  // pinned already; a repeat pin is a no-op
        }
        if (kick_scored >= budget) { break; }
        ++kick_scored;
        vec_push_back(kicks,
                      { .leg = { .trans = t, .leg = leg }, .kind = MoveKind::Reverse });
        vec_push_back(kick_frame, g.segments[seg].frame.v);
      }
      for (uint32_t m = 0; m < here.sub_ranks.size(); ++m) {
        if ((c.submachines[m].live == 0) || (here.sub_ranks[m] < 2) ||
            (here.sub_down[m] != 0) || (kick_scored >= budget)) {
          continue;
        }
        ++kick_scored;
        vec_push_back(
            kicks,
            { .orient = { .frame = SubmachineId{ m } }, .kind = MoveKind::Orient });
        vec_push_back(kick_frame, m);
      }
      // Fold kicks: each across-page frame without a fold pin flips its drawn fold.
      std::vector<uint8_t> const &folded{ candidates[best].sized.folded };
      for (uint32_t m = 0; m < here.sub_ranks.size(); ++m) {
        bool const fold_pinned{ std::ranges::any_of(
            held[best].folds,
            [m](FoldPin const &had) { return had.frame.v == m; }) };
        if ((c.submachines[m].live == 0) || (here.sub_ranks[m] < 2) ||
            (here.sub_down[m] != 0) || (m >= folded.size()) || fold_pinned ||
            (kick_scored >= budget)) {
          continue;
        }
        ++kick_scored;
        uint32_t const mode{ (folded[m] != 0) ? FOLD_NEVER : FOLD_ALWAYS };
        vec_push_back(kicks,
                      { .fold = { .frame = SubmachineId{ m }, .mode = mode },
                        .kind = MoveKind::Fold });
        vec_push_back(kick_frame, m);
      }
      if (kicks.empty()) { break; }

      std::vector<Improved> tried(kicks.size());
      std::vector<Cost> framed(kicks.size());
      parallel_for(static_cast<uint32_t>(kicks.size()), o.threads, [&](uint32_t j) {
        std::vector<uint8_t> redo(c.submachines.size(), 0);
        if (kick_frame[j] < redo.size()) { redo[kick_frame[j]] = 1; }
        SearchPins start{ warm(held[best], redo) };
        add_move(start, kicks[j]);
        tried[j] = search_kicked(start, redo, framed[j]);
      });
      for (uint32_t j = 0; j < tried.size(); ++j) {
        KickVerdict verdict{ KickVerdict::NotViable };
        if (tried[j].viable) {
          verdict = cost_less(tried[j].cost, cost[best]) ? KickVerdict::Improves
                                                         : KickVerdict::NotBetter;
        }
        TraceEvent e{ search_event(TraceKind::KickScored,
                                   static_cast<uint16_t>(verdict),
                                   table[best],
                                   tried[j].cost) };
        e.frame = kick_frame[j];
        e.search.move = static_cast<uint16_t>(kicks[j].kind);
        e.search.trans =
            (kicks[j].kind == MoveKind::Reverse) ? kicks[j].leg.trans.v : INVALID;
        e.search.leg = kicks[j].leg.leg;
        e.search.framed_t0 = framed[j].t0_violations;
        e.search.framed = framed[j].t2;
        trace_outline_emit(e);
      }

      // Each frame's best improving kick, and the best overall, by cost plus jitter; ties
      // go to enumeration order.
      std::vector<int64_t> jit(kicks.size(), 0);
      for (uint32_t j = 0; (p.jitter_seed != 0) && (j < kicks.size()); ++j) {
        jit[j] = jitter(static_cast<uint32_t>(p.jitter_seed),
                        move_key(kicks[j]),
                        jitter_span(cost[best]));
      }
      auto const before = [&](uint32_t j, uint32_t pick) {
        return ranks_before(tried[j].cost,
                            jit[j],
                            j,
                            (pick != INVALID) ? tried[pick].cost : cost[best],
                            (pick != INVALID) ? jit[pick] : 0,
                            pick,
                            cost[best]);
      };
      std::vector<uint32_t> in_frame(c.submachines.size(), INVALID);
      uint32_t single{ INVALID };
      for (uint32_t j = 0; j < tried.size(); ++j) {
        if (!tried[j].viable || !cost_less(tried[j].cost, cost[best])) { continue; }
        uint32_t const f{ kick_frame[j] };
        if ((f < in_frame.size()) && before(j, in_frame[f])) { in_frame[f] = j; }
        if (before(j, single)) { single = j; }
      }
      if (single == INVALID) { break; }

      std::vector<uint32_t> winners;
      for (uint32_t const j : in_frame) {
        if (j != INVALID) { vec_push_back(winners, j); }
      }
      if (winners.size() > 1) {
        std::vector<uint8_t> redo(c.submachines.size(), 0);
        for (uint32_t const j : winners) {
          if (kick_frame[j] < redo.size()) { redo[kick_frame[j]] = 1; }
        }
        SearchPins start{ warm(held[best], redo) };
        for (uint32_t const j : winners) { add_move(start, kicks[j]); }
        Cost together_framed;
        Improved together{ search_kicked(start, redo, together_framed) };
        if (together.viable && cost_less(together.cost, tried[single].cost)) {
          take(std::move(together), KickHow::Together);
          kicked = true;
          continue;
        }
      }
      take(std::move(tried[single]), KickHow::Single);
      kicked = true;
      // The other frames' best kicks, cheapest first, each searched on top of the
      // round's taken pins and kept where it still improves.
      std::vector<uint32_t> rest;
      for (uint32_t const j : winners) {
        if (j != single) { vec_push_back(rest, j); }
      }
      std::vector<Cost> tried_cost(tried.size());
      for (uint32_t j = 0; j < tried.size(); ++j) { tried_cost[j] = tried[j].cost; }
      kick_order(rest, tried_cost, jit);
      for (uint32_t const j : rest) {
        std::vector<uint8_t> redo(c.submachines.size(), 0);
        if (kick_frame[j] < redo.size()) { redo[kick_frame[j]] = 1; }
        SearchPins start{ warm(held[best], redo) };
        add_move(start, kicks[j]);
        Cost more_framed;
        Improved more{ search_kicked(start, redo, more_framed) };
        if (more.viable && cost_less(more.cost, cost[best])) {
          take(std::move(more), KickHow::Stacked);
        }
      }
    }
    // After any kick, one unscoped search from the kicked pins, kept where it improves.
    if (kicked) {
      Improved settled{ search_from(held[best], nullptr) };
      if (settled.viable && cost_less(settled.cost, cost[best])) {
        take(std::move(settled), KickHow::Settled);
      }
    }
  };
  // Rows kick in parallel, each writing only its own slots; a row whose converged drawing
  // matches an earlier row's is skipped.
  auto const same_drawing = [&](uint32_t a, uint32_t b) {
    return (cost[a].t0_violations == cost[b].t0_violations) &&
           (cost[a].t2 == cost[b].t2) && same_geometry(candidates[a], candidates[b]);
  };
  if (budget != 0) {
    // Repeats are found before any kick changes a row's drawing.
    std::vector<uint8_t> repeat(rows, 0);
    for (uint32_t i = 0; i < rows; ++i) {
      for (uint32_t j = 0; (j < i) && (viable[i] != 0) && (repeat[i] == 0); ++j) {
        if ((viable[j] != 0) && (repeat[j] == 0) && same_drawing(i, j)) {
          repeat[i] = 1;
          TraceEvent e{ search_event(TraceKind::RowRepeated, 0, table[i], cost[i]) };
          e.search.of = table[j];
          trace_outline_emit(e);
        }
      }
    }
    std::vector<uint32_t> kicking;
    for (uint32_t i = 0; i < rows; ++i) {
      if ((viable[i] != 0) && (repeat[i] == 0)) { vec_push_back(kicking, i); }
    }
    // The culled search kicks the `kick_rows` cheapest, ties to the lower row.
    auto const kick_rows{ static_cast<uint32_t>(imax(p.kick_rows, 1)) };
    if (culled_search && (kicking.size() > kick_rows)) {
      scav_stable_sort(kicking, [&](uint32_t a, uint32_t b) {
        return cost_less(cost[a], cost[b]);
      });
      vec_resize(kicking, kick_rows);
      scav_stable_sort(kicking, [](uint32_t a, uint32_t b) { return a < b; });
    }
    parallel_for(static_cast<uint32_t>(kicking.size()), o.threads, [&](uint32_t k) {
      kick(kicking[k]);
    });
    // A second search from each row's converged pins adds the fold moves; the cheaper is
    // kept, ties to the first. The culled search refolds only the rows that repeat none.
    std::vector<Candidate> first{ candidates };
    std::vector<Cost> const first_cost{ cost };
    std::vector<SearchPins> first_held{ held };
    std::vector<uint8_t> refolding(rows, 1);
    for (uint32_t i = 0; culled_search && (i < rows); ++i) {
      refolding[i] = (repeat[i] != 0) ? 0U : 1U;
    }
    search_rows(refolding, true);
#ifdef SCAV_TESTING
    test_schedule_first = first_cost;
    test_schedule_second = cost;
#endif
    for (uint32_t i = 0; i < rows; ++i) {
      if ((viable[i] == 0) || cost_less(cost[i], first_cost[i])) { continue; }
      candidates[i] = std::move(first[i]);
      cost[i] = first_cost[i];
      held[i] = std::move(first_held[i]);
    }
#ifdef SCAV_TESTING
    test_schedule_kept = cost;
#endif
  }
  uint32_t const best{ search_argmin(cost, eligible) };
  // `moves`: the winning row's pin count beyond the seed's, floored at 0.
  if (moves != nullptr) {
    auto const count = [](SearchPins const &q) {
      return static_cast<uint32_t>(q.ranks.size() + q.cuts.size() + q.reverses.size() +
                                   q.ends.size() + q.orients.size() + q.folds.size() +
                                   q.loops.size());
    };
    uint32_t const now{ count(held[best]) };
    uint32_t const had{ count(seed) };
    *moves = (now > had) ? (now - had) : 0U;
  }
  // `taken`: the winning pins, with the reversals and sides of its laid-out drawing.
  if (taken != nullptr) {
    *taken = held[best];
    taken->reverses = candidates[best].laid.reverses;
    taken->ends = candidates[best].laid.ends;
    // Each port end the facing pass would turn on a re-lay from `taken` keeps its side.
    SubmachineOrders const drawn{ order_submachines(c, g, s, p, o.threads, *taken) };
    Facing again;
    std::vector<FacingTaken> seats;
    facing_flips(again, seats, c, g, drawn, candidates[best].sized, s);
    for (EndPin const &e : again.sides) {
      if (e.trans.v >= g.trans_segments.size()) { continue; }
      Span const segs{ g.trans_segments[e.trans.v] };
      if (e.leg >= segs.len) { continue; }
      vec_push_back(taken->ends,
                    { .trans = e.trans,
                      .leg = e.leg,
                      .end = e.end,
                      .face = drawn.seg_side[segs.off + e.leg] });
    }
  }

  SizedLayout sized{ std::move(candidates[best].sized) };
  Routes routes{ std::move(candidates[best].routes) };
  if (inflations != nullptr) { *inflations = candidates[best].inflations; }
  if (tuple != nullptr) { *tuple = table[best]; }
  placed = routes.placed;

  // `failed` is parallel to the transitions; findings come out in ordinal order.
  for (uint32_t t = 0; t < routes.failed.size(); ++t) {
    if (routes.failed[t] == 0) { continue; }
    vec_push_back(diags,
                  { .code = DiagCode::RouteDegraded,
                    .subject = { .kind = ElemKind::Transition, .ordinal = t },
                    .doc = { INVALID },
                    .src = {} });
  }

  write_columns(c, sized, routes, inputs_digest(s, o));
  SearchStats held_bytes;
  held_bytes.memo_bytes = (move_memo_at != nullptr) ? move_memo_at->peak_bytes() : 0;
  stats_add(&counts, held_bytes);
  return true;
}

namespace {

// A copy of the named column's rows; empty when the column is absent.
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

void layout_occupied_spans(Chart const &c,
                           scav_profile const &p,
                           std::vector<OccupiedSpan> &out) {
  out.clear();
  auto const state{ rows_of<scav_rect>(c, "scav.geom.state") };
  auto const route{ rows_of<scav_span>(c, "scav.geom.route") };
  auto const point{ rows_of<scav_point>(c, "scav.geom.point") };
  for (uint32_t t = 0; t < route.size(); ++t) {
    if ((t >= c.transitions.size()) || (c.transitions[t].live == 0) ||
        (route[t].len < 2) || !inner_loop(c, t)) {
      continue;
    }
    uint32_t const st{ c.transitions[t].src.v };
    if ((st >= state.size()) || ((route[t].off + route[t].len) > point.size())) {
      continue;
    }
    loop_occupied({ point[route[t].off], point[route[t].off + route[t].len - 1] },
                  state[st],
                  st,
                  route_clearance(p),
                  out);
  }
}

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
                            "scav.geom.state_lead",
                            "scav.geom.state_trail",
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

bool layout_search_stats(Chart &c,
                         scav_spaces const &s,
                         scav_layout_opts const &o,
                         std::vector<scav_placed> &placed,
                         std::vector<Diagnostic> &diags,
                         std::vector<char> &out,
                         uint32_t row,
                         SearchPins const *pins,
                         uint32_t *tuple,
                         SearchPins *taken) {
  SearchStats counted;
  bool const ran{
    layout_run(c, s, o, placed, diags, nullptr, tuple, row, nullptr, taken, pins, &counted)
  };
  search_stats_to_json(counted, out);
  return ran;
}

bool layout_trace(Chart &c,
                  scav_spaces const &s,
                  scav_layout_opts const &o,
                  std::vector<scav_placed> &placed,
                  std::vector<Diagnostic> &diags,
                  TraceChunk sink,
                  void *ctx,
                  bool &streamed,
                  uint32_t row,
                  TraceScope scope,
                  SearchPins const *pins) {
  streamed = false;
  if (sink == nullptr) { return false; }
  LayoutTrace trace;
  trace.sink = sink;
  trace.ctx = ctx;
  trace_begin(trace, c);
  scav_layout_opts serial{ o };
  serial.threads = 1;
  bool laid{ false };
  if (scope != TraceScope::Shipped) {
    if (scope == TraceScope::Outline) {
      trace_outline_set(&trace);
    } else {
      trace_sink_set(&trace);
    }
    laid = layout_run(c,
                      s,
                      serial,
                      placed,
                      diags,
                      nullptr,
                      nullptr,
                      row,
                      nullptr,
                      nullptr,
                      pins);
    trace_outline_set(nullptr);
    trace_sink_set(nullptr);
  } else {
    // Searches first for the winning row and pins, then re-lays only those on one thread
    // with a zero move budget, traced.
    uint32_t won{ 0 };
    SearchPins taken;
    if (layout_run(c, s, o, placed, diags, nullptr, &won, row, nullptr, &taken, pins)) {
      serial.profile.portfolio_k = 0;
      trace_sink_set(&trace);
      std::vector<Diagnostic> again;
      laid = layout_run(c,
                        s,
                        serial,
                        placed,
                        again,
                        nullptr,
                        nullptr,
                        won,
                        nullptr,
                        nullptr,
                        &taken);
      trace_sink_set(nullptr);
    }
  }
  streamed = trace_end(trace);
  return laid;
}

uint32_t layout_structural_hash(Chart const &c) {
  std::vector<scav_byte> b;
  std::vector<scav_point> const points{ rows_of<scav_point>(c, "scav.geom.point") };
  for (scav_span const route : rows_of<scav_span>(c, "scav.geom.route")) {
    append_u32(b, route.len);
    // Direction tokens per step; invariant under translation.
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

#ifdef SCAV_TESTING
void layout_test_prefix_shortcut(bool on) {
  test_prefix_shortcut = on;
  ScopedLock const held{ test_prefix_lock };
  test_prefix_used = 0;
  test_prefix_mismatches = 0;
}
void layout_test_prefix_verify(bool on) {
  test_prefix_verify = on;
  ScopedLock const held{ test_prefix_lock };
  test_prefix_used = 0;
  test_prefix_mismatches = 0;
}
uint64_t layout_test_prefix_used() {
  ScopedLock const held{ test_prefix_lock };
  return test_prefix_used;
}
uint64_t layout_test_prefix_mismatches() {
  ScopedLock const held{ test_prefix_lock };
  return test_prefix_mismatches;
}
void layout_test_skip_noop_faces(bool on) {
  test_skip_noop_faces = on;
  ScopedLock const held{ test_noop_lock };
  test_noop_faces = 0;
}
uint64_t layout_test_noop_faces() {
  ScopedLock const held{ test_noop_lock };
  return test_noop_faces;
}
void layout_test_cull(bool on, bool verify) {
  test_cull = on;
  test_cull_verify = verify;
  ScopedLock const held{ test_cull_lock };
  for (std::atomic<uint64_t> &k : test_culled) { k = 0; }
  test_cull_mismatches = 0;
}
std::array<uint64_t, TRACE_MOVES> layout_test_culled() {
  ScopedLock const held{ test_cull_lock };
  std::array<uint64_t, TRACE_MOVES> out{};
  for (uint32_t k = 0; k < TRACE_MOVES; ++k) { out[k] = test_culled[k]; }
  return out;
}
uint64_t layout_test_cull_mismatches() {
  ScopedLock const held{ test_cull_lock };
  return test_cull_mismatches;
}
void layout_test_label_bound(bool on, bool verify) {
  test_label_bound = on;
  test_label_bound_verify = verify;
  ScopedLock const held{ test_label_bound_lock };
  test_label_bound_skipped = 0;
  test_label_bound_labelled = 0;
  test_label_bound_mismatches = 0;
}
uint64_t layout_test_label_bound_skipped() {
  ScopedLock const held{ test_label_bound_lock };
  return test_label_bound_skipped;
}
uint64_t layout_test_label_bound_labelled() {
  ScopedLock const held{ test_label_bound_lock };
  return test_label_bound_labelled;
}
uint64_t layout_test_label_bound_mismatches() {
  ScopedLock const held{ test_label_bound_lock };
  return test_label_bound_mismatches;
}
void layout_test_search_memo(bool on) { test_search_memo = on; }
void layout_test_candidate_memo_budget(uint64_t bytes) {
  test_candidate_memo_budget = bytes;
}
uint32_t layout_test_candidate_memos_built() {
  ScopedLock const held{ test_candidate_memo_lock };
  uint32_t const out{ test_candidate_memos_built };
  test_candidate_memos_built = 0;
  return out;
}
void layout_test_candidate_memo_empty(uint32_t every) {
  ScopedLock const held{ test_candidate_memo_lock };
  test_candidate_memo_empty = every;
  test_candidate_memo_labellings = 0;
}
void layout_test_candidate_memo(bool on, bool verify) {
  test_candidate_memo = on;
  test_candidate_memo_verify = verify;
  ScopedLock const held{ test_candidate_memo_lock };
  test_candidate_memo_deduped = 0;
  test_candidate_memo_drawn = 0;
  test_candidate_memo_faced = 0;
  test_candidate_memo_mismatches = 0;
}
uint64_t layout_test_candidate_memo_deduped() {
  ScopedLock const held{ test_candidate_memo_lock };
  return test_candidate_memo_deduped;
}
uint64_t layout_test_candidate_memo_drawn() {
  ScopedLock const held{ test_candidate_memo_lock };
  return test_candidate_memo_drawn;
}
uint64_t layout_test_candidate_memo_faced() {
  ScopedLock const held{ test_candidate_memo_lock };
  return test_candidate_memo_faced;
}
uint64_t layout_test_candidate_memo_mismatches() {
  ScopedLock const held{ test_candidate_memo_lock };
  return test_candidate_memo_mismatches;
}
void layout_test_route_bound(bool on, bool verify) {
  test_route_bound = on;
  test_route_bound_verify = verify;
  test_route_bound_pruned = 0;
  test_route_bound_checked = 0;
  test_route_bound_mismatches = 0;
}
uint64_t layout_test_route_bound_pruned() {
  ScopedLock const held{ test_route_bound_lock };
  return test_route_bound_pruned;
}
uint64_t layout_test_route_bound_checked() {
  ScopedLock const held{ test_route_bound_lock };
  return test_route_bound_checked;
}
uint64_t layout_test_route_bound_mismatches() {
  ScopedLock const held{ test_route_bound_lock };
  return test_route_bound_mismatches;
}
void layout_test_cost_stop(bool on) {
  test_cost_stop = on;
  ScopedLock const held{ test_route_bound_lock };
  test_cost_stopped = 0;
}
uint64_t layout_test_cost_stopped() {
  ScopedLock const held{ test_route_bound_lock };
  return test_cost_stopped;
}
void layout_test_search_memo_verify(bool on) { test_search_memo_verify = on; }
void layout_test_no_search(bool on) { test_no_search = on; }
void layout_test_row_alias(bool on) { test_row_alias = on; }
uint32_t layout_test_search_memo_hits() { return test_search_memo_hits; }
uint32_t layout_test_search_memo_mismatches() { return test_search_memo_mismatches; }
std::vector<Cost> const &layout_test_schedule_first() { return test_schedule_first; }
std::vector<Cost> const &layout_test_schedule_second() { return test_schedule_second; }
std::vector<Cost> const &layout_test_schedule_kept() { return test_schedule_kept; }
void layout_test_dont_look_verify(bool on) {
  ScopedLock const held{ test_dont_look_lock };
  test_dont_look_verify = on;
  test_dont_look_checked = 0;
  test_dont_look_mismatches = 0;
}
uint64_t layout_test_dont_look_checked() {
  ScopedLock const held{ test_dont_look_lock };
  return test_dont_look_checked;
}
uint64_t layout_test_dont_look_mismatches() {
  ScopedLock const held{ test_dont_look_lock };
  return test_dont_look_mismatches;
}
void layout_test_degrade(bool on) {
  ScopedLock const held{ test_degrade_lock };
  test_degrade = on;
  test_degrade_drawing.clear();
  test_degraded = 0;
  test_taken_degraded = 0;
}
uint64_t layout_test_degraded() {
  ScopedLock const held{ test_degrade_lock };
  return test_degraded;
}
uint64_t layout_test_taken_degraded() {
  ScopedLock const held{ test_degrade_lock };
  return test_taken_degraded;
}
void layout_test_bound_replay(bool on) {
  ScopedLock const held{ test_dont_look_lock };
  test_bound_replay = on;
  test_bound_replayed = 0;
  test_bound_replay_mismatches = 0;
}
uint64_t layout_test_bound_replayed() {
  ScopedLock const held{ test_dont_look_lock };
  return test_bound_replayed;
}
uint64_t layout_test_bound_replay_mismatches() {
  ScopedLock const held{ test_dont_look_lock };
  return test_bound_replay_mismatches;
}
#endif

}  // namespace scav
