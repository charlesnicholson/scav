// The four phases in a line, the portfolio of phase-2 tuples wrapped around
// the last two of them, then the geometry columns as the only output.
// Everything else here is the columns and the two hashes over them.

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
#include "layout/wire.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_stable_sort.h"
#include "scav_thread.h"
#include "scav_vec.h"
#include "scav_xxhash.h"

#include <algorithm>
#include <array>
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
void layout_test_label_bound(bool on, bool verify);
uint64_t layout_test_label_bound_skipped();
uint64_t layout_test_label_bound_labelled();
uint64_t layout_test_label_bound_mismatches();
void layout_test_search_memo(bool on);
void layout_test_search_memo_verify(bool on);
void layout_test_no_search(bool on);
uint32_t layout_test_search_memo_hits();
uint32_t layout_test_search_memo_mismatches();
std::vector<Cost> const &layout_test_schedule_first();
std::vector<Cost> const &layout_test_schedule_second();
std::vector<Cost> const &layout_test_schedule_kept();
#endif

SCAV_INTERNAL_BEGIN
// The inflation loop's decision and the portfolio's three pure parts,
// bracketed so a test reaches cases no chart does. The prototypes a test uses
// are its own; see scav_internal.h.
bool inflation_done(uint32_t fewest, uint32_t degraded, uint32_t unreachable, bool &keep);
uint32_t search_tuple_count(scav_profile const &p);
uint32_t search_move_budget(scav_profile const &p);
Row search_row(scav_profile const &p, uint32_t index);
uint32_t search_argmin(std::vector<Cost> const &cost, std::vector<uint8_t> const &viable);
// Every input a Level 1 search's result depends on, as words: the objective, the row,
// the budget, the seed pins in order, and the frames it may move in.
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
  GeomLead,
  GeomTrail,
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

static_assert(sizeof(scav_profile) == 49 * sizeof(int32_t),
              "the profile must stay a flat block of int32 with no padding, or the "
              "inputs digest below would hash bytes whose values are unspecified");

// Every non-geometry input a golden depends on. Without it a hash names the
// numbers that came out and not the run that produced them.
uint32_t inputs_digest(scav_spaces const &s, scav_layout_opts const &o) {
  std::vector<scav_byte> b;
  // Padding is what forbids hashing a struct's bytes, and the assert above
  // proves there is none, so the copy reads every knob and nothing else.
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
  // The font reaches layout only as the integers it measured, so its identity
  // rides in here rather than as an argument layout would never read.
  append_u32(b, spaces_digest(s));
  return xxhash32(b.data(), b.size(), 0);
}

void write_columns(Chart &c, SizedLayout const &z, Routes const &r, uint32_t inputs) {
  write_rows(c, geom_column(c, GEOM[GeomState]), z.state);
  write_rows(c, geom_column(c, GEOM[GeomBefore]), z.before);
  write_rows(c, geom_column(c, GEOM[GeomAfter]), z.after);
  write_rows(c, geom_column(c, GEOM[GeomLead]), z.lead);
  write_rows(c, geom_column(c, GEOM[GeomTrail]), z.trail);
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
  // The pins the drawing was laid out with: the caller's, with the ports the
  // facing pass turned. Re-deriving from these finds nothing left to turn.
  SearchPins laid;
  scav_rect sized_chart{};  // `sized.chart` before the routes and labels covered it
};

bool same_rect(scav_rect const &a, scav_rect const &b) {
  return (a.x == b.x) && (a.y == b.y) && (a.w == b.w) && (a.h == b.h);
}

// Whether two candidates place every state and route point alike.
bool same_geometry(Candidate const &a, Candidate const &b) {
  return std::ranges::equal(a.sized.state, b.sized.state, same_rect) &&
         std::ranges::equal(a.routes.points, b.routes.points, same);
}

// What the facing pass turns: legs whose in-frame edge is turned round, and
// legs whose port goes onto a cross border.
struct Facing {
  std::vector<ReversePin> reverses;
  std::vector<SidePin> sides;
};

// A cross-border side the facing pass already gave a node.
struct FacingTaken {
  uint32_t node, side;
};

// Legs whose port faces away from its far end. A port on its state's border moves onto a
// cross border the state sees unobstructed, one per side; others turn their edge round.
// Into `out`, with `taken` the caller's scratch.
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
    // Along the frame's ranks: across the page for most, down it for a
    // frame whose ranks run down, whose ports are on its top and bottom.
    bool const down{ (m < o.sub_down.size()) && (o.sub_down[m] != 0) };
    uint32_t const owner{ c.submachines[m].owner.v };
    auto const lined = [&](uint32_t face) {
      return (owner < c.states.size()) && face_lined(s, owner, face);
    };
    // The state a boundary node's segment joins in this frame, past any bends.
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
    // The cross border a port would take, INVALID where the state it joins
    // does not see it past the rest of the frame.
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
      if (lined(side)) { return INVALID; }
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
      uint32_t const side{ (down ? 0U : 2U) + ((o.seg_cross[nd.subject] == 1) ? 0U : 1U) };
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
      bool const on_leading{ down ? (z.node[at].y == frame.y)
                                  : (z.node[at].x == frame.x) };
      uint32_t const rank_face{ down ? 2U : 0U };
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
      // Leaving through the port or entering by it, and so which end is far.
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
      // The other end must be the transition's own state, met at its box.
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
                      { .trans = t, .leg = leg, .end = leaves ? 1U : 0U, .side = side });
        trace_emit({ .kind = TraceKind::PortTurned,
                     .frame = m,
                     .port = { .seg = seg, .trans = t.v, .leg = leg, .side = side } });
        continue;
      }
      bool wants_leading{ far_at < mid_at };
      if (on_state && lined(rank_face + (on_leading ? 0U : 1U))) {
        // Off a lined face: the other rank border, else a cross border the joined state
        // sees; with every face lined the port stays.
        uint32_t const j{ joined(seg) };
        uint32_t cross{ INVALID };
        for (bool const first : { true, false }) {
          if ((cross == INVALID) && (j != INVALID)) { cross = seen(j, first); }
        }
        if (lined(rank_face + (on_leading ? 1U : 0U)) && (cross == INVALID)) {
          trace_emit({ .kind = TraceKind::PortWalled,
                       .frame = m,
                       .port = { .seg = seg,
                                 .trans = t.v,
                                 .leg = leg,
                                 .side = rank_face + (on_leading ? 0U : 1U) } });
        }
        if (lined(rank_face + (on_leading ? 1U : 0U)) && (cross != INVALID)) {
          vec_push_back(taken, { .node = j, .side = cross });
          vec_push_back(
              out.sides,
              { .trans = t, .leg = leg, .end = leaves ? 1U : 0U, .side = cross });
          trace_emit({ .kind = TraceKind::PortTurned,
                       .frame = m,
                       .port = { .seg = seg, .trans = t.v, .leg = leg, .side = cross } });
          continue;
        }
        wants_leading = !on_leading;
      }
      if (on_leading == wants_leading) { continue; }
      if (on_state && lined(rank_face + (wants_leading ? 0U : 1U))) { continue; }
      vec_push_back(out.reverses, { .trans = t, .leg = leg });
      uint32_t const along{ (down ? 2U : 0U) + (wants_leading ? 0U : 1U) };
      trace_emit({ .kind = TraceKind::PortTurned,
                   .frame = m,
                   .port = { .seg = seg, .trans = t.v, .leg = leg, .side = along } });
    }
  }
}

// A candidate as phase 3 first routes it. Phases 1 and 2 and the facing pass read no
// face pin, so candidates differing only in faces share a prefix.
struct Prefix {
  SubmachineOrders laid;
  SizedLayout sized;
  SearchPins turned;
  Routes routes;  // as phase 3 first routes it, over `sized`
  bool ok{ false };
};

// What one candidate builds and discards beside its `Candidate`.
struct CandidateScratch {
  SubmachineOrders facing;
  SizedLayout again;
  Facing flips;
  std::vector<FacingTaken> taken;
};

// Bounds everything laid out, not just the root submachine: a route bends into
// a frame's padding and a path box centres on one, so both can reach past it.
// Before the score rather than after the pick, so `area` and `aspect` price
// the canvas that ships.
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

// The labels `search_candidate` places on `cand`, onto the routes it laid out without
// them.
void label_candidate(Candidate &cand,
                     Chart const &c,
                     scav_spaces const &s,
                     scav_profile const &knobs) {
  cand.sized.chart = cand.sized_chart;  // as uncovered as `route_transitions` saw it
  label_routes(cand.routes, c, cand.sized, s, knobs);
  cover_chart(cand);
}

// The caches a candidate reads and writes, each optional.
struct CandidateReuse {
  RouteCache const *reuse{ nullptr };  // the incumbent's routes, read
  RouteCache *fill{ nullptr };         // this candidate's routes, written
  Prefix *prefix{ nullptr };           // receives the prefix
  Prefix const *from{ nullptr };       // differs from the pins only in faces
  CandidateScratch *scratch{ nullptr };
};

// Phases 2 and 3 for `row` into `out`, reusing its capacity and `with.scratch`'s.
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
  scav_profile const &knobs{ row.knobs };
  Prefix *const prefix{ with.prefix };
  Prefix const *const from{ with.from };
  CandidateScratch own;
  CandidateScratch &sc{ (with.scratch != nullptr) ? *with.scratch : own };
  out.inflations = 0;
  out.viable = false;
  out.sized_chart = {};
  SubmachineOrders &facing{ sc.facing };
  SubmachineOrders const *use{ &orders };
  if (from != nullptr) {
    if (!from->ok) {
      out = Candidate{};
      return;
    }
    out.sized = from->sized;
    out.laid = from->turned;
    if (pins != nullptr) { out.laid.faces = pins->faces; }
    use = &from->laid;
  } else {
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
      if (prefix != nullptr) { prefix->ok = false; }
      out.routes = Routes{};
      out.laid = SearchPins{};
      return;
    }
    // Ports turned to face where their routes go, and the frames laid out again with
    // them; a function of the tuple and pins, so re-deriving the drawing repeats it.
    SearchPins &turned{ out.laid };
    if (pins != nullptr) {
      turned = *pins;  // copy-assigned, keeping `turned`'s storage
    } else {
      turned = SearchPins{};
    }
    Facing &flips{ sc.flips };
    facing_flips(flips, sc.taken, c, g, orders, out.sized, s);
    if (!flips.reverses.empty() || !flips.sides.empty()) {
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
      vec_insert(turned.sides, turned.sides.end(), flips.sides.begin(), flips.sides.end());
      order_submachines(facing, c, g, s, knobs, threads, turned);
      SizedLayout &again{ sc.again };
      std::vector<Diagnostic> spilled;
      if (size_layout(c,
                      g,
                      facing,
                      s,
                      knobs,
                      again,
                      spilled,
                      row.dar,
                      row.pack,
                      row.fold)) {
        use = &facing;
        std::swap(out.sized, again);
      }
    }
    if (prefix != nullptr) {
      prefix->laid = *use;
      prefix->sized = out.sized;
      prefix->turned = out.laid;
      prefix->ok = true;
    }
  }
  SubmachineOrders const &laid{ *use };
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
                    labels);
  if (prefix != nullptr) { prefix->routes = out.routes; }

  // `out` holds the best attempt so far; `done` reads from it, not from the latest
  // attempt.
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

SCAV_INTERNAL_END

namespace {

// What a bounded-move pass kept, so the caller replaces its candidate only when
// something was taken (11.10a).
struct Improved {
  Candidate best;
  Cost cost{};
  SearchPins held;
  bool viable{ false };  // the start laid out at all
};

// One Level 1 move; `kind` names which of its pins is set.
enum class MoveKind : uint32_t { Rank, Cut, Reverse, Face, Side, Fold };
static_assert((static_cast<uint16_t>(MoveKind::Rank) == TRACE_MOVE_RANK) &&
                  (static_cast<uint16_t>(MoveKind::Cut) == TRACE_MOVE_CUT) &&
                  (static_cast<uint16_t>(MoveKind::Reverse) == TRACE_MOVE_REVERSE) &&
                  (static_cast<uint16_t>(MoveKind::Face) == TRACE_MOVE_FACE) &&
                  (static_cast<uint16_t>(MoveKind::Side) == TRACE_MOVE_SIDE) &&
                  (static_cast<uint16_t>(MoveKind::Fold) == TRACE_MOVE_FOLD),
              "the trace names a move by this enum's ordinal");
struct Move {
  RankPin pin{};
  ChainCut leg{};
  FacePin face{};
  SidePin side{};
  FoldPin fold{};
  MoveKind kind{ MoveKind::Rank };
};

// What scoring one move yields: its cost and shares, without its geometry.
struct Scored {
  Cost cost{};
  std::array<int32_t, TIER2_TERMS> share{};
  bool viable{ false };
  bool inflated{ false };
};

void put_pins(SearchPins const &p, std::vector<uint32_t> &w) {
  vec_push_back(w, static_cast<uint32_t>(p.ranks.size()));
  for (RankPin const &r : p.ranks) { vec_insert(w, w.end(), { r.state.v, r.rank }); }
  vec_push_back(w, static_cast<uint32_t>(p.cuts.size()));
  for (ChainCut const &k : p.cuts) { vec_insert(w, w.end(), { k.trans.v, k.leg }); }
  vec_push_back(w, static_cast<uint32_t>(p.reverses.size()));
  for (ReversePin const &r : p.reverses) { vec_insert(w, w.end(), { r.trans.v, r.leg }); }
  vec_push_back(w, static_cast<uint32_t>(p.faces.size()));
  for (FacePin const &f : p.faces) {
    vec_insert(w, w.end(), { f.trans.v, f.leg, f.end, f.face });
  }
  vec_push_back(w, static_cast<uint32_t>(p.orients.size()));
  for (OrientPin const &o : p.orients) { vec_push_back(w, o.frame.v); }
  vec_push_back(w, static_cast<uint32_t>(p.sides.size()));
  for (SidePin const &sp : p.sides) {
    vec_insert(w, w.end(), { sp.trans.v, sp.leg, sp.end, sp.side });
  }
  vec_push_back(w, static_cast<uint32_t>(p.folds.size()));
  for (FoldPin const &f : p.folds) {
    vec_insert(w, w.end(), { f.frame.v, f.mode, f.layer });
  }
}

#ifdef SCAV_TESTING
bool same_pins(SearchPins const &a, SearchPins const &b) {
  std::vector<uint32_t> wa;
  std::vector<uint32_t> wb;
  put_pins(a, wa);
  put_pins(b, wb);
  return wa == wb;
}

// Whether two candidates are one drawing on one set of pins.
bool same_candidate(Candidate const &a, Candidate const &b) {
  return (a.viable == b.viable) && (a.inflations == b.inflations) &&
         same_rect(a.sized.chart, b.sized.chart) &&
         same_rect(a.sized_chart, b.sized_chart) && same_geometry(a, b) &&
         (a.routes.unplaced == b.routes.unplaced) &&
         std::ranges::equal(a.routes.placed, b.routes.placed, same_rect) &&
         same_pins(a.laid, b.laid);
}

bool same_cost(Cost const &a, Cost const &b) {
  return (a.t0_violations == b.t0_violations) && (a.t1_hints == b.t1_hints) &&
         (a.t2 == b.t2);
}

bool same_scored(Scored const &a, Scored const &b) {
  return (a.viable == b.viable) && (a.inflated == b.inflated) &&
         same_cost(a.cost, b.cost) && (a.share == b.share);
}
#endif

// Phases 1 to 3 for one set of pins, then the exact objective; pure in its arguments.
#ifdef SCAV_TESTING
// Whether a face move is scored from the incumbent's prefix, and whether each is also
// scored whole and compared; the counters tally uses and disagreements.
bool test_prefix_shortcut{ true };
bool test_prefix_verify{ false };
Mutex test_prefix_lock;
uint64_t test_prefix_used{ 0 };
uint64_t test_prefix_mismatches{ 0 };
#endif

// Whether every path box fits the sizing's chart, which a placed box then lies inside.
bool labels_fit(scav_spaces const &s, scav_rect const &chart) {
  for (uint32_t i = 0; (s.path_box != nullptr) && (i < s.n_path_box); ++i) {
    if ((s.path_box[i].w > chart.w) || (s.path_box[i].h > chart.h)) { return false; }
  }
  return true;
}

// Without `labelled`, a candidate routed without its labels scores a lower bound on its
// cost: the label terms are unpriced and the chart is the least it can grow to.
Scored scored_of(Chart const &c,
                 SplitGraph const &g,
                 CostContext const &scoring,
                 scav_spaces const &s,
                 scav_profile const &objective,
                 Candidate const &cand,
                 bool labelled = true) {
  Scored out;
  if (!cand.viable) { return out; }
  out.viable = true;
  if (cand.inflations != 0) {
    out.inflated = true;
    return out;
  }
  CostTerms terms{ cost_terms(scoring, c, g, cand.sized, cand.routes, s, objective) };
  // Aspect may fall as the chart grows, so it bounds only where no box can grow it.
  if (!labelled && !labels_fit(s, cand.sized_chart)) { terms.aspect = 0; }
  out.cost = cost_of(terms, objective);
  std::array<int64_t, TIER2_TERMS> const share{ cost_shares(terms, objective) };
  for (uint32_t k = 0; k < TIER2_TERMS; ++k) {
    out.share[k] = static_cast<int32_t>(share[k]);
  }
  return out;
}

// The candidate a move was scored on, in the scoring thread's scratch.
struct Routed {
  Candidate *cand{ nullptr };
};

// A candidate the bound pass routed, kept for its labels.
struct KeptCandidate {
  Candidate cand;
  Cost bound{};
  uint32_t index{ INVALID };
};

// What one scored move orders and discards. Per thread: a move runs on one thread and
// waits on nothing.
struct MoveScratch {
  SearchPins pins;  // the move's, which its scoring reads until it returns
  SubmachineOrders moved;
  Candidate whole, face;
  CandidateScratch keep;
};

MoveScratch &move_scratch() {
  thread_local MoveScratch s;
  return s;
}

// `from`, where given, is the incumbent's prefix and `pins` the incumbent's
// with one face more: the candidate's phases 1 and 2 are the incumbent's. Without
// `labels` the score is `scored_of`'s bound. `routed`, where given, receives the candidate
// scored.
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
                  Routed *routed) {
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
  // A traced search scores every move whole, its trace recording each move's phases.
  bool shortcut{ (from != nullptr) && (trace_sink() == nullptr) };
#ifdef SCAV_TESTING
  shortcut = shortcut && test_prefix_shortcut;
#endif
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
// Whether a search leaves no-op faces unscored, and how many it has left.
bool test_skip_noop_faces{ true };
Mutex test_noop_lock;
uint64_t test_noop_faces{ 0 };
#endif

// True where the search may leave a face with no effect unscored.
bool skipping_noop_faces() {
#ifdef SCAV_TESTING
  if (!test_skip_noop_faces) { return false; }
  ScopedLock const held{ test_noop_lock };
  ++test_noop_faces;
#endif
  return true;
}

#ifdef SCAV_TESTING
// Whether a labelled round scores by bound, whether each such round is also scored whole
// and its pick compared, each candidate labelled on kept routes against a full lay-out,
// and the tallies: candidates whose labels the bound skipped, candidates it labelled, and
// rounds or candidates that disagreed.
bool test_label_bound{ true };
bool test_label_bound_verify{ false };
Mutex test_label_bound_lock;
uint64_t test_label_bound_skipped{ 0 };
uint64_t test_label_bound_labelled{ 0 };
uint64_t test_label_bound_mismatches{ 0 };
#endif

// Whether a candidate's bound is below `incumbent`, so it may yet win and be scored
// exactly.
bool may_win(Scored const &bound, Cost const &incumbent) {
  return bound.viable && !bound.inflated && cost_less(bound.cost, incumbent);
}

// How many candidates of a labelled round, the first in bound order, keep their routes.
constexpr uint32_t KEPT_ROUTES{ 128 };

// `routed`, candidate `index` with bound `bound`, into `kept` while it is among the
// `KEPT_ROUTES` least by bound then index; `kept_n` of `kept` are in use.
void keep_least(std::vector<KeptCandidate> &kept,
                uint32_t &kept_n,
                uint32_t index,
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
}

// The index scoring every candidate whole and reducing in order would take: the least cost
// strictly below `incumbent`, the lowest index among equals; INVALID where none is.
// `bound(i)` scores candidate i's lower bound and `exact(i)` its cost, and only a
// candidate whose bound can still win is scored exactly, least bound first, one at a time.
template <typename Bound, typename Exact>
uint32_t least_by_bound(uint32_t n,
                        uint32_t threads,
                        Cost const &incumbent,
                        std::vector<Scored> &got,
                        std::vector<uint32_t> &order,
                        Bound const &bound,
                        Exact const &exact) {
  parallel_for(n, threads, [&](uint32_t i) { got[i] = bound(i); });
  order.clear();
  uint32_t open{ 0 };
  for (uint32_t i = 0; i < n; ++i) {
    if (!got[i].viable || got[i].inflated) { continue; }
    ++open;
    if (may_win(got[i], incumbent)) { vec_push_back(order, i); }
  }
  scav_stable_sort(order, [&got](uint32_t a, uint32_t b) {
    return cost_less(got[a].cost, got[b].cost);
  });
  uint32_t win{ INVALID };
  Cost best{ incumbent };
  auto const beats = [&](uint32_t i) {
    return cost_less(got[i].cost, best) ||
           ((win != INVALID) && (i < win) && !cost_less(best, got[i].cost));
  };
  uint32_t at{ 0 };
  for (; (at < order.size()) && beats(order[at]); ++at) {
    got[order[at]] = exact(order[at]);
    if (beats(order[at])) {
      best = got[order[at]].cost;
      win = order[at];
    }
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
                          std::vector<Scored> const &got,
                          uint32_t win,
                          Score const &score) {
  std::vector<Scored> full(n);
  parallel_for(n, threads, [&](uint32_t i) { full[i] = score(i, true); });
  uint32_t want{ INVALID };
  Cost best{ incumbent };
  bool same{ true };
  for (uint32_t i = 0; i < n; ++i) {
    same =
        same && (full[i].viable == got[i].viable) && (full[i].inflated == got[i].inflated);
    if (!full[i].viable || full[i].inflated) { continue; }
    same = same && !cost_less(full[i].cost, got[i].cost);
    if (cost_less(full[i].cost, best)) {
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

// Level 1: greedy, strictly improving moves in enumeration order. A move is a pin, so
// taking one appends to the pins the next round re-derives from.
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
                    std::vector<uint8_t> const *scope) {
  Improved out;
  // `scope` is per submachine, null for all: a move is offered only in a frame it names.
  auto const in_scope = [scope](uint32_t frame) {
    return (scope == nullptr) || ((frame < scope->size()) && ((*scope)[frame] != 0));
  };
  // Continued from, not restarted: `taken` reports every pin the drawing rests
  // on, so a caller handing them back gets the moves it already has plus more.
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
  // The start is scored here, as it stands.
  out.viable = out.best.viable;
  if (!out.viable) { return out; }
  CostContext const scoring{ cost_context(c) };
  std::vector<uint8_t> party;  // set where the incumbent's route bends or is charged
  out.cost = cost_of(
      cost_terms(scoring, c, g, out.best.sized, out.best.routes, s, objective, &party),
      objective);

  auto const add = [](SearchPins &into, Move const &m) {
    switch (m.kind) {
      case MoveKind::Cut: vec_push_back(into.cuts, m.leg); break;
      case MoveKind::Reverse:
        vec_push_back(into.reverses, { .trans = m.leg.trans, .leg = m.leg.leg });
        break;
      case MoveKind::Face: vec_push_back(into.faces, m.face); break;
      case MoveKind::Side: vec_push_back(into.sides, m.side); break;
      case MoveKind::Fold: vec_push_back(into.folds, m.fold); break;
      case MoveKind::Rank: vec_push_back(into.ranks, m.pin); break;
    }
  };
  auto const with = [&add](SearchPins base_pins, Move const &m) {
    add(base_pins, m);
    return base_pins;
  };
  // `with` into the scoring thread's own pins.
  auto const with_here = [&add](SearchPins const &base_pins,
                                Move const &m) -> SearchPins const & {
    SearchPins &into{ move_scratch().pins };
    into = base_pins;  // copy-assignment keeps each vector's storage
    add(into, m);
    return into;
  };

  // One counter per move kind, each capped at the budget.
  uint32_t cut_scored{ 0 };
  uint32_t rev_scored{ 0 };
  uint32_t face_scored{ 0 };
  uint32_t side_scored{ 0 };
  uint32_t pin_scored{ 0 };
  uint32_t fold_scored{ 0 };
  std::vector<Move> round;
  std::vector<Scored> got;
  std::vector<uint32_t> order;
  // A labelled round's first candidates in bound order, as their bound routed them: the
  // first `kept_n` of `kept`.
  std::vector<KeptCandidate> kept;
  uint32_t kept_n{ 0 };
  Mutex kept_lock;
  // A labelled round routes every candidate, then labels only those whose bound can still
  // win; a traced round scores each whole.
  bool bounded{ (s.path_box != nullptr) && (s.n_path_box != 0) &&
                (trace_sink() == nullptr) };
#ifdef SCAV_TESTING
  bounded = bounded && test_label_bound;
#endif
  std::vector<uint8_t> chained;
  std::vector<uint8_t> source;
  while ((cut_scored < budget) || (rev_scored < budget) || (face_scored < budget) ||
         (side_scored < budget) || (pin_scored < budget) ||
         (refold && (fold_scored < budget))) {
    // Enumerated first, scored second, reduced third. The scan used to do all
    // three at once, which made it sequential for no reason: a candidate is a
    // pure function of the model, the tuple and the pins, and nothing it
    // computes is read by the next one (11.10c).
    round.clear();

    // Cuts first: the segments phase 1 chained, named by the bends it created.
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

    // Which edge of a cycle carries the reversal; only a segment on a cycle is offered.
    for (uint32_t seg = 0; (seg < g.segments.size()) && (rev_scored < budget); ++seg) {
      if ((here.seg_cyclic[seg] == 0) || !in_scope(g.segments[seg].frame.v)) { continue; }
      TransId const t{ g.segments[seg].trans };
      if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
      uint32_t const leg{ seg - g.trans_segments[t.v].off };
      bool already{ false };
      for (ReversePin const &had : held.reverses) {
        already = already || ((had.trans.v == t.v) && (had.leg == leg));
      }
      if (already) { continue; }
      ++rev_scored;
      vec_push_back(round,
                    { .leg = { .trans = t, .leg = leg }, .kind = MoveKind::Reverse });
    }

    // Four faces per segment end, less any pinned. A face the router gives no effect
    // draws the incumbent exactly, so it is charged to the budget unscored.
    std::vector<uint8_t> const &faceable{ base.faceable };
    for (uint32_t seg = 0; (seg < g.segments.size()) && (face_scored < budget); ++seg) {
      if (!in_scope(g.segments[seg].frame.v)) { continue; }
      TransId const t{ g.segments[seg].trans };
      if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
      if ((t.v >= party.size()) || (party[t.v] == 0)) { continue; }  // straight, unpriced
      uint32_t const leg{ seg - g.trans_segments[t.v].off };
      for (uint32_t end = 0; (end < 2) && (face_scored < budget); ++end) {
        bool already{ false };
        for (FacePin const &had : held.faces) {
          already =
              already || ((had.trans.v == t.v) && (had.leg == leg) && (had.end == end));
        }
        if (already) { continue; }
        size_t const at{ (size_t{ 2 } * seg) + end };
        uint32_t const effective{ (at < faceable.size()) ? uint32_t{ faceable[at] }
                                                         : 0xFU };
        for (uint32_t f = 0; (f < 4) && (face_scored < budget); ++f) {
          ++face_scored;
          if ((((effective >> f) & 1U) == 0) && skipping_noop_faces()) { continue; }
          vec_push_back(round,
                        { .face = { .trans = t, .leg = leg, .end = end, .face = f },
                          .kind = MoveKind::Face });
        }
      }
    }

    // Every side of its state's border a port may cross but the incumbent's; a port
    // already pinned is not re-offered.
    SubmachineOrders const &laid{ incumbent.laid };
    vec_assign(source, laid.nodes.size(), 0);
    for (OrderEdge const &e : laid.edges) { source[e.src] = 1; }
    for (uint32_t seg = 0;
         incumbent.ok && (seg < g.segments.size()) && (side_scored < budget);
         ++seg) {
      uint32_t const frame{ g.segments[seg].frame.v };
      uint32_t const port{ laid.seg_port[seg] };
      uint32_t const node{ laid.seg_node[seg] };
      if (!in_scope(frame) || (port == INVALID) || (node == INVALID) ||
          (g.ports[port].state.v == INVALID)) {
        continue;
      }
      TransId const t{ g.segments[seg].trans };
      if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
      uint32_t const leg{ seg - g.trans_segments[t.v].off };
      uint32_t const end{ (g.segments[seg].dst_port == port) ? 1U : 0U };
      bool already{ false };
      for (SidePin const &had : held.sides) {
        already =
            already || ((had.trans.v == t.v) && (had.leg == leg) && (had.end == end));
      }
      if (already) { continue; }
      bool const down{ laid.sub_down[frame] != 0 };
      uint8_t const cross{ laid.seg_cross[seg] };
      uint32_t const now{ (cross != 0)
                              ? ((down ? 0U : 2U) + (cross - 1U))
                              : ((down ? 2U : 0U) + ((source[node] != 0) ? 0U : 1U)) };
      for (uint32_t side = 0; (side < 4) && (side_scored < budget); ++side) {
        if ((side == now) || face_lined(s, g.ports[port].state.v, side)) { continue; }
        ++side_scored;
        vec_push_back(round,
                      { .side = { .trans = t, .leg = leg, .end = end, .side = side },
                        .kind = MoveKind::Side });
      }
    }

    // An initial pseudostate is not offered: phase 1 seats it before the state it enters.
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
        ++pin_scored;
        vec_push_back(round, { .pin = { .state = StateId{ st }, .rank = r } });
      }
    }
    // Under `refold`, a frame drawn folded moves its cut before each other rank.
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
    if (round.empty()) { break; }

    // A candidate runs its phases on one thread: it is already the unit a free
    // thread takes, and sharding its frames too would only add claims.
    auto const score = [&](uint32_t i, bool labels, Routed *routed = nullptr) {
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
                        routed);
    };
    uint32_t const n{ static_cast<uint32_t>(round.size()) };
    vec_assign(got, n, {});
    uint32_t win{ INVALID };
    if (bounded) {
      // The first candidates in bound order are labelled on the routes their bound laid
      // out; any after them are laid out again.
#ifdef SCAV_TESTING
      uint32_t exacts{ 0 };
#endif
      auto const bound = [&](uint32_t i) {
        Routed routed;
        Scored const scored{ score(i, false, &routed) };
        if (may_win(scored, out.cost)) {
          ScopedLock const lock{ kept_lock };
          keep_least(kept, kept_n, i, scored.cost, routed);
        }
        return scored;
      };
      auto const exact = [&](uint32_t i) {
        uint32_t k{ 0 };
        while ((k < kept_n) && (kept[k].index != i)) { ++k; }
        if (k == kept_n) {
#ifdef SCAV_TESTING
          if (exacts < KEPT_ROUTES) {
            ScopedLock const lock{ test_label_bound_lock };
            ++test_label_bound_mismatches;
          }
#endif
          return score(i, true);
        }
#ifdef SCAV_TESTING
        ++exacts;
#endif
        Candidate &cand{ kept[k].cand };
        label_candidate(cand, c, s, row.knobs);
        Scored const scored{ scored_of(c, g, scoring, s, objective, cand) };
#ifdef SCAV_TESTING
        if (test_label_bound_verify) {
          Routed full;
          Scored const want{ score(i, true, &full) };
          if (!same_scored(scored, want) || !same_candidate(cand, *full.cand)) {
            ScopedLock const lock{ test_label_bound_lock };
            ++test_label_bound_mismatches;
          }
        }
#endif
        return scored;
      };
      win = least_by_bound(n, threads, out.cost, got, order, bound, exact);
      kept_n = 0;
#ifdef SCAV_TESTING
      if (test_label_bound_verify) {
        verify_bounded_round(n, threads, out.cost, got, win, score);
      }
#endif
    } else {
      parallel_for(n, threads, [&](uint32_t i) { got[i] = score(i, true); });
    }

    // Reduced in enumeration order, with the trace emitted here in that order.
    Move take{};
    Cost best{ out.cost };
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
        // Inflated spacing belongs to a profile the pins do not name, so such a move is
        // never taken.
        verdict = MoveVerdict::Inflated;
      } else if (cost_less(sc.cost, best)) {
        verdict = MoveVerdict::Taken;
        best = sc.cost;
        take = m;
        found = true;
      }
      uint32_t moved_trans{ INVALID };
      uint32_t moved_leg{ 0 };
      if (m.kind == MoveKind::Face) {
        moved_trans = m.face.trans.v;
        moved_leg = m.face.leg;
      } else if (m.kind == MoveKind::Side) {
        moved_trans = m.side.trans.v;
        moved_leg = m.side.leg;
      } else if (m.kind != MoveKind::Rank) {
        moved_trans = m.leg.trans.v;
        moved_leg = m.leg.leg;
      }
      bool const refolded{ m.kind == MoveKind::Fold };
      uint32_t moved_rank{ refolded ? m.fold.layer : 0U };
      if (m.kind == MoveKind::Rank) { moved_rank = m.pin.rank; }
      trace_emit(
          { .kind = TraceKind::CandidateScored,
            .pass = static_cast<uint16_t>(verdict),
            .frame = refolded ? m.fold.frame.v : INVALID,
            .score = { .row = INVALID,
                       .state = (m.kind == MoveKind::Rank) ? m.pin.state.v : INVALID,
                       .rank = moved_rank,
                       .trans = moved_trans,
                       .leg = moved_leg,
                       .move = static_cast<uint16_t>(m.kind),
                       .end = static_cast<uint16_t>(
                           (m.kind == MoveKind::Side) ? m.side.end : m.face.end),
                       .face = (m.kind == MoveKind::Side) ? m.side.side : m.face.face,
                       .t0 = sc.viable ? sc.cost.t0_violations : 0,
                       .t2 = sc.viable ? sc.cost.t2 : 0 } });
      // Beside the score, so a rejected move says which term rejected it.
      if ((trace_sink() != nullptr) && sc.viable) {
        TraceEvent e{ .kind = TraceKind::CandidateTerms, .terms = {} };
        for (uint32_t k = 0; k < TIER2_TERMS; ++k) { e.terms.share[k] = sc.share[k]; }
        trace_emit(e);
      }
    }

    if (!found) { break; }
    held = with(held, take);
    order_submachines(here, c, g, s, objective, threads, held);
    // Re-derived as the taken candidate was scored, reusing every frame the move left or
    // shifted.
    RouteCache const was{ std::move(base) };
    base = RouteCache{};
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
    (void)cost_terms(scoring, c, g, out.best.sized, out.best.routes, s, objective, &party);
  }
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
  vec_resize(p.faces, next());
  for (FacePin &f : p.faces) {
    f = { .trans = TransId{ next() }, .leg = next(), .end = next(), .face = next() };
  }
  vec_resize(p.orients, next());
  for (OrientPin &o : p.orients) { o = { .frame = SubmachineId{ next() } }; }
  vec_resize(p.sides, next());
  for (SidePin &sp : p.sides) {
    sp = { .trans = TransId{ next() }, .leg = next(), .end = next(), .side = next() };
  }
  vec_resize(p.folds, next());
  for (FoldPin &f : p.folds) {
    f = { .frame = SubmachineId{ next() }, .mode = next(), .layer = next() };
  }
  return p;
}

// Every search a layout has run, by `search_key`. Shared by the pool's threads and
// locked only around a lookup or an insert.
struct SearchMemo {
  Mutex lock;
  Memo table{ size_t{ 1 } << 24 };
  uint32_t hits{ 0 };        // under `lock`
  uint32_t mismatches{ 0 };  // under `lock`
};

#ifdef SCAV_TESTING
// Whether a layout runs its searches through a memo, how many it answered from one,
// and whether every answer is checked against running the search anyway.
bool test_search_memo{ true };
bool test_search_memo_verify{ false };
bool test_no_search{ false };  // a zero move budget whatever the profile asks
uint32_t test_search_memo_hits{ 0 };
uint32_t test_search_memo_mismatches{ 0 };
// Per row of the last searched layout: each schedule's cost, and the one kept.
std::vector<Cost> test_schedule_first, test_schedule_second, test_schedule_kept;

bool same_result(Improved const &a, Improved const &b) {
  return (a.viable == b.viable) && same_cost(a.cost, b.cost) &&
         same_pins(a.held, b.held) && same_candidate(a.best, b.best);
}
#endif

// `run_search` through the memo: a hit keeps the reached pins and cost and re-derives
// the drawing from the pins. A traced run always searches.
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
                      SearchMemo *memo) {
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
                      scope);
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
    if (hit) {
      vec_assign(value, at, at + len);
      ++memo->hits;
    }
  }
  if (hit) {
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
      ++memo->mismatches;
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

}  // namespace

SCAV_INTERNAL_BEGIN

// The table rows and bounded moves this chart runs: all of `portfolio_m` and
// `portfolio_k`.
uint32_t search_tuple_count(scav_profile const &p) {
  return imin(static_cast<uint32_t>(imax(p.portfolio_m, 1)), LAYOUT_SEARCH_ROWS);
}

uint32_t search_move_budget(scav_profile const &p) {
  return static_cast<uint32_t>(imax(p.portfolio_k, 0));
}

// Row `index` as a delta from the profile: bit 0 flips the box packer, bit 1 compaction,
// bit 2 the owner's hole, bit 3 the fold; row 0 is the caller's own tuple.
Row search_row(scav_profile const &p, uint32_t index) {
  Row out{ .knobs = p };
  out.knobs.trybox ^= static_cast<int32_t>(index & 1U);
  out.pack = (((index >> 1U) & 1U) != 0) ? Compaction::On : Compaction::Off;
  out.dar = (((index >> 2U) & 1U) != 0) ? DarSource::OwnerHole : DarSource::Profile;
  out.fold = (((index >> 3U) & 1U) != 0) ? Fold::Always : Fold::Scale;
  return out;
}

// `argmin(Cost, index)` in index order; row 0 answers for a set with nothing viable.
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
                SearchPins const *pins) {
  MemoRun const scope;
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
  // Once for every attempt below: phase 1 reads `sweep_count` and no extent, so
  // the inflated copies the retry loop makes order to the same rows.
  SearchPins const seed{ (pins != nullptr) ? *pins : SearchPins{} };
  SubmachineOrders const orders{ order_submachines(c, g, s, p, o.threads, seed) };

  // Level 2: every admitted row laid out whole and reduced in index order; a pinned row is
  // a table of one.
  bool const pinned{ row != INVALID };
  uint32_t const rows{ pinned ? 1U : search_tuple_count(p) };
  auto const row_of = [&](uint32_t i) { return search_row(p, pinned ? row : i); };
  std::vector<Candidate> candidates(rows);
  std::vector<Cost> cost(rows);
  std::vector<uint8_t> viable(rows, 0);
  // Rows run at once, each on one thread, and are reduced in index order.
  std::vector<std::vector<Diagnostic>> spilled(rows);
  CostContext const scoring{ cost_context(c) };
  parallel_for(rows, (rows > 1) ? o.threads : 1U, [&](uint32_t i) {
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
    // A single row is not scored; rows are compared under the caller's profile, not the
    // tuple's copy.
    if (viable[i] != 0) {
      CostTerms const t{
        cost_terms(scoring, c, g, candidates[i].sized, candidates[i].routes, s, p)
      };
      cost[i] = cost_of(t, p);
    }
  });
  // Row 0 is the caller's own tuple, so its findings are the run's and every
  // other row's are a candidate's business. Merged here rather than in the
  // worker, where the order would be the scheduler's.
  vec_insert(diags, diags.end(), spilled[0].begin(), spilled[0].end());
  // A tuple leaving the coordinate domain is no candidate; row 0 leaving it fails the run.
  if (!candidates[0].viable) { return false; }

  // Level 1 from every viable row, ranked by what each converges to; rows run at once, and
  // a lone row takes the caller's threads.
  uint32_t budget{ search_move_budget(p) };
#ifdef SCAV_TESTING
  if (test_no_search) { budget = 0; }
#endif
  std::vector<SearchPins> held(rows, seed);
  SearchMemo memo;
  SearchMemo *memo_at{ &memo };
#ifdef SCAV_TESTING
  if (!test_search_memo) { memo_at = nullptr; }
#endif
  // Searches the rows `which` names from the pins they hold, and keeps what
  // each reached.
  auto const search_rows = [&](std::vector<uint8_t> const &which, bool refold) {
    std::vector<uint32_t> active;
    for (uint32_t i = 0; i < rows; ++i) {
      if ((viable[i] != 0) && (which[i] != 0)) { vec_push_back(active, i); }
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
                             memo_at);
    });
    for (uint32_t k = 0; k < n; ++k) {
      if (!done[k].viable) { continue; }
      uint32_t const i{ active[k] };
      candidates[i] = std::move(done[k].best);
      cost[i] = done[k].cost;
      held[i] = std::move(done[k].held);
    }
  };

  // **Every viable row is searched to convergence** and the rows ranked by
  // what they reach. A shorter search from each ranked them better than none
  // did, but not well enough to cut on: on `brew` the two-round screen dropped
  // the row that converges to a canvas 38% smaller (11.10g).
  if (budget != 0) { search_rows(std::vector<uint8_t>(rows, 1), false); }
  std::vector<uint8_t> const &eligible{ viable };

  // Iterated local search on every viable row: each unturned cycle edge starts a search,
  // kept where it converges below the incumbent; kicks in distinct frames combine.
  auto const kick = [&](uint32_t best) {
    Row const best_row{ row_of(best) };
    // Row `best` searched from `start`, its moves confined to `within`'s frames where
    // given.
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
                          memo_at);
    };
    auto const frame_of_leg = [&](TransId t, uint32_t leg) {
      if (t.v >= g.trans_segments.size()) { return INVALID; }
      Span const segs{ g.trans_segments[t.v] };
      return (leg < segs.len) ? g.segments[segs.off + leg].frame.v : INVALID;
    };
    auto const outside = [&](uint32_t frame, std::vector<uint8_t> const &redo) {
      return (frame >= redo.size()) || (redo[frame] == 0);
    };
    // The incumbent's pins less every rank, cut, face, side and fold pin in a re-decided
    // frame; orientations and reversals are kept.
    auto const warm = [&](SearchPins const &from, std::vector<uint8_t> const &redo) {
      SearchPins out;
      out.reverses = from.reverses;
      out.orients = from.orients;
      for (SidePin const &sp : from.sides) {
        if (outside(frame_of_leg(sp.trans, sp.leg), redo)) {
          vec_push_back(out.sides, sp);
        }
      }
      for (RankPin const &r : from.ranks) {
        uint32_t const f{ (r.state.v < c.states.size()) ? c.states[r.state.v].parent.v
                                                        : INVALID };
        if (outside(f, redo)) { vec_push_back(out.ranks, r); }
      }
      for (ChainCut const &k : from.cuts) {
        if (outside(frame_of_leg(k.trans, k.leg), redo)) { vec_push_back(out.cuts, k); }
      }
      for (FacePin const &fp : from.faces) {
        if (outside(frame_of_leg(fp.trans, fp.leg), redo)) {
          vec_push_back(out.faces, fp);
        }
      }
      for (FoldPin const &fp : from.folds) {
        if (outside(fp.frame.v, redo)) { vec_push_back(out.folds, fp); }
      }
      return out;
    };
    auto const take = [&](Improved &&won) {
      candidates[best] = std::move(won.best);
      cost[best] = won.cost;
      held[best] = std::move(won.held);
    };
    // Rounds of kicks until none improves, then one settling pass. `turns`
    // adds a frame turned to run down, and one refolded, to the reversals a round tries.
    auto const kick_rounds = [&](bool turns) {
      bool kicked{ false };
      // Kicks draw from the move budget.
      uint32_t kick_scored{ 0 };
      for (;;) {
        SubmachineOrders const here{
          order_submachines(c, g, s, p, o.threads, held[best])
        };
        std::vector<uint8_t> turned(g.segments.size(), 0);
        for (OrderEdge const &e : here.edges) {
          if ((e.reversed != 0) && (e.segment < turned.size())) { turned[e.segment] = 1; }
        }
        // A kick is a reversal, a frame turned to run down or a frame refolded, kept only
        // where it converges cheaper.
        struct Kick {
          ReversePin reverse{};
          OrientPin orient{};
          FoldPin fold{};
        };
        std::vector<Kick> kicks;
        std::vector<uint32_t> kick_frame;
        for (uint32_t seg = 0; seg < g.segments.size(); ++seg) {
          if ((here.seg_cyclic[seg] == 0) || (turned[seg] != 0)) { continue; }
          TransId const t{ g.segments[seg].trans };
          if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
          uint32_t const leg{ seg - g.trans_segments[t.v].off };
          if (std::ranges::any_of(held[best].reverses, [t, leg](ReversePin const &had) {
                return (had.trans.v == t.v) && (had.leg == leg);
              })) {
            continue;  // pinned and turned back by the walk: pinning it again is a no-op
          }
          if (kick_scored >= budget) { break; }
          ++kick_scored;
          vec_push_back(kicks, { .reverse = { .trans = t, .leg = leg } });
          vec_push_back(kick_frame, g.segments[seg].frame.v);
        }
        for (uint32_t m = 0; turns && (m < here.sub_ranks.size()); ++m) {
          if ((c.submachines[m].live == 0) || (here.sub_ranks[m] < 2) ||
              (here.sub_down[m] != 0) || (kick_scored >= budget)) {
            continue;
          }
          ++kick_scored;
          vec_push_back(kicks, { .orient = { .frame = SubmachineId{ m } } });
          vec_push_back(kick_frame, m);
        }
        // A frame across the page with no fold pin, kicked to the fold it does not draw.
        std::vector<uint8_t> const &folded{ candidates[best].sized.folded };
        for (uint32_t m = 0; turns && (m < here.sub_ranks.size()); ++m) {
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
          vec_push_back(kicks, { .fold = { .frame = SubmachineId{ m }, .mode = mode } });
          vec_push_back(kick_frame, m);
        }
        if (kicks.empty()) { break; }
        auto const with_kick = [](SearchPins &into, Kick const &k) {
          if (k.fold.frame.v != INVALID) {
            vec_push_back(into.folds, k.fold);
          } else if (k.orient.frame.v != INVALID) {
            vec_push_back(into.orients, k.orient);
          } else {
            vec_push_back(into.reverses, k.reverse);
          }
        };

        std::vector<Improved> tried(kicks.size());
        parallel_for(static_cast<uint32_t>(kicks.size()), o.threads, [&](uint32_t j) {
          std::vector<uint8_t> redo(c.submachines.size(), 0);
          if (kick_frame[j] < redo.size()) { redo[kick_frame[j]] = 1; }
          SearchPins start{ warm(held[best], redo) };
          with_kick(start, kicks[j]);
          tried[j] = search_from(start, &redo);
        });

        // The best improving kick of each frame, and the best of those, in
        // enumeration order so the pick is the model's and not the scheduler's.
        std::vector<uint32_t> in_frame(c.submachines.size(), INVALID);
        uint32_t single{ INVALID };
        for (uint32_t j = 0; j < tried.size(); ++j) {
          if (!tried[j].viable || !cost_less(tried[j].cost, cost[best])) { continue; }
          uint32_t const f{ kick_frame[j] };
          if ((f < in_frame.size()) &&
              ((in_frame[f] == INVALID) ||
               cost_less(tried[j].cost, tried[in_frame[f]].cost))) {
            in_frame[f] = j;
          }
          if ((single == INVALID) || cost_less(tried[j].cost, tried[single].cost)) {
            single = j;
          }
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
          for (uint32_t const j : winners) { with_kick(start, kicks[j]); }
          Improved together{ search_from(start, &redo) };
          if (together.viable && cost_less(together.cost, tried[single].cost)) {
            take(std::move(together));
            kicked = true;
            continue;
          }
        }
        take(std::move(tried[single]));
        kicked = true;
        // The other frames' best kicks, cheapest first, each searched on top of
        // what the round has taken so far and kept where it still improves: a
        // round lands every independent choice it can for one search apiece,
        // where taking one per round re-scored every kick for each of them.
        std::vector<uint32_t> rest;
        for (uint32_t const j : winners) {
          if (j != single) { vec_push_back(rest, j); }
        }
        scav_insertion_sort(rest.data(),
                            rest.data() + rest.size(),
                            [&](uint32_t a, uint32_t b) {
                              return cost_less(tried[a].cost, tried[b].cost);
                            });
        for (uint32_t const j : rest) {
          std::vector<uint8_t> redo(c.submachines.size(), 0);
          if (kick_frame[j] < redo.size()) { redo[kick_frame[j]] = 1; }
          SearchPins start{ warm(held[best], redo) };
          with_kick(start, kicks[j]);
          Improved more{ search_from(start, &redo) };
          if (more.viable && cost_less(more.cost, cost[best])) { take(std::move(more)); }
        }
      }
      // One unscoped pass from what the kicks reached takes up what a frame's new size
      // opened.
      if (kicked) {
        Improved settled{ search_from(held[best], nullptr) };
        if (settled.viable && cost_less(settled.cost, cost[best])) {
          take(std::move(settled));
        }
      }
    };
    // Two schedules from the row's converged drawing, turns every round and turns after
    // reversals converge; the cheaper is kept, ties to the first.
    Candidate const from_candidate{ candidates[best] };
    Cost const from_cost{ cost[best] };
    SearchPins const from_held{ held[best] };
    kick_rounds(true);
    Improved mixed{ .best = std::move(candidates[best]),
                    .cost = cost[best],
                    .held = std::move(held[best]),
                    .viable = true };
    candidates[best] = from_candidate;
    cost[best] = from_cost;
    held[best] = from_held;
    kick_rounds(false);
    kick_rounds(true);
    if (!cost_less(cost[best], mixed.cost)) { take(std::move(mixed)); }
  };
  // Every row kicks at once, touching only its own slots; a row that converged to an
  // earlier row's drawing is not kicked again.
  auto const same_drawing = [&](uint32_t a, uint32_t b) {
    return (cost[a].t0_violations == cost[b].t0_violations) &&
           (cost[a].t2 == cost[b].t2) && same_geometry(candidates[a], candidates[b]);
  };
  if (budget != 0) {
    // Decided before any kick, which replaces a row's drawing.
    std::vector<uint8_t> repeat(rows, 0);
    for (uint32_t i = 0; i < rows; ++i) {
      for (uint32_t j = 0; (j < i) && (viable[i] != 0) && (repeat[i] == 0); ++j) {
        if ((viable[j] != 0) && (repeat[j] == 0) && same_drawing(i, j)) { repeat[i] = 1; }
      }
    }
    std::vector<uint32_t> kicking;
    for (uint32_t i = 0; i < rows; ++i) {
      if ((viable[i] != 0) && (repeat[i] == 0)) { vec_push_back(kicking, i); }
    }
    parallel_for(static_cast<uint32_t>(kicking.size()), o.threads, [&](uint32_t k) {
      kick(kicking[k]);
    });
    // A second search from each row's converged pins adds the fold moves; the cheaper is
    // kept, ties to the first.
    std::vector<Candidate> first{ candidates };
    std::vector<Cost> const first_cost{ cost };
    std::vector<SearchPins> first_held{ held };
    search_rows(std::vector<uint8_t>(rows, 1), true);
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
#ifdef SCAV_TESTING
  test_search_memo_hits = memo.hits;
  test_search_memo_mismatches = memo.mismatches;
#endif
  // Written whichever way the branches above went: what the drawing rests on,
  // not what this run added, so a run with no budget stands on its seed. The
  // move count is the pins beyond the seed rather than a sum of what each
  // search took: a screened row's moves are its finish's start, and a kick
  // drops the pins of the frame it re-decides.
  if (moves != nullptr) {
    auto const count = [](SearchPins const &q) {
      return static_cast<uint32_t>(q.ranks.size() + q.cuts.size() + q.reverses.size() +
                                   q.faces.size() + q.orients.size() + q.sides.size() +
                                   q.folds.size());
    };
    uint32_t const now{ count(held[best]) };
    uint32_t const had{ count(seed) };
    *moves = (now > had) ? (now - had) : 0U;
  }
  // With the ports the winning drawing turned, so what is handed back re-derives
  // it with nothing left to turn.
  if (taken != nullptr) {
    *taken = held[best];
    taken->reverses = candidates[best].laid.reverses;
    taken->sides = candidates[best].laid.sides;
  }

  SizedLayout sized{ std::move(candidates[best].sized) };
  Routes routes{ std::move(candidates[best].routes) };
  if (inflations != nullptr) { *inflations = candidates[best].inflations; }
  if (tuple != nullptr) { *tuple = pinned ? row : best; }
  placed = routes.placed;

  // `failed` is parallel to the transitions, so one walk emits the findings in
  // ordinal order.
  for (uint32_t t = 0; t < routes.failed.size(); ++t) {
    if (routes.failed[t] == 0) { continue; }
    vec_push_back(diags,
                  { .code = DiagCode::RouteDegraded,
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

bool layout_trace_json(Chart &c,
                       scav_spaces const &s,
                       scav_layout_opts const &o,
                       std::vector<scav_placed> &placed,
                       std::vector<Diagnostic> &diags,
                       std::vector<char> &out,
                       uint32_t row,
                       TraceScope scope) {
  if (scope == TraceScope::Search) {
    LayoutTrace t;
    scav_layout_opts serial{ o };
    serial.threads = 1;
    trace_sink_set(&t);
    bool const ran{ layout_run(c, s, serial, placed, diags, nullptr, nullptr, row) };
    trace_sink_set(nullptr);
    trace_to_json(t, c, out);
    return ran;
  }

  // Search first and keep what won, so the trace below is of the one drawing
  // that ships rather than of every candidate the search threw away (11.16).
  uint32_t won{ 0 };
  SearchPins pins;
  if (!layout_run(c, s, o, placed, diags, nullptr, &won, row, nullptr, &pins)) {
    trace_to_json({}, c, out);
    return false;
  }

  // The tuple and the pins together name that drawing, so nothing is left to
  // search: one thread for a deterministic event order, no moves to score.
  scav_layout_opts serial{ o };
  serial.threads = 1;
  serial.profile.portfolio_k = 0;
  LayoutTrace t;
  trace_sink_set(&t);
  std::vector<Diagnostic> again;
  bool const laid{
    layout_run(c, s, serial, placed, again, nullptr, nullptr, won, nullptr, nullptr, &pins)
  };
  trace_sink_set(nullptr);
  trace_to_json(t, c, out);
  return laid;
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
void layout_test_search_memo_verify(bool on) { test_search_memo_verify = on; }
void layout_test_no_search(bool on) { test_no_search = on; }
uint32_t layout_test_search_memo_hits() { return test_search_memo_hits; }
uint32_t layout_test_search_memo_mismatches() { return test_search_memo_mismatches; }
std::vector<Cost> const &layout_test_schedule_first() { return test_schedule_first; }
std::vector<Cost> const &layout_test_schedule_second() { return test_schedule_second; }
std::vector<Cost> const &layout_test_schedule_kept() { return test_schedule_kept; }
#endif

}  // namespace scav
