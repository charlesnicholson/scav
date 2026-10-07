// One pass per depth level, deepest first, each frame before the state that owns it.
// Ranks give one axis, `cross_coordinates` the other; the box formula sizes the state.

#include "layout/size.h"
#include "layout/trace.h"

#include "layout/coords.h"
#include "layout/geom.h"
#include "layout/memo.h"
#include "layout/pack.h"
#include "layout/router.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_cold.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_vec.h"
#include "scav_vector.h"

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace scav {

SizedLayout::SizedLayout(SizedLayout &&) noexcept = default;
SizedLayout &SizedLayout::operator=(SizedLayout &&) noexcept = default;
SizedLayout::~SizedLayout() = default;
static_assert(std::is_nothrow_move_constructible_v<SizedLayout> &&
              std::is_nothrow_move_assignable_v<SizedLayout>);

// Test entry points: a hole's ratio and every state's hole.
SCAV_INTERNAL_BEGIN
FrameDar size_hole_ratio(int32_t w, int32_t h);
void size_owner_holes(Chart const &c,
                      SizedLayout const &z,
                      int32_t sep,
                      Vector<FrameDar> &hole);
SCAV_INTERNAL_END

namespace {

scav_box_space box_of(scav_box_space const *rows, uint32_t count, uint32_t i) {
  return ((rows != nullptr) && (i < count)) ? rows[i] : scav_box_space{};
}

// Per-thread memo of `lay_out_sub` results; holds up to 2^20 words.
Memo &frame_memo() {
  thread_local Memo m{ size_t{ 1 } << 20 };
  return m;
}

// Both extents inside the coordinate domain; a saturated packing fails.
bool fits(Packing const &p) { return (p.w <= COORD_MAX) && (p.h <= COORD_MAX); }

// `pack_lr` into `packed`; under `trybox`, `pack_box` replaces it when the box fits and
// scales better or `pack_lr` does not fit. `row` is the caller's scratch.
void pack_best(Packing &packed,
               Packing &row,
               Vector<scav_rect> const &rects,
               int32_t sep,
               scav_profile const &p,
               FrameDar dar,
               Compaction compaction) {
  pack_lr(packed, rects, sep, dar.num, dar.den, compaction);
  if (p.trybox != 0) {
    pack_box(row, rects, sep);
    if (fits(row) && (!fits(packed) ||
                      pack_better(row, packed, dar.num, dar.den, p.sm_tiebreak != 0))) {
      packed.at.assign(row.at.begin(), row.at.end());
      packed.w = row.w;
      packed.h = row.h;
    }
  }
}

void overflow(std::vector<Diagnostic> &diags, ElemKind kind, uint32_t ordinal) {
  vec_push_back(diags,
                { .code = DiagCode::CoordinateOverflow,
                  .subject = { .kind = kind, .ordinal = ordinal },
                  .doc = { INVALID },
                  .src = {} });
}

// True for a non-Normal state with no bands, no loop room and only empty live
// submachines; the box formula and the band inset give it no `pad` ring.
bool bare_pseudostate(Chart const &c,
                      Vector<scav_rect> const &sub,
                      scav_box_space const &b,
                      scav_extent room,
                      uint32_t i) {
  if ((c.states[i].kind == StateKind::Normal) || (b.h_before != 0) || (b.h_after != 0) ||
      (b.w_before != 0) || (b.w_after != 0) || (room.h != 0)) {
    return false;
  }
  Span const subs{ c.states[i].submachines };
  for (uint32_t k = 0; k < subs.len; ++k) {
    uint32_t const m{ c.submachine_ids[subs.off + k].v };
    if (c.submachines[m].live == 0) { continue; }
    if ((sub[m].w != 0) || (sub[m].h != 0)) { return false; }
  }
  return true;
}

// A pseudostate the seating pass moved, levelled or declined.
struct Seated {
  uint32_t state;
  SeatHow how;
};

// One component laid out at one wrap width, packed or stacked.
struct Shape {
  Vector<scav_point> at;
  Wide w{ 0 }, h{ 0 };
  bool ok{ true };
  // Trace records, emitted for the kept shape.
  Vector<TraceFold> cuts;
  Vector<Seated> seated;
  Vector<TraceShift> centred;
  Vector<TracePiece> packed;
  Vector<TraceGap> lanes;
  Vector<TraceCarry> carried;
  // Some piece sits off the first piece's row, or not right of the one before.
  bool wraps{ false };
  // Every edge between stacked pieces joins two states without submachines.
  bool drawable{ true };
  Vector<uint8_t> lean;  // `SizedLayout::lean`, per frame edge

  // A default shape of `n` nodes and `edges` edges, keeping each buffer's storage.
  void reset(size_t n, size_t edges) {
    at.assign(n, scav_point{});
    lean.assign(edges, 0);
    w = 0;
    h = 0;
    ok = true;
    cuts.clear();
    seated.clear();
    centred.clear();
    packed.clear();
    lanes.clear();
    carried.clear();
    wraps = false;
    drawable = true;
  }
};

// One chunk of one component. `index` maps a frame node into `nodes`, and `chunk_index` a
// component node into the chunk, which indexes `centre`, `seat_at` and the layers.
struct ChunkView {
  Span span, espan, gspan;
  bool down;
  uint32_t first, last;
  Vector<uint32_t> const &nodes, &index, &local_rank, &global_rank;
  Vector<uint32_t> const &chunk_index, &chunk_nodes;
  Vector<int32_t> const &line, &inset, &centre, &seat_at;
  CoordGraph const &cg;
};

// A frame the descent has yet to visit, and its root-absolute origin.
struct Frame {
  uint32_t sub;
  int32_t x, y;
};

// Every buffer a sizing pass uses, per thread, reassigned in place; one frame at a time.
struct SizeScratch {
  // The pass, and the states it sizes.
  Vector<scav_point> sub_local;
  Vector<int32_t> seg_label_h, seg_label_w;
  Vector<uint32_t> port_seg;
  std::vector<Vector<uint32_t>> states_at, subs_at;
  Vector<scav_rect> kids;
  Vector<uint32_t> ids;
  Vector<Frame> work;
  Vector<scav_extent> loop_label, loop_room;  // `loop_rooms`
  Vector<uint8_t> looped;  // per state, 1 where an outer self-loop leaves it
  // What an `OwnerHole` sizing's first pass sized.
  SizedLayout first;
  Vector<FrameDar> hole;  // `size_owner_holes` of `first`
  // One frame.
  Vector<int32_t> reserve;
  Vector<uint32_t> adj_count, adj_off, adj, fill, component, queue;
  Vector<uint32_t> member_off, member, out_deg;
  Vector<scav_point> local;
  Vector<scav_rect> boxes;
  // One component, and one layout of it.
  Vector<uint32_t> nodes, global_rank, local_rank, index, in_layer;
  Vector<uint32_t> group, labelled, grouped, chunk_of, chunks;
  Vector<int32_t> extent, layer_w, widest_label, group_w, line, inset;
  Vector<Wide> layer_h, carry, applied, layer_lead;
  Vector<uint8_t> glued, ridden, paired, bare, rides;
  Vector<uint32_t> piece_of, ride_edge;
  Vector<scav_rect> pieces;
  Packing packed, row, placed;  // a frame's pieces, `pack_best`'s other, a frame's boxes
  Shape best, folded, stacked;
  // One chunk.
  CoordGraph cg;
  std::vector<Vector<uint32_t>> spare_layers;  // the layers a smaller chunk dropped
  Vector<uint32_t> chunk_index, chunk_nodes, arrivals, nearest;
  Vector<int32_t> seat_at, enter_at, centre;
  Vector<uint8_t> apart, open, left;
  Vector<uint32_t> mate, turning, turned_by, seat_to, fan;
  Vector<Wide> layer_x, kept_w;
};

SizeScratch &size_scratch() {
  thread_local SizeScratch s;
  return s;
}

// At least `n` empty buckets, each keeping its storage.
void clear_buckets(std::vector<Vector<uint32_t>> &buckets, size_t n) {
  if (buckets.size() < n) { vec_resize(buckets, n); }
  for (Vector<uint32_t> &b : buckets) { b.clear(); }
}

// Exactly `n` empty layers, moving storage to and from `spare`.
void clear_layers(std::vector<Vector<uint32_t>> &layers,
                  std::vector<Vector<uint32_t>> &spare,
                  size_t n) {
  while (layers.size() > n) {
    vec_push_back(spare, std::move(layers.back()));
    layers.pop_back();
  }
  while ((layers.size() < n) && !spare.empty()) {
    vec_push_back(layers, std::move(spare.back()));
    spare.pop_back();
  }
  vec_resize(layers, n);
  for (Vector<uint32_t> &l : layers) { l.clear(); }
}

// One sizing pass: what its frames and states share, and what it has sized so far.
struct Sizer {
  Chart const &c;
  SplitGraph const &g;
  SubmachineOrders const &o;
  scav_spaces const &s;
  scav_profile const &p;
  Vector<FrameDar> const &hole;
  Compaction compaction;
  Fold fold;
  SizedLayout &out;
  std::vector<Diagnostic> &diags;
  FrameDar profile_dar{ .num = p.dar_num, .den = p.dar_den };
  uint32_t profile_word{ memo_profile(p) };
  SizeScratch &sc{ size_scratch() };
  // Each submachine's origin inside its owner's packing; the descent makes it absolute.
  Vector<scav_point> &sub_local{ sc.sub_local };
  // Per segment, its label's extent across the frame's ranks and along them; only a
  // transition's `label_segment` carries it.
  Vector<int32_t> &seg_label_h{ sc.seg_label_h };
  Vector<int32_t> &seg_label_w{ sc.seg_label_w };
  // Per port, the segment on the border's inner side, whose boundary node is
  // where the router seats that port's slot.
  Vector<uint32_t> &port_seg{ sc.port_seg };
  Vector<scav_extent> &loop_label{ sc.loop_label };
  Vector<scav_extent> &loop_room{ sc.loop_room };
  Vector<uint8_t> &looped{ sc.looped };
  bool ok{ true };

  // The ratio packings inside `state` aim at: its hole's, else the profile's.
  [[nodiscard]] FrameDar dar_of(uint32_t state) const {
    return ((state < hole.size()) && (hole[state].num != 0)) ? hole[state] : profile_dar;
  }
  [[nodiscard]] FrameDar owner_dar(uint32_t m) const {
    StateId const owner{ c.submachines[m].owner };
    return (owner.v == INVALID) ? profile_dar : dar_of(owner.v);
  }
  // Whether frame `m`'s ranks run down the page: along the ranks is y and across is x;
  // for a frame running across, along is x and across is y.
  [[nodiscard]] bool runs_down(uint32_t m) const {
    return (m < o.sub_down.size()) && (o.sub_down[m] != 0);
  }
  static_assert((static_cast<uint32_t>(Fold::Scale) == FOLD_SCALE) &&
                (static_cast<uint32_t>(Fold::Always) == FOLD_ALWAYS) &&
                (static_cast<uint32_t>(Fold::Never) == FOLD_NEVER));
  // The row's rule, or the frame's fold pin where it has one.
  [[nodiscard]] Fold fold_of(uint32_t m) const {
    return ((m < o.sub_fold.size()) && (o.sub_fold[m] != 0))
               ? static_cast<Fold>(o.sub_fold[m] - 1U)
               : fold;
  }
  // The frame rank a fold pin's single cut falls before, or 0 for the width target's cuts.
  [[nodiscard]] uint32_t cut_of(uint32_t m) const {
    return (m < o.sub_fold_cut.size()) ? o.sub_fold_cut[m] : 0U;
  }
  // `seg_cross` of `seg`, 0 where a hand-built order lacks the column; and whether `node`
  // is a boundary node on a cross border.
  [[nodiscard]] uint8_t cross_of(uint32_t seg) const {
    return (seg < o.seg_cross.size()) ? o.seg_cross[seg] : uint8_t{ 0 };
  }
  [[nodiscard]] bool on_cross_border(uint32_t node) const {
    return (o.nodes[node].kind == OrderKind::Boundary) &&
           (cross_of(o.nodes[node].subject) != 0);
  }
  [[nodiscard]] scav_extent room_of(uint32_t state) const {
    return (state < loop_room.size()) ? loop_room[state] : scav_extent{};
  }
  [[nodiscard]] bool bare(scav_box_space const &b, uint32_t state) const {
    return bare_pseudostate(c, out.sub, b, room_of(state), state);
  }
  // The gap each band keeps from the contents beside it, as interior pieces keep from one
  // another: `sub_sep` where both are nonempty. Left, right, top, bottom.
  [[nodiscard]] std::array<int32_t, 4> band_gaps(scav_box_space const &b,
                                                 uint32_t state) const {
    scav_extent const room{ room_of(state) };
    bool full{ (room.w > 0) || (room.h > 0) };
    Span const subs{ c.states[state].submachines };
    for (uint32_t u = 0; !full && (u < subs.len); ++u) {
      uint32_t const m{ c.submachine_ids[subs.off + u].v };
      full = (c.submachines[m].live != 0) && ((out.sub[m].w > 0) || (out.sub[m].h > 0));
    }
    std::array<int32_t, 4> const band{ b.w_before, b.w_after, b.h_before, b.h_after };
    std::array<int32_t, 4> gap{};
    for (uint32_t k = 0; k < 4; ++k) { gap[k] = (full && (band[k] > 0)) ? p.sub_sep : 0; }
    return gap;
  }
  // How far a loop room above the packed submachines moves them down; 0 for one below.
  [[nodiscard]] int32_t room_shift(uint32_t state, int32_t packed_h) const {
    scav_extent const room{ room_of(state) };
    LoopPlace const at{ loop_place(out, state) };
    bool const above{ (at.face == 2) || ((at.face < 2) && (at.end == 0)) };
    if (!above || (room.h == 0)) { return 0; }
    return room.h + ((packed_h > 0) ? p.sub_sep : 0);
  }
  [[nodiscard]] int32_t attach_at(uint32_t seg, uint32_t state, bool down) const;
  [[nodiscard]] bool port_at(uint32_t seg, uint32_t state, bool down, int32_t &at) const;
  // Whether `seg` meets `state` at a port on its border, on any face.
  [[nodiscard]] bool ported(uint32_t seg, uint32_t state) const {
    if (seg >= g.segments.size()) { return false; }
    for (uint32_t const port : { g.segments[seg].src_port, g.segments[seg].dst_port }) {
      if ((port < g.ports.size()) && (g.ports[port].state.v == state)) { return true; }
    }
    return false;
  }
  void trace_ports(uint32_t m, bool down) const;
  [[nodiscard]] uint32_t connected_components(Span span, Span espan);
  void count_turns(ChunkView const &v);
  void step_layers(ChunkView const &v, Vector<TraceGap> &lanes);
  void glue_layers(Span span,
                   Span espan,
                   Vector<uint32_t> const &local_rank,
                   uint32_t layers);
  void assign_pieces(Span span,
                     Span espan,
                     Vector<uint32_t> const &nodes,
                     Vector<uint32_t> const &index,
                     Vector<uint32_t> const &local_rank,
                     bool carry);
  void seat_riders(ChunkView const &v, Shape &shape, uint32_t chunk, Wide chunk_w);
  void seat_pseudostates(ChunkView const &v, Shape &shape, Wide chunk_w, Wide chunk_h);
  // Whether chunk node `i` is an initial whose one neighbour lies in a later layer.
  [[nodiscard]] bool initial_ahead(ChunkView const &v, uint32_t i) const;
  [[nodiscard]] Wide leg_label_room(ChunkView const &v,
                                    Shape const &shape,
                                    Wide chunk_h) const;
  [[nodiscard]] Wide label_room(OrderEdge const &e) const;
  [[nodiscard]] Wide column_label_room(ChunkView const &v,
                                       Shape &shape,
                                       uint32_t chunk,
                                       Wide chunk_w) const;
  void lay_out_sub(uint32_t m);
  void place_sub(uint32_t m,
                 bool down,
                 FrameDar dar,
                 Vector<scav_rect> const &boxes,
                 Vector<uint32_t> const &component,
                 Vector<scav_point> const &local);
  void level_rank_ports(uint32_t m, bool down);
  void size_sub(uint32_t m);
  void size_state(uint32_t i);
};

// Where `seg` meets `state`, across the ranks from its centre: its port's boundary node,
// or zero for the box itself or a frame inside running the other way.
int32_t Sizer::attach_at(uint32_t seg, uint32_t state, bool down) const {
  int32_t at{ 0 };
  static_cast<void>(port_at(seg, state, down, at));
  return at;
}

// Sets `at` as `attach_at` returns it; true when `seg` meets `state` at a port on the
// faces this frame's edges arrive at.
bool Sizer::port_at(uint32_t seg, uint32_t state, bool down, int32_t &at) const {
  at = 0;
  if (seg >= g.segments.size()) { return false; }  // a hand-built frame
  SplitSegment const &sg{ g.segments[seg] };
  for (uint32_t const port : { sg.src_port, sg.dst_port }) {
    if ((port >= g.ports.size()) || (g.ports[port].state.v != state)) { continue; }
    uint32_t const inner{ port_seg[port] };
    if ((inner == INVALID) || (inner >= o.seg_node.size())) { continue; }
    uint32_t const node{ o.seg_node[inner] };
    uint32_t const frame{ g.segments[inner].frame.v };
    if ((node >= out.node.size()) || (frame >= c.submachines.size()) ||
        (c.submachines[frame].owner.v != state)) {
      continue;
    }
    // Top or bottom border: a rank-border port of a frame running down, or a cross-border
    // port of one running across.
    bool const top_or_bottom{ runs_down(frame) != (cross_of(inner) != 0) };
    if (top_or_bottom != down) { continue; }
    scav_box_space const b{ box_of(s.box_state, s.n_box_state, state) };
    Wide const pad{ bare(b, state) ? 0 : p.pad };
    std::array<int32_t, 4> const gap{ band_gaps(b, state) };
    if (down) {
      Wide const x{ pad + b.w_before + gap[0] + sub_local[frame].x + out.node[node].x };
      at = static_cast<int32_t>(x - (out.state[state].w / 2));
      return true;
    }
    Wide const y{ Wide{ pad } + b.h_before + gap[2] + room_shift(state, 1) +
                  sub_local[frame].y + out.node[node].y };
    at = static_cast<int32_t>(y - (out.state[state].h / 2));
    return true;
  }
  return false;
}

// Traces each edge end that meets a composite at a port off its centre.
void Sizer::trace_ports(uint32_t m, bool down) const {
  if (trace_sink() == nullptr) { return; }
  Span const espan{ o.sub_edges[m] };
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    for (uint32_t const end : { e.src, e.dst }) {
      if (o.nodes[end].kind != OrderKind::State) { continue; }
      int32_t const at{ attach_at(e.segment, o.nodes[end].subject, down) };
      if (at == 0) { continue; }
      trace_emit(
          { .kind = TraceKind::PortAttached,
            .frame = m,
            .shift = { .state = o.nodes[end].subject, .seg = e.segment, .by = at } });
    }
  }
}

// Components in first-node order, and nodes within one in (rank, pos) order: `component`
// per node, and component `id`'s nodes from `member[member_off[id]]`.
uint32_t Sizer::connected_components(Span span, Span espan) {
  Vector<uint32_t> &adj_count{ sc.adj_count };
  adj_count.assign(span.len, 0);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    ++adj_count[e.src - span.off];
    ++adj_count[e.dst - span.off];
  }
  Vector<uint32_t> &adj_off{ sc.adj_off };
  adj_off.assign(size_t{ span.len } + 1, 0);
  for (uint32_t k = 0; k < span.len; ++k) { adj_off[k + 1] = adj_off[k] + adj_count[k]; }
  Vector<uint32_t> &adj{ sc.adj };
  adj.assign(adj_off[span.len], 0);
  Vector<uint32_t> &fill{ sc.fill };
  fill.assign(adj_off.begin(), adj_off.end() - 1);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    adj[fill[e.src - span.off]++] = e.dst - span.off;
    adj[fill[e.dst - span.off]++] = e.src - span.off;
  }

  Vector<uint32_t> &component{ sc.component };
  component.assign(span.len, INVALID);
  Vector<uint32_t> &queue{ sc.queue };
  uint32_t components{ 0 };
  for (uint32_t seed = 0; seed < span.len; ++seed) {
    if (component[seed] != INVALID) { continue; }
    uint32_t const id{ components++ };
    component[seed] = id;
    queue.assign(1, seed);
    for (uint32_t at = 0; at < queue.size(); ++at) {
      uint32_t const node{ queue[at] };
      for (uint32_t k = adj_off[node]; k < adj_off[node + 1]; ++k) {
        if (component[adj[k]] == INVALID) {
          component[adj[k]] = id;
          queue.push_back(adj[k]);
        }
      }
    }
  }
  Vector<uint32_t> &member_off{ sc.member_off };
  member_off.assign(size_t{ components } + 1, 0);
  for (uint32_t k = 0; k < span.len; ++k) { ++member_off[component[k] + 1]; }
  for (uint32_t id = 0; id < components; ++id) { member_off[id + 1] += member_off[id]; }
  Vector<uint32_t> &member{ sc.member };
  member.assign(span.len, 0);
  fill.assign(member_off.begin(), member_off.end() - 1);
  for (uint32_t k = 0; k < span.len; ++k) { member[fill[component[k]]++] = k; }
  return components;
}

// The lanes turning in each boundary of the chunk, into `turning` and `turned_by`. An edge
// whose two ends overlap across the ranks runs straight and takes no lane.
void Sizer::count_turns(ChunkView const &v) {
  auto const across = [&](uint32_t st) {
    return v.down ? out.state[st].w : out.state[st].h;
  };
  auto const node_of = [&](uint32_t i) -> OrderNode const & {
    return o.nodes[v.span.off + v.nodes[i]];
  };
  auto const in_chunk = [&](uint32_t node) {
    uint32_t const i{ v.index[node - v.span.off] };
    return ((i != INVALID) && (v.chunk_index[i] != INVALID)) ? i : INVALID;
  };
  size_t const n{ v.chunk_nodes.size() };

  // Per chunk node, the one node every edge here joins it to, or `many`.
  uint32_t const many{ INVALID - 1 };
  Vector<uint32_t> &mate{ sc.mate };
  mate.assign(n, INVALID);
  for (uint32_t k = 0; k < v.espan.len; ++k) {
    OrderEdge const &e{ o.edges[v.espan.off + k] };
    uint32_t const a{ v.index[e.src - v.span.off] };
    uint32_t const b{ v.index[e.dst - v.span.off] };
    if ((a == INVALID) || (b == INVALID) || (a == b)) { continue; }
    for (uint32_t const end : { a, b }) {
      if (v.chunk_index[end] == INVALID) { continue; }
      uint32_t const there{ v.chunk_index[(end == a) ? b : a] };
      uint32_t &one{ mate[v.chunk_index[end]] };
      one = ((one == INVALID) || (one == there)) ? there : many;
      if (there == INVALID) { one = many; }
    }
  }
  auto const levelled = [&](uint32_t i, uint32_t with) {
    OrderNode const &nd{ node_of(i) };
    if (nd.kind != OrderKind::State) { return false; }
    StateKind const kind{ c.states[nd.subject].kind };
    return ((kind == StateKind::Initial) || (kind == StateKind::Final)) &&
           (mate[v.chunk_index[i]] == v.chunk_index[with]);
  };

  // Per chunk node, the mate the seating moves it beside, or INVALID: an initial's in the
  // layer either side or a final's in the one before, where the moved box stays inside.
  Wide chunk_h{ 0 };
  for (uint32_t i = 0; i < n; ++i) {
    chunk_h = imax(chunk_h, (Wide{ v.centre[i] } - (v.cg.extent[i] / 2)) + v.cg.extent[i]);
  }
  Vector<uint32_t> &seat_to{ sc.seat_to };
  seat_to.assign(n, INVALID);
  for (uint32_t i = 0; i < n; ++i) {
    OrderNode const &nd{ node_of(v.chunk_nodes[i]) };
    uint32_t const one{ mate[i] };
    if ((nd.kind != OrderKind::State) || (one >= n)) { continue; }
    StateKind const kind{ c.states[nd.subject].kind };
    uint32_t const r{ v.local_rank[v.nodes[v.chunk_nodes[i]]] };
    uint32_t const nr{ v.local_rank[v.nodes[v.chunk_nodes[one]]] };
    bool const toward{ ((kind == StateKind::Initial) && (nr == (r + 1))) ||
                       ((kind != StateKind::Normal) && ((nr + 1) == r)) };
    Wide const y{ Wide{ v.centre[one] } + v.seat_at[i] - (v.cg.extent[i] / 2) };
    bool const inside{ (y >= 0) && ((y + v.cg.extent[i]) <= chunk_h) };
    if (toward && inside && (node_of(v.chunk_nodes[one]).kind == OrderKind::State)) {
      seat_to[i] = one;
    }
  }
  auto const passes = [&](uint32_t end, uint32_t far) {
    uint32_t const rf{ v.local_rank[v.nodes[far]] };
    for (uint32_t const q : v.cg.layers[rf - v.first]) {
      if ((q != v.chunk_index[far]) && (seat_to[q] == v.chunk_index[end])) { return true; }
    }
    return false;
  };

  enum class End : uint8_t { Face, Point, Port };
  auto const reach = [&](OrderEdge const &e, uint32_t i, Wide &lo, Wide &hi) {
    OrderNode const &nd{ node_of(i) };
    lo = v.centre[v.chunk_index[i]];
    hi = lo;
    if (nd.kind != OrderKind::State) { return End::Point; }
    bool ported{ false };
    if (e.segment < g.segments.size()) {
      for (uint32_t const port :
           { g.segments[e.segment].src_port, g.segments[e.segment].dst_port }) {
        ported =
            ported || ((port < g.ports.size()) && (g.ports[port].state.v == nd.subject));
      }
    }
    StateKind const kind{ c.states[nd.subject].kind };
    if (ported) {
      lo += attach_at(e.segment, nd.subject, v.down);
      hi = lo;
      return End::Port;
    }
    if (kind_inscribed(kind)) { return End::Point; }
    Wide const half{ (across(nd.subject) / 2) -
                     state_corner_radius(kind, out.state[nd.subject], p.pad) };
    lo -= half;
    hi += half;
    return End::Face;
  };
  // Per chunk node, its edges toward the layer before and toward the one after.
  Vector<uint32_t> &fan{ sc.fan };
  fan.assign(2 * n, 0);
  auto const side = [&](uint32_t i, uint32_t toward) {
    uint32_t const r{ v.local_rank[v.nodes[i]] };
    uint32_t const t{ v.local_rank[v.nodes[toward]] };
    return (2 * static_cast<size_t>(v.chunk_index[i])) + ((t > r) ? 1U : 0U);
  };
  for (uint32_t k = 0; k < v.espan.len; ++k) {
    OrderEdge const &e{ o.edges[v.espan.off + k] };
    uint32_t const a{ in_chunk(e.src) };
    uint32_t const b{ in_chunk(e.dst) };
    if ((a == INVALID) || (b == INVALID) ||
        (v.local_rank[v.nodes[a]] == v.local_rank[v.nodes[b]])) {
      continue;
    }
    ++fan[side(a, b)];
    ++fan[side(b, a)];
  }

  Vector<uint32_t> &turning{ sc.turning };
  turning.assign(v.last - v.first, 0);
  Vector<uint32_t> &turned_by{ sc.turned_by };
  turned_by.assign(v.last - v.first, INVALID);
  for (uint32_t k = 0; k < v.espan.len; ++k) {
    OrderEdge const &e{ o.edges[v.espan.off + k] };
    uint32_t const a{ in_chunk(e.src) };
    uint32_t const b{ in_chunk(e.dst) };
    if ((a == INVALID) || (b == INVALID)) { continue; }
    uint32_t const ra{ v.local_rank[v.nodes[a]] };
    uint32_t const rb{ v.local_rank[v.nodes[b]] };
    if (ra == rb) { continue; }
    Wide alo{ 0 };
    Wide ahi{ 0 };
    Wide blo{ 0 };
    Wide bhi{ 0 };
    End const ea{ reach(e, a, alo, ahi) };
    End const eb{ reach(e, b, blo, bhi) };
    bool const alone{ ((ea == End::Port) || (fan[side(a, b)] == 1)) &&
                      ((eb == End::Port) || (fan[side(b, a)] == 1)) };
    bool const faces{ (ea == End::Face) && (eb == End::Face) };
    bool const straight{ levelled(a, b) || levelled(b, a) ||
                         ((imax(alo, blo) <= imin(ahi, bhi)) && (faces || alone) &&
                          !passes(a, b) && !passes(b, a)) };
    if (straight) { continue; }
    uint32_t const lo{ imin(ra, rb) - v.first };
    uint32_t const hi{ imax(ra, rb) - v.first };
    ++turning[lo];
    turned_by[lo] = e.segment;
    if (hi > (lo + 1)) {
      ++turning[hi - 1];
      turned_by[hi - 1] = e.segment;
    }
  }
}

// Each layer's place along the ranks into `layer_x`, and its room into `kept_w`. A seated
// pseudostate that fits inside its neighbour's layer takes no room in its own.
bool Sizer::initial_ahead(ChunkView const &v, uint32_t i) const {
  OrderNode const &nd{ o.nodes[v.span.off + v.nodes[v.chunk_nodes[i]]] };
  if ((nd.kind != OrderKind::State) || (c.states[nd.subject].kind != StateKind::Initial)) {
    return false;
  }
  uint32_t const one{ (i < sc.mate.size()) ? sc.mate[i] : INVALID };
  if (one >= v.chunk_nodes.size()) { return true; }
  return v.local_rank[v.nodes[v.chunk_nodes[one]]] >
         v.local_rank[v.nodes[v.chunk_nodes[i]]];
}

void Sizer::step_layers(ChunkView const &v, Vector<TraceGap> &lanes) {
  auto const along = [&](uint32_t st) {
    return v.down ? out.state[st].h : out.state[st].w;
  };
  auto const node_of = [&](uint32_t i) -> OrderNode const & {
    return o.nodes[v.span.off + v.nodes[i]];
  };
  Vector<int32_t> const &label_row{ (o.labels.size() == o.gaps.size()) ? o.labels
                                                                       : o.gaps };
  auto const label_gap = [&](uint32_t r) {
    uint32_t const b{ v.global_rank[r] };
    return (b < v.gspan.len) ? Wide{ label_row[v.gspan.off + b] } : Wide{ 0 };
  };
  Vector<uint32_t> const &turning{ sc.turning };
  int32_t const pitch{ label_line_height(p) };
  auto const lanes_at = [&](uint32_t r) {
    uint32_t const n{ turning[r - v.first] };
    return (n < 2) ? Wide{ 0 } : imin(Wide{ n } * pitch, Wide{ SPACE_MAX });
  };
  for (uint32_t r = v.first; (r + 1) < v.last; ++r) {
    if (turning[r - v.first] < 2) { continue; }
    lanes.push_back({ .boundary = v.global_rank[r],
                      .seg = sc.turned_by[r - v.first],
                      .width = static_cast<int32_t>(lanes_at(r)) });
  }

  auto const inset_of = [&](uint32_t i) {
    return (v.line[i] == 0) ? Wide{ 0 } : Wide{ v.inset[i] };
  };
  Vector<uint32_t> const &seat_to{ sc.seat_to };
  Vector<uint8_t> &left{ sc.left };
  left.assign(v.chunk_nodes.size(), 0);
  Vector<Wide> &kept_w{ sc.kept_w };
  kept_w.assign(v.last - v.first, 0);
  for (uint32_t r = v.first; r < v.last; ++r) {
    for (uint32_t const i : v.cg.layers[r - v.first]) {
      OrderNode const &nd{ node_of(v.chunk_nodes[i]) };
      uint32_t const one{ seat_to[i] };
      if ((nd.kind != OrderKind::State) || (one == INVALID)) { continue; }
      uint32_t const near{ v.chunk_nodes[one] };
      uint32_t const nr{ v.local_rank[v.nodes[near]] };
      Wide const need{ Wide{ p.rank_sep } + along(nd.subject) + (p.node_sep / 2) };
      Wide const room{ (nr > r) ? inset_of(near)
                                : (kept_w[nr - v.first] - inset_of(near) -
                                   along(node_of(near).subject)) };
      left[i] = (room >= need) ? 1U : 0U;
    }
    // The seating takes the lowest-indexed pseudostate beside each neighbour.
    for (uint32_t const i : v.cg.layers[r - v.first]) {
      for (uint32_t const j : v.cg.layers[r - v.first]) {
        if ((j < i) && (left[j] != 0) && (seat_to[j] == seat_to[i])) { left[i] = 0; }
      }
    }
    for (uint32_t const i : v.cg.layers[r - v.first]) {
      OrderNode const &nd{ node_of(v.chunk_nodes[i]) };
      if ((nd.kind != OrderKind::State) || (left[i] != 0)) { continue; }
      kept_w[r - v.first] =
          imax(kept_w[r - v.first],
               imax(Wide{ along(nd.subject) }, Wide{ v.line[v.chunk_nodes[i]] }));
    }
  }
  Vector<uint8_t> &open{ sc.open };
  open.assign(v.last - v.first, 1);
  for (uint32_t r = v.first; r < v.last; ++r) {
    for (uint32_t const i : v.cg.layers[r - v.first]) {
      if ((node_of(v.chunk_nodes[i]).kind != OrderKind::Boundary) && (left[i] == 0)) {
        open[r - v.first] = 0;
      }
    }
  }
  auto const sep_in = [&](uint32_t r) {
    bool const clear{ (open[r - v.first] != 0) || (open[r + 1 - v.first] != 0) };
    return (clear && (label_gap(r) == 0)) ? Wide{ box_clearance(p) } : Wide{ p.rank_sep };
  };
  // Where a chunk node starts along the ranks within its layer, and its width.
  auto const x_in_layer = [&](uint32_t q) {
    OrderNode const &nd{ node_of(v.chunk_nodes[q]) };
    uint32_t const r{ v.local_rank[v.nodes[v.chunk_nodes[q]]] };
    return initial_ahead(v, q) ? (kept_w[r - v.first] - along(nd.subject))
                               : inset_of(v.chunk_nodes[q]);
  };
  auto const w_of = [&](uint32_t q) {
    OrderNode const &nd{ node_of(v.chunk_nodes[q]) };
    return (nd.kind == OrderKind::State) ? Wide{ along(nd.subject) } : Wide{ 0 };
  };
  // Whether `q`, of the seated pseudostate `i`'s layer, shares the height its
  // box moves to, by the test the seating makes.
  auto const in_way = [&](uint32_t i, uint32_t q) {
    if ((q == i) || (left[q] != 0) ||
        (node_of(v.chunk_nodes[q]).kind == OrderKind::Boundary)) {
      return false;
    }
    Wide const lo{ Wide{ v.centre[seat_to[i]] } + v.seat_at[i] - (v.cg.extent[i] / 2) -
                   (p.node_sep / 2) };
    Wide const hi{ lo + v.cg.extent[i] + p.node_sep };
    Wide const qlo{ Wide{ v.centre[q] } - (v.cg.extent[q] / 2) };
    return (lo < (qlo + v.cg.extent[q])) && (qlo < hi);
  };
  Vector<Wide> &layer_x{ sc.layer_x };
  layer_x.assign(v.last - v.first, 0);
  for (uint32_t r = v.first + 1; r < v.last; ++r) {
    Wide const before{ layer_x[r - v.first - 1] };
    Wide x{ before + kept_w[r - v.first - 1] + sep_in(r - 1) +
            imax(label_gap(r - 1), lanes_at(r - 1)) };
    for (uint32_t const i : v.cg.layers[r - v.first - 1]) {
      if ((seat_to[i] == INVALID) || (left[i] != 0) ||
          (v.local_rank[v.nodes[v.chunk_nodes[seat_to[i]]]] != r)) {
        continue;
      }
      Wide const past{ Wide{ p.rank_sep } + w_of(i) + (p.node_sep / 2) -
                       inset_of(v.chunk_nodes[seat_to[i]]) };
      for (uint32_t const q : v.cg.layers[r - v.first - 1]) {
        if (in_way(i, q)) { x = imax(x, before + x_in_layer(q) + w_of(q) + past); }
      }
    }
    for (uint32_t const i : v.cg.layers[r - v.first]) {
      if ((seat_to[i] == INVALID) || (left[i] != 0) ||
          ((v.local_rank[v.nodes[v.chunk_nodes[seat_to[i]]]] + 1) != r)) {
        continue;
      }
      uint32_t const src{ seat_to[i] };
      Wide const end{ before + inset_of(v.chunk_nodes[src]) + w_of(src) + p.rank_sep +
                      w_of(i) + (p.node_sep / 2) };
      for (uint32_t const q : v.cg.layers[r - v.first]) {
        if (in_way(i, q)) { x = imax(x, end - x_in_layer(q)); }
      }
    }
    layer_x[r - v.first] = x;
  }
}

// An initial or final with one neighbour here moves level with it and `rank_sep` away,
// else only level, where its box clears every other node and stays inside the piece.
void Sizer::seat_pseudostates(ChunkView const &v,
                              Shape &shape,
                              Wide chunk_w,
                              Wide chunk_h) {
  auto const along = [&](uint32_t st) {
    return v.down ? out.state[st].h : out.state[st].w;
  };
  auto const box_of_node = [&](uint32_t i, Wide x, Wide y) {
    OrderNode const &nd{ o.nodes[v.span.off + v.nodes[v.chunk_nodes[i]]] };
    Wide const w{ (nd.kind == OrderKind::State) ? Wide{ along(nd.subject) } : Wide{ 0 } };
    Wide const h{ v.cg.extent[i] };
    return scav_rect{ .x = static_cast<int32_t>(x),
                      .y = static_cast<int32_t>(y - (h / 2)),
                      .w = static_cast<int32_t>(w),
                      .h = static_cast<int32_t>(h) };
  };
  for (uint32_t i = 0; i < v.chunk_nodes.size(); ++i) {
    uint32_t const at{ v.chunk_nodes[i] };
    OrderNode const &nd{ o.nodes[v.span.off + v.nodes[at]] };
    if (nd.kind != OrderKind::State) { continue; }
    StateKind const kind{ c.states[nd.subject].kind };
    if ((kind != StateKind::Initial) && (kind != StateKind::Final)) { continue; }
    uint32_t other{ INVALID };
    bool alone{ true };
    for (uint32_t k = 0; k < v.espan.len; ++k) {
      OrderEdge const &e{ o.edges[v.espan.off + k] };
      uint32_t const a{ v.index[e.src - v.span.off] };
      uint32_t const b{ v.index[e.dst - v.span.off] };
      uint32_t far{ INVALID };
      if (a == at) {
        far = b;
      } else if (b == at) {
        far = a;
      }
      if ((far == INVALID) || (far == at)) { continue; }
      if ((far >= v.chunk_index.size()) || (v.chunk_index[far] == INVALID)) {
        alone = false;
        continue;
      }
      if ((other != INVALID) && (other != v.chunk_index[far])) { alone = false; }
      other = v.chunk_index[far];
    }
    if (!alone || (other == INVALID)) { continue; }
    uint32_t const r{ v.local_rank[v.nodes[at]] };
    uint32_t const near_rank{ v.local_rank[v.nodes[v.chunk_nodes[other]]] };
    OrderNode const &nn{ o.nodes[v.span.off + v.nodes[v.chunk_nodes[other]]] };
    Wide const nw{ (nn.kind == OrderKind::State) ? Wide{ along(nn.subject) } : Wide{ 0 } };
    Wide const nx{ shape.at[v.chunk_nodes[other]].x };
    // A final sits `rank_sep` after the state it joins, an initial `rank_sep` before or
    // after the state it enters.
    Wide x{ shape.at[at].x };
    if (r == (near_rank + 1)) { x = nx + nw + p.rank_sep; }
    if ((kind == StateKind::Initial) && (near_rank == (r + 1))) {
      x = nx - p.rank_sep - along(nd.subject);
    }
    Wide const y{ Wide{ shape.at[v.chunk_nodes[other]].y } + v.seat_at[i] };
    auto const clear = [&](Wide cx, Wide cy) {
      scav_rect const want{ box_of_node(i, cx, cy) };
      if ((want.x < 0) || (want.y < 0) || ((Wide{ want.x } + want.w) > chunk_w) ||
          ((Wide{ want.y } + want.h) > chunk_h)) {
        return false;
      }
      // Skips boundary nodes, which sit on the frame's edge.
      scav_rect const room{ grow(want, p.node_sep / 2) };
      for (uint32_t j = 0; j < v.chunk_nodes.size(); ++j) {
        if ((j == i) || (o.nodes[v.span.off + v.nodes[v.chunk_nodes[j]]].kind ==
                         OrderKind::Boundary)) {
          continue;
        }
        scav_rect const there{
          box_of_node(j, shape.at[v.chunk_nodes[j]].x, shape.at[v.chunk_nodes[j]].y)
        };
        if (overlaps(room, there)) { return false; }
      }
      return true;
    };
    if (clear(x, y)) {
      shape.at[at] = { .x = static_cast<int32_t>(x), .y = static_cast<int32_t>(y) };
      shape.seated.push_back({ .state = nd.subject, .how = SeatHow::Moved });
    } else if (clear(shape.at[at].x, y)) {
      shape.at[at].y = static_cast<int32_t>(y);
      shape.seated.push_back({ .state = nd.subject, .how = SeatHow::Levelled });
    } else {
      shape.seated.push_back({ .state = nd.subject, .how = SeatHow::Declined });
    }
  }
}

// A label beside a leg between ranks needs its leader, height and half a node gap on one
// side; where neither side has it, the piece grows on its trailing side.
Wide Sizer::leg_label_room(ChunkView const &v, Shape const &shape, Wide chunk_h) const {
  auto const across = [&](uint32_t st) {
    return v.down ? out.state[st].w : out.state[st].h;
  };
  auto const in_chunk = [&](uint32_t node) {
    uint32_t const i{ v.index[node - v.span.off] };
    return ((i != INVALID) && (v.chunk_index[i] != INVALID)) ? i : INVALID;
  };
  int32_t const leader{ label_leader(p) };
  for (uint32_t k = 0; k < v.espan.len; ++k) {
    OrderEdge const &e{ o.edges[v.espan.off + k] };
    uint32_t const a{ in_chunk(e.src) };
    uint32_t const b{ in_chunk(e.dst) };
    if ((a == INVALID) || (b == INVALID) ||
        (v.local_rank[v.nodes[a]] == v.local_rank[v.nodes[b]]) ||
        (e.segment >= seg_label_h.size()) || (seg_label_h[e.segment] == 0)) {
      continue;
    }
    auto const span_of = [&](uint32_t i, Wide &lo, Wide &hi) {
      OrderNode const &nd{ o.nodes[v.span.off + v.nodes[i]] };
      Wide const half{ (nd.kind == OrderKind::State) ? Wide{ across(nd.subject) / 2 }
                                                     : Wide{ 0 } };
      lo = Wide{ shape.at[i].y } - half;
      hi = Wide{ shape.at[i].y } + half;
    };
    Wide alo{ 0 };
    Wide ahi{ 0 };
    Wide blo{ 0 };
    Wide bhi{ 0 };
    span_of(a, alo, ahi);
    span_of(b, blo, bhi);
    Wide const lo{ imax(alo, blo) };
    Wide const hi{ imin(ahi, bhi) };
    if (hi < lo) { continue; }
    Wide const leg{ lo + ((hi - lo) / 2) };
    Wide const need{ Wide{ leader } + seg_label_h[e.segment] + (p.node_sep / 2) };
    if ((leg < need) && ((chunk_h - leg) < need)) { chunk_h = leg + need; }
  }
  return chunk_h;
}

// A label's leader and width beside a leg, with half a node gap; zero for no label.
Wide Sizer::label_room(OrderEdge const &e) const {
  bool const has{ (e.segment < seg_label_w.size()) && (seg_label_w[e.segment] != 0) };
  return has ? (Wide{ label_leader(p) } + seg_label_w[e.segment] + (p.node_sep / 2))
             : Wide{ 0 };
}

// A labelled edge into a later piece shifts its end's layer along when a state lies below
// that end; a label on a flat leg inside one column widens the trailing side.
Wide Sizer::column_label_room(ChunkView const &v,
                              Shape &shape,
                              uint32_t chunk,
                              Wide chunk_w) const {
  auto const along = [&](uint32_t st) {
    return v.down ? out.state[st].h : out.state[st].w;
  };
  auto const width_of = [&](uint32_t i) {
    OrderNode const &nd{ o.nodes[v.span.off + v.nodes[i]] };
    return (nd.kind == OrderKind::State) ? Wide{ along(nd.subject) } : Wide{ 0 };
  };
  Vector<Wide> &layer_lead{ sc.layer_lead };
  layer_lead.assign(v.last - v.first, 0);
  for (uint32_t k = 0; k < v.espan.len; ++k) {
    OrderEdge const &e{ o.edges[v.espan.off + k] };
    uint32_t const ia{ v.index[e.src - v.span.off] };
    uint32_t const ib{ v.index[e.dst - v.span.off] };
    Wide const need{ label_room(e) };
    if ((ia == INVALID) || (ib == INVALID) || (need == 0)) { continue; }
    uint32_t const end{ (v.chunk_index[ia] != INVALID) ? ia : ib };
    uint32_t const other{ (end == ia) ? ib : ia };
    if ((v.chunk_index[end] == INVALID) || (sc.piece_of[other] <= chunk)) { continue; }
    uint32_t const r{ v.local_rank[v.nodes[end]] };
    bool passed{ false };
    Wide lead{ COORD_MAX };
    Wide trail{ 0 };
    for (uint32_t const j : v.chunk_nodes) {
      OrderNode const &nd{ o.nodes[v.span.off + v.nodes[j]] };
      if (nd.kind != OrderKind::State) { continue; }
      uint32_t const rj{ v.local_rank[v.nodes[j]] };
      if (rj == r) {
        lead = imin(lead, Wide{ shape.at[j].x });
        passed = passed || ((j != end) && (shape.at[j].y > shape.at[end].y));
      } else if (rj < r) {
        trail = imax(trail, Wide{ shape.at[j].x } + along(nd.subject));
      }
    }
    Wide const grow{ (Wide{ box_clearance(p) } + need) - (lead - trail) };
    if (!passed || (grow <= 0)) { continue; }
    for (uint32_t const j : v.chunk_nodes) {
      if (Wide{ shape.at[j].x } >= lead) {
        shape.at[j].x =
            static_cast<int32_t>(imin(Wide{ shape.at[j].x } + grow, Wide{ COORD_MAX }));
      }
    }
    chunk_w += grow;
    for (uint32_t q = (r - v.first) + 1; q < layer_lead.size(); ++q) {
      layer_lead[q] += (layer_lead[q] != 0) ? grow : Wide{ 0 };
    }
    layer_lead[r - v.first] = lead + grow;
  }
  for (uint32_t k = 0; k < v.espan.len; ++k) {
    OrderEdge const &e{ o.edges[v.espan.off + k] };
    uint32_t const a{ v.index[e.src - v.span.off] };
    uint32_t const b{ v.index[e.dst - v.span.off] };
    if ((a == INVALID) || (b == INVALID) || (v.chunk_index[a] == INVALID) ||
        (v.chunk_index[b] == INVALID) ||
        (v.local_rank[v.nodes[a]] != v.local_rank[v.nodes[b]])) {
      continue;
    }
    Wide const need{ label_room(e) };
    Wide const lead{ layer_lead[v.local_rank[v.nodes[a]] - v.first] };
    Wide const xa{ Wide{ shape.at[a].x } - lead };
    Wide const xb{ Wide{ shape.at[b].x } - lead };
    Wide const lo{ imax(xa, xb) };
    Wide const hi{ imin(xa + width_of(a), xb + width_of(b)) };
    Wide const leg{ (hi <= lo) ? Wide{ -1 } : (lo + ((hi - lo) / 2)) };
    if ((need != 0) && (leg >= 0) && (leg < need)) {
      chunk_w = imax(chunk_w, lead + leg + need);
    }
  }
  return chunk_w;
}

// `glued` marks a layer a cut before would part a pseudostate from its neighbour, `ridden`
// one parting a boundary node, which a pinned cut carries across (`rides`).
void Sizer::glue_layers(Span span,
                        Span espan,
                        Vector<uint32_t> const &local_rank,
                        uint32_t layers) {
  Vector<uint8_t> &glued{ sc.glued };
  glued.assign(layers, 0);
  Vector<uint8_t> &rides{ sc.rides };
  rides.assign(span.len, 0);
  sc.ridden.assign(layers, 0);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    uint32_t const a{ e.src - span.off };
    uint32_t const b{ e.dst - span.off };
    for (uint32_t const end : { a, b }) {
      OrderNode const &nd{ o.nodes[span.off + end] };
      bool const pseudostate{ (nd.kind == OrderKind::State) &&
                              ((c.states[nd.subject].kind == StateKind::Initial) ||
                               (c.states[nd.subject].kind == StateKind::Final)) };
      if (!pseudostate && (nd.kind != OrderKind::Boundary)) { continue; }
      uint32_t const other{ (end == a) ? b : a };
      uint32_t const ra{ local_rank[end] };
      uint32_t const rb{ local_rank[other] };
      if ((ra == INVALID) || (rb == INVALID) || (ra == rb)) { continue; }
      if (!pseudostate && (o.nodes[span.off + other].kind != OrderKind::Boundary) &&
          (sc.bare[ra] == 0)) {
        rides[end] = 1;
        sc.ridden[imax(ra, rb)] = 1;
        continue;
      }
      glued[imax(ra, rb)] = 1;
    }
  }
}

// Per node, the piece it is laid out in: its layer's, or under `carry` a riding boundary
// node's is the piece of the node it joins, with `ride_edge` naming the edge between them.
void Sizer::assign_pieces(Span span,
                          Span espan,
                          Vector<uint32_t> const &nodes,
                          Vector<uint32_t> const &index,
                          Vector<uint32_t> const &local_rank,
                          bool carry) {
  Vector<uint32_t> const &chunk_of{ sc.chunk_of };
  Vector<uint32_t> &piece_of{ sc.piece_of };
  Vector<uint32_t> &ride_edge{ sc.ride_edge };
  piece_of.resize(nodes.size());
  ride_edge.assign(nodes.size(), INVALID);
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    piece_of[i] = chunk_of[local_rank[nodes[i]]];
  }
  for (uint32_t k = 0; carry && (k < espan.len); ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    uint32_t const a{ index[e.src - span.off] };
    uint32_t const b{ index[e.dst - span.off] };
    if ((a == INVALID) || (b == INVALID)) { continue; }
    for (uint32_t const end : { a, b }) {
      uint32_t const other{ (end == a) ? b : a };
      uint32_t const joins{ chunk_of[local_rank[nodes[other]]] };
      if ((sc.rides[nodes[end]] == 0) || (joins == piece_of[end])) { continue; }
      piece_of[end] = joins;
      ride_edge[end] = k;
    }
  }
}

// A riding boundary node sits on its piece's leading edge, or trailing where it follows
// the node it joins, level with that node's end of their edge.
void Sizer::seat_riders(ChunkView const &v, Shape &shape, uint32_t chunk, Wide chunk_w) {
  for (uint32_t i = 0; i < v.nodes.size(); ++i) {
    uint32_t const k{ sc.ride_edge[i] };
    if ((k == INVALID) || (sc.piece_of[i] != chunk)) { continue; }
    OrderEdge const &e{ o.edges[v.espan.off + k] };
    uint32_t const other{ (e.src == (v.span.off + v.nodes[i])) ? e.dst : e.src };
    uint32_t const j{ v.index[other - v.span.off] };
    OrderNode const &nd{ o.nodes[other] };
    int32_t const at{ (nd.kind == OrderKind::State)
                          ? attach_at(e.segment, nd.subject, v.down)
                          : 0 };
    bool const leading{ v.local_rank[v.nodes[i]] < v.local_rank[v.nodes[j]] };
    shape.at[i] = { .x = leading ? 0 : static_cast<int32_t>(chunk_w),
                    .y = static_cast<int32_t>(Wide{ shape.at[j].y } + at) };
    shape.carried.push_back({ .seg = o.nodes[v.span.off + v.nodes[i]].subject,
                              .rank = v.global_rank[v.first] });
  }
}

// Lays out each connected component of frame `m` separately, then packs them.
void Sizer::lay_out_sub(uint32_t m) {
  Span const span{ o.sub_nodes[m] };
  uint32_t const ranks{ o.sub_ranks[m] };
  if ((span.len == 0) || (ranks == 0)) { return; }
  // Works along and across the ranks, placed as x and y at the end; a frame running down
  // inverts its hole's ratio.
  bool const down{ runs_down(m) };
  FrameDar const hole_dar{ owner_dar(m) };
  FrameDar const dar{ down ? FrameDar{ .num = hole_dar.den, .den = hole_dar.num }
                           : hole_dar };
  auto const along = [&](uint32_t st) { return down ? out.state[st].h : out.state[st].w; };
  auto const across = [&](uint32_t st) {
    return down ? out.state[st].w : out.state[st].h;
  };
  Span const espan{ o.sub_edges[m] };
  Span const gspan{ o.sub_gaps[m] };

  // Each end of a labelled edge reserves `leader + box height`, half on each side.
  int32_t const leader{ label_leader(p) };
  Vector<int32_t> &reserve{ sc.reserve };
  reserve.assign(span.len, 0);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    if (e.segment >= seg_label_h.size()) { continue; }  // a hand-built frame
    int32_t const box_h{ seg_label_h[e.segment] };
    if (box_h == 0) { continue; }
    uint32_t const src{ e.src - span.off };
    uint32_t const dst{ e.dst - span.off };
    reserve[src] = imax(reserve[src], leader + box_h);
    reserve[dst] = imax(reserve[dst], leader + box_h);
  }

  trace_ports(m, down);

  uint32_t const components{ connected_components(span, espan) };
  Vector<uint32_t> const &member{ sc.member };
  Vector<uint32_t> const &member_off{ sc.member_off };
  Vector<scav_point> &local{ sc.local };
  local.assign(span.len, scav_point{});
  Vector<scav_rect> &boxes{ sc.boxes };
  boxes.assign(components, scav_rect{});
  for (uint32_t id = 0; id < components; ++id) {
    // Leaves out cross-border ports; `place_sub` puts them on the frame's edge.
    sc.nodes.clear();
    for (uint32_t k = member_off[id]; k < member_off[id + 1]; ++k) {
      if (!on_cross_border(span.off + member[k])) { sc.nodes.push_back(member[k]); }
    }
    Vector<uint32_t> const &nodes{ sc.nodes };

    // Ranks renumbered from zero; `global_rank` keeps each frame rank for the gap charges.
    Vector<uint32_t> &global_rank{ sc.global_rank };
    global_rank.clear();
    Vector<uint32_t> &local_rank{ sc.local_rank };
    local_rank.assign(span.len, INVALID);
    for (uint32_t const k : nodes) {
      uint32_t const rank{ o.nodes[span.off + k].rank };
      if (global_rank.empty() || (global_rank.back() != rank)) {
        global_rank.push_back(rank);
      }
      local_rank[k] = static_cast<uint32_t>(global_rank.size()) - 1;
    }
    uint32_t const layers{ static_cast<uint32_t>(global_rank.size()) };
    Vector<uint32_t> &index{ sc.index };
    index.assign(span.len, INVALID);
    Vector<int32_t> &extent{ sc.extent };
    extent.assign(nodes.size(), 0);
    Vector<int32_t> &layer_w{ sc.layer_w };
    layer_w.assign(layers, 0);
    Vector<Wide> &layer_h{ sc.layer_h };
    layer_h.assign(layers, 0);
    // Running down, a node alone in its layer takes no reserve.
    Vector<uint32_t> &in_layer{ sc.in_layer };
    in_layer.assign(layers, 0);
    for (uint32_t const k : nodes) { ++in_layer[local_rank[k]]; }
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      index[nodes[i]] = i;
      OrderNode const &nd{ o.nodes[span.off + nodes[i]] };
      uint32_t const r{ local_rank[nodes[i]] };
      if (nd.kind == OrderKind::State) {
        extent[i] = across(nd.subject);
        layer_w[r] = imax(layer_w[r], along(nd.subject));
      }
      Wide const room{ (!down || (in_layer[r] > 1)) ? Wide{ reserve[nodes[i]] }
                                                    : Wide{ 0 } };
      extent[i] = static_cast<int32_t>(imin(Wide{ extent[i] } + room, Wide{ COORD_MAX }));
      layer_h[r] += extent[i] + p.node_sep;
    }
    // States joined inside one column share a centre line: the widest member's, or its
    // label room's; pseudostates seat beside their state and a bar keeps the leading edge.
    Vector<uint32_t> &group{ sc.group };
    group.assign(nodes.size(), 0);
    for (uint32_t i = 0; i < group.size(); ++i) { group[i] = i; }
    auto const find = [&group](uint32_t i) {
      while (group[i] != i) { i = group[i] = group[group[i]]; }
      return i;
    };
    auto const columnar = [&](uint32_t i) {
      OrderNode const &nd{ o.nodes[span.off + nodes[i]] };
      if (nd.kind != OrderKind::State) { return false; }
      StateKind const kind{ c.states[nd.subject].kind };
      return (kind != StateKind::Initial) && (kind != StateKind::Final) &&
             (kind != StateKind::Fork) && (kind != StateKind::Join);
    };
    auto const flat = [&](OrderEdge const &e, uint32_t &a, uint32_t &b) {
      a = index[e.src - span.off];
      b = index[e.dst - span.off];
      return (a != INVALID) && (b != INVALID) &&
             (local_rank[nodes[a]] == local_rank[nodes[b]]) && columnar(a) && columnar(b);
    };
    for (uint32_t k = 0; k < espan.len; ++k) {
      uint32_t a{ 0 };
      uint32_t b{ 0 };
      if (!flat(o.edges[espan.off + k], a, b)) { continue; }
      uint32_t const ga{ find(a) };
      uint32_t const gb{ find(b) };
      if (ga != gb) { group[imax(ga, gb)] = imin(ga, gb); }
    }
    // A group with two or more labelled edges fits the widest label's room on each side
    // of its line.
    Vector<uint32_t> &labelled{ sc.labelled };
    labelled.assign(nodes.size(), 0);
    Vector<int32_t> &widest_label{ sc.widest_label };
    widest_label.assign(nodes.size(), 0);
    for (uint32_t k = 0; k < espan.len; ++k) {
      OrderEdge const &e{ o.edges[espan.off + k] };
      uint32_t a{ 0 };
      uint32_t b{ 0 };
      if (!flat(e, a, b) || (e.segment >= seg_label_w.size()) ||
          (seg_label_w[e.segment] == 0)) {
        continue;
      }
      ++labelled[find(a)];
      widest_label[find(a)] = imax(widest_label[find(a)], seg_label_w[e.segment]);
    }
    Vector<int32_t> &group_w{ sc.group_w };
    group_w.assign(nodes.size(), 0);
    Vector<uint32_t> &grouped{ sc.grouped };
    grouped.assign(nodes.size(), 0);
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      if (!columnar(i)) { continue; }
      uint32_t const root_of{ find(i) };
      ++grouped[root_of];
      group_w[root_of] =
          imax(group_w[root_of], along(o.nodes[span.off + nodes[i]].subject));
      if (labelled[root_of] >= 2) {
        group_w[root_of] =
            imax(group_w[root_of], (2 * (leader + widest_label[root_of])) + p.node_sep);
      }
    }
    // Parallel to `nodes`: the width whose centre a grouped state sits on, zero outside a
    // group of two or more.
    Vector<int32_t> &line{ sc.line };
    line.assign(nodes.size(), 0);
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      uint32_t const root_of{ columnar(i) ? find(i) : INVALID };
      if ((root_of == INVALID) || (grouped[root_of] < 2)) { continue; }
      line[i] = group_w[root_of];
      uint32_t const r{ local_rank[nodes[i]] };
      layer_w[r] = imax(layer_w[r], group_w[root_of]);
    }
    // Per node, where a grouped state starts along its column; a state meeting a
    // composite's port centres on that port, clamped to the line.
    Vector<int32_t> &inset{ sc.inset };
    inset.assign(nodes.size(), 0);
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      if (line[i] == 0) { continue; }
      inset[i] = (line[i] - along(o.nodes[span.off + nodes[i]].subject)) / 2;
    }
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      uint32_t const st{ o.nodes[span.off + nodes[i]].subject };
      if ((line[i] == 0) || (c.states[st].submachines.len != 0)) { continue; }
      uint32_t ports{ 0 };
      Wide want{ 0 };
      for (uint32_t k = 0; k < espan.len; ++k) {
        OrderEdge const &e{ o.edges[espan.off + k] };
        uint32_t a{ 0 };
        uint32_t b{ 0 };
        if (!flat(e, a, b) || ((a != i) && (b != i))) { continue; }
        uint32_t const other{ (a == i) ? b : a };
        uint32_t const composite{ o.nodes[span.off + nodes[other]].subject };
        int32_t at{ 0 };
        if ((c.states[composite].submachines.len == 0) ||
            !port_at(e.segment, composite, !down, at)) {
          continue;
        }
        ++ports;
        want = Wide{ inset[other] } + (along(composite) / 2) + at - (along(st) / 2);
      }
      if (ports != 1) { continue; }
      inset[i] =
          static_cast<int32_t>(imin(imax(want, Wide{ 0 }), Wide{ line[i] } - along(st)));
    }
    auto const boundary_gap = [&](uint32_t r) {
      return (global_rank[r] < gspan.len) ? Wide{ o.gaps[gspan.off + global_rank[r]] }
                                          : Wide{ 0 };
    };
    // Hand-built orders carry no label row, and their gaps are all labels.
    Vector<int32_t> const &label_row{ (o.labels.size() == o.gaps.size()) ? o.labels
                                                                         : o.gaps };
    auto const label_gap = [&](uint32_t r) {
      return (global_rank[r] < gspan.len) ? Wide{ label_row[gspan.off + global_rank[r]] }
                                          : Wide{ 0 };
    };
    // A layer of boundary nodes alone is bare; a step beside one keeps `box_clearance`, or
    // `rank_sep` where a label sits between.
    Vector<uint8_t> &bare{ sc.bare };
    bare.assign(layers, 1);
    for (uint32_t const k : nodes) {
      if (o.nodes[span.off + k].kind != OrderKind::Boundary) { bare[local_rank[k]] = 0; }
    }
    auto const sep_after = [&](uint32_t r) {
      bool const open{ (bare[r] != 0) || (bare[r + 1] != 0) };
      return (open && (label_gap(r) == 0)) ? Wide{ box_clearance(p) } : Wide{ p.rank_sep };
    };

    glue_layers(span, espan, local_rank, layers);
    Vector<uint8_t> const &glued{ sc.glued };
    // A leg's position: the midpoint of its two ends' overlap, or -1 with none; and the
    // room its label needs beside it.
    auto const width_of_node = [&](uint32_t i) {
      OrderNode const &nd{ o.nodes[span.off + nodes[i]] };
      return (nd.kind == OrderKind::State) ? Wide{ along(nd.subject) } : Wide{ 0 };
    };
    auto const leg_of = [&](Wide xa, Wide xb, uint32_t a, uint32_t b) {
      Wide const lo{ imax(xa, xb) };
      Wide const hi{ imin(xa + width_of_node(a), xb + width_of_node(b)) };
      return (hi <= lo) ? Wide{ -1 } : (lo + ((hi - lo) / 2));
    };
    auto const room_of = [&](OrderEdge const &e) {
      bool const has{ (e.segment < seg_label_w.size()) && (seg_label_w[e.segment] != 0) };
      return has ? (Wide{ leader } + seg_label_w[e.segment] + (p.node_sep / 2))
                 : Wide{ 0 };
    };
    // One layout of the component, cut where the run passes `wrap_at`, or before frame
    // rank `cut_at` when nonzero; `stack` stacks the pieces in place of packing them.
    auto const lay_out = [&](Shape &shape, Wide wrap_at, uint32_t cut_at, bool stack) {
      shape.reset(nodes.size(), espan.len);
      Vector<uint32_t> &chunk_of{ sc.chunk_of };
      chunk_of.assign(layers, 0);
      Vector<uint32_t> &chunks{ sc.chunks };
      chunks.assign(1, 0);
      Wide run{ 0 };
      for (uint32_t r = 0; r < layers; ++r) {
        Wide const step{ (r == 0) ? Wide{ layer_w[r] }
                                  : (Wide{ layer_w[r] } + sep_after(r - 1) +
                                     boundary_gap(r - 1)) };
        bool const wants_cut{ (r > 0) && ((cut_at != 0) ? (global_rank[r] == cut_at)
                                                        : ((run + step) > wrap_at)) };
        bool const refused{ (glued[r] != 0) || ((cut_at == 0) && (sc.ridden[r] != 0)) };
        if (wants_cut && refused) {
          shape.cuts.push_back({ .rank = global_rank[r], .refused = 1, .carried = 0 });
        }
        if (wants_cut && !refused) {
          shape.cuts.push_back({ .rank = global_rank[r], .refused = 0, .carried = 0 });
          chunks.push_back(r);
          run = layer_w[r];
        } else {
          run += step;
        }
        chunk_of[r] = static_cast<uint32_t>(chunks.size()) - 1;
      }
      assign_pieces(span, espan, nodes, index, local_rank, cut_at != 0);
      Vector<uint32_t> const &piece_of{ sc.piece_of };

      Vector<scav_rect> &pieces{ sc.pieces };
      pieces.assign(chunks.size(), scav_rect{});
      Vector<Wide> &carry{ sc.carry };
      carry.assign(chunks.size(), 0);
      bool pieces_fit{ true };
      for (uint32_t chunk = 0; chunk < chunks.size(); ++chunk) {
        uint32_t const first{ chunks[chunk] };
        uint32_t const last{ ((chunk + 1) < chunks.size()) ? chunks[chunk + 1] : layers };

        // The chunk's coordinate graph: its nodes, then the edges with both ends in it.
        CoordGraph &cg{ sc.cg };
        clear_layers(cg.layers, sc.spare_layers, last - first);
        cg.extent.clear();
        cg.edges.clear();
        Vector<uint32_t> &chunk_index{ sc.chunk_index };
        chunk_index.assign(nodes.size(), INVALID);
        Vector<uint32_t> &chunk_nodes{ sc.chunk_nodes };
        chunk_nodes.clear();
        for (uint32_t i = 0; i < nodes.size(); ++i) {
          uint32_t const r{ local_rank[nodes[i]] };
          if ((chunk_of[r] != chunk) || (piece_of[i] != chunk)) { continue; }
          chunk_index[i] = static_cast<uint32_t>(chunk_nodes.size());
          chunk_nodes.push_back(i);
          cg.extent.push_back(extent[i]);
          cg.layers[r - first].push_back(chunk_index[i]);
        }
        // An initial and one other edge into the same face each meet it `half` off centre;
        // the initial's edge is weak.
        Vector<int32_t> &seat_at{ sc.seat_at };
        seat_at.assign(chunk_nodes.size(), 0);
        Vector<int32_t> &enter_at{ sc.enter_at };
        enter_at.assign(chunk_nodes.size(), 0);
        Vector<uint8_t> &apart{ sc.apart };
        apart.assign(espan.len, 0);
        auto const in_chunk = [&](uint32_t node) {
          uint32_t const i{ index[node - span.off] };
          return ((i != INVALID) && (chunk_index[i] != INVALID)) ? i : INVALID;
        };
        auto const initial = [&](uint32_t node) {
          OrderNode const &nd{ o.nodes[node] };
          return (nd.kind == OrderKind::State) &&
                 (c.states[nd.subject].kind == StateKind::Initial);
        };
        // Per node, how many edges enter its leading face from a non-initial, and the
        // least position among their earlier ends.
        Vector<uint32_t> &arrivals{ sc.arrivals };
        arrivals.assign(chunk_nodes.size(), 0);
        Vector<uint32_t> &nearest{ sc.nearest };
        nearest.assign(chunk_nodes.size(), INVALID);
        for (uint32_t k = 0; k < espan.len; ++k) {
          OrderEdge const &e{ o.edges[espan.off + k] };
          uint32_t const a{ in_chunk(e.src) };
          uint32_t const b{ in_chunk(e.dst) };
          if ((a == INVALID) || (b == INVALID)) { continue; }
          bool const forward{ local_rank[nodes[b]] == (local_rank[nodes[a]] + 1) };
          bool const back{ local_rank[nodes[a]] == (local_rank[nodes[b]] + 1) };
          if (!forward && !back) { continue; }
          uint32_t const later{ forward ? b : a };
          uint32_t const earlier{ forward ? a : b };
          if (initial(span.off + nodes[earlier])) { continue; }
          ++arrivals[chunk_index[later]];
          uint32_t &near{ nearest[chunk_index[later]] };
          near = imin(near, o.nodes[span.off + nodes[earlier]].pos);
        }
        for (uint32_t k = 0; k < espan.len; ++k) {
          OrderEdge const &e{ o.edges[espan.off + k] };
          uint32_t const dot{ in_chunk(e.src) };
          uint32_t const target{ in_chunk(e.dst) };
          if ((dot == INVALID) || (target == INVALID) || !initial(e.src)) { continue; }
          OrderNode const &nd{ o.nodes[e.src] };
          OrderNode const &nt{ o.nodes[e.dst] };
          if ((nt.kind != OrderKind::State) ||
              (c.states[nt.subject].submachines.len != 0) ||
              (local_rank[nodes[target]] != (local_rank[nodes[dot]] + 1))) {
            continue;
          }
          uint32_t const others{ arrivals[chunk_index[target]] };
          int32_t const half{ ceil_div((across(nd.subject) / 2) + box_clearance(p), 2) };
          if ((others != 1) || ((4 * half) > across(nt.subject))) { continue; }
          bool const above{ nd.pos < nearest[chunk_index[target]] };
          seat_at[chunk_index[dot]] = above ? -half : half;
          enter_at[chunk_index[target]] = above ? half : -half;
          apart[k] = 1;
        }
        for (uint32_t k = 0; k < espan.len; ++k) {
          OrderEdge const &e{ o.edges[espan.off + k] };
          uint32_t from{ index[e.src - span.off] };
          uint32_t to{ index[e.dst - span.off] };
          if ((from == INVALID) || (to == INVALID)) { continue; }
          if ((chunk_index[from] == INVALID) || (chunk_index[to] == INVALID)) { continue; }
          // Brandes-Kopf takes edges between consecutive layers, earlier end first: a back
          // edge is flipped; a flat edge or one spanning layers is skipped.
          uint32_t const rf{ local_rank[nodes[from]] };
          uint32_t const rt{ local_rank[nodes[to]] };
          auto const at_end = [&](uint32_t node) {
            OrderNode const &nd{ o.nodes[node] };
            return (nd.kind == OrderKind::State) ? attach_at(e.segment, nd.subject, down)
                                                 : 0;
          };
          int32_t from_at{ at_end(e.src) };
          int32_t to_at{ at_end(e.dst) };
          if ((rf + 1) != rt) {
            if ((rt + 1) != rf) { continue; }
            uint32_t const swap{ from };
            from = to;
            to = swap;
            int32_t const swap_at{ from_at };
            from_at = to_at;
            to_at = swap_at;
          }
          // The initial's own edge meets the target at the initial's height.
          to_at +=
              (apart[k] != 0) ? seat_at[chunk_index[from]] : enter_at[chunk_index[to]];
          cg.edges.push_back({ .from = chunk_index[from],
                               .to = chunk_index[to],
                               .inner = ((o.nodes[e.src].kind == OrderKind::Bend) &&
                                         (o.nodes[e.dst].kind == OrderKind::Bend))
                                            ? 1U
                                            : 0U,
                               .from_at = from_at,
                               .to_at = to_at,
                               .weak = apart[k] });
        }
        cg.sep = p.node_sep;
        Vector<int32_t> &centre{ sc.centre };
        cross_coordinates(cg, centre);

        ChunkView const view{ .span = span,
                              .espan = espan,
                              .gspan = gspan,
                              .down = down,
                              .first = first,
                              .last = last,
                              .nodes = nodes,
                              .index = index,
                              .local_rank = local_rank,
                              .global_rank = global_rank,
                              .chunk_index = chunk_index,
                              .chunk_nodes = chunk_nodes,
                              .line = line,
                              .inset = inset,
                              .centre = centre,
                              .seat_at = seat_at,
                              .cg = cg };
        count_turns(view);

        // The gap charged to the boundary this cut falls on, less the packer's `node_sep`;
        // applied below where the packing puts the piece beside the one before.
        carry[chunk] = (first == 0)
                           ? Wide{ 0 }
                           : imax(boundary_gap(first - 1) - p.node_sep, Wide{ 0 });
        step_layers(view, shape.lanes);
        Vector<Wide> const &kept_w{ sc.kept_w };
        Vector<Wide> const &layer_x{ sc.layer_x };
        Wide chunk_w{ layer_x[last - first - 1] + kept_w[last - first - 1] };
        Wide chunk_h{ 0 };
        for (uint32_t i = 0; i < chunk_nodes.size(); ++i) {
          // Leading edge plus extent, exact for an odd extent.
          chunk_h = imax(chunk_h, (Wide{ centre[i] } - (cg.extent[i] / 2)) + cg.extent[i]);
        }
        for (uint32_t i = 0; i < chunk_nodes.size(); ++i) {
          uint32_t const at{ chunk_nodes[i] };
          uint32_t const r{ local_rank[nodes[at]] };
          // An initial sits flush against its layer's trailing edge, beside the state it
          // enters in the next.
          OrderNode const &nd{ o.nodes[span.off + nodes[at]] };
          Wide const flush{ initial_ahead(view, i)
                                ? imax(kept_w[r - first] - along(nd.subject), Wide{ 0 })
                                : Wide{ 0 } };
          // On its group's centre line, where it has one.
          Wide const from_edge{ (line[at] == 0) ? flush : Wide{ inset[at] } };
          if (from_edge != flush) {
            shape.centred.push_back({ .state = nd.subject,
                                      .seg = INVALID,
                                      .by = static_cast<int32_t>(from_edge) });
          }
          // Piece-local; the packing below places the piece.
          shape.at[at] = { .x = static_cast<int32_t>(layer_x[r - first] + from_edge),
                           .y = static_cast<int32_t>(centre[i]) };
        }

        seat_pseudostates(view, shape, chunk_w, chunk_h);
        seat_riders(view, shape, chunk, chunk_w);
        chunk_w = column_label_room(view, shape, chunk, chunk_w);
        chunk_h = leg_label_room(view, shape, chunk_h);
        pieces_fit = pieces_fit && (chunk_w <= COORD_MAX) && (chunk_h <= COORD_MAX);
        if (!pieces_fit) { break; }
        pieces[chunk] = { .x = 0,
                          .y = 0,
                          .w = static_cast<int32_t>(chunk_w),
                          .h = static_cast<int32_t>(chunk_h) };
      }
      if (!pieces_fit) {
        shape.ok = false;
        return;
      }

      // Pieces are packed by `pack_best`; under `stack`, each sits under the one before at
      // the leading edge.
      Packing &packed{ sc.packed };
      if (stack) {
        packed.at.clear();
        Wide y{ 0 };
        Wide w{ 0 };
        for (scav_rect const &piece : pieces) {
          packed.at.push_back({ .x = 0,
                                .y = static_cast<int32_t>(imin(y, Wide{ PACK_SATURATED })),
                                .w = piece.w,
                                .h = piece.h });
          w = imax(w, Wide{ piece.w });
          y += Wide{ piece.h } + p.node_sep;
        }
        packed.w = static_cast<int32_t>(imin(w, Wide{ PACK_SATURATED }));
        packed.h = static_cast<int32_t>(imin(y - p.node_sep, Wide{ PACK_SATURATED }));
      } else {
        pack_best(packed, sc.row, pieces, p.node_sep, p, dar, compaction);
      }
      // Where the packing puts a piece beside the one before, its carried gap widens it at
      // the leading edge and the pieces repack.
      Vector<Wide> &applied{ sc.applied };
      applied.assign(chunks.size(), 0);
      for (uint32_t k = 1; (k < chunks.size()) && !stack; ++k) {
        scav_rect const &here{ packed.at[k] };
        scav_rect const &before{ packed.at[k - 1] };
        bool const beside{ (here.y < (before.y + before.h)) &&
                           (before.y < (here.y + here.h)) };
        if (!beside || (carry[k] == 0)) { continue; }
        applied[k] = carry[k];
        pieces[k].w =
            static_cast<int32_t>(imin(Wide{ pieces[k].w } + carry[k], Wide{ COORD_MAX }));
        for (uint32_t i = 0; i < nodes.size(); ++i) {
          if (piece_of[i] == k) {
            shape.at[i].x = static_cast<int32_t>(Wide{ shape.at[i].x } + carry[k]);
          }
        }
        for (TraceFold &cut : shape.cuts) {
          if ((cut.refused == 0) && (cut.rank == global_rank[chunks[k]])) {
            cut.carried = static_cast<int32_t>(carry[k]);
          }
        }
      }
      bool widened{ false };
      for (Wide const room : applied) { widened = widened || (room != 0); }
      if (widened) { pack_best(packed, sc.row, pieces, p.node_sep, p, dar, compaction); }
      // Label room on cut edges between stacked pieces; two labelled edges on one pair of
      // nodes take it on both sides, and the leading side shifts every piece along.
      auto const node_x = [&](uint32_t i) {
        return Wide{ shape.at[i].x } + packed.at[piece_of[i]].x;
      };
      auto const cut_ends = [&](OrderEdge const &e, uint32_t &a, uint32_t &b) {
        a = index[e.src - span.off];
        b = index[e.dst - span.off];
        if ((a == INVALID) || (b == INVALID) || (room_of(e) == 0)) { return false; }
        uint32_t const ca{ piece_of[a] };
        uint32_t const cb{ piece_of[b] };
        scav_rect const &pa{ packed.at[ca] };
        scav_rect const &pb{ packed.at[cb] };
        return (ca != cb) && ((pa.y >= (pb.y + pb.h)) || (pb.y >= (pa.y + pa.h)));
      };
      Vector<uint8_t> &paired{ sc.paired };
      paired.assign(espan.len, 0);
      for (uint32_t k = 0; k < espan.len; ++k) {
        uint32_t a{ 0 };
        uint32_t b{ 0 };
        if (!cut_ends(o.edges[espan.off + k], a, b)) { continue; }
        for (uint32_t q = k + 1; q < espan.len; ++q) {
          uint32_t fa{ 0 };
          uint32_t fb{ 0 };
          if (!cut_ends(o.edges[espan.off + q], fa, fb) || ((fa != a) && (fa != b)) ||
              ((fb != a) && (fb != b))) {
            continue;
          }
          paired[k] = 1;
          paired[q] = 1;
        }
      }
      // How far in from a state's leading end along the ranks a straight leg can seat: the
      // corner arc clamped to [1, half its length], or half its length if inscribed.
      auto const seat_inset = [&](uint32_t i) {
        OrderNode const &nd{ o.nodes[span.off + nodes[i]] };
        if (nd.kind != OrderKind::State) { return Wide{ 0 }; }
        StateKind const kind{ c.states[nd.subject].kind };
        Wide const len{ along(nd.subject) };
        Wide const arc{ state_corner_radius(kind, out.state[nd.subject], p.pad) };
        return kind_inscribed(kind) ? (len / 2) : imin(imax(arc, Wide{ 1 }), len / 2);
      };
      auto const seat_run = [&](uint32_t i, Wide &lo, Wide &hi) {
        OrderNode const &nd{ o.nodes[span.off + nodes[i]] };
        if (nd.kind != OrderKind::State) { return false; }
        lo = node_x(i) + seat_inset(i);
        hi = kind_inscribed(c.states[nd.subject].kind)
                 ? lo
                 : ((node_x(i) + along(nd.subject)) - seat_inset(i));
        return true;
      };
      // A piece below the one before moves forward to start the first cut edge's target
      // under its source, within `reach` and `node_sep` clear of other pieces.
      for (uint32_t k = 1; k < chunks.size(); ++k) {
        scav_rect &here{ packed.at[k] };
        scav_rect const &before{ packed.at[k - 1] };
        if (Wide{ here.y } < (Wide{ before.y } + before.h)) { continue; }
        uint32_t cut{ INVALID };
        uint32_t above{ INVALID };
        uint32_t below{ INVALID };
        for (uint32_t q = 0; (q < espan.len) && (cut == INVALID); ++q) {
          OrderEdge const &e{ o.edges[espan.off + q] };
          uint32_t const a{ index[e.src - span.off] };
          uint32_t const b{ index[e.dst - span.off] };
          if ((a == INVALID) || (b == INVALID) ||
              (o.nodes[e.src].kind != OrderKind::State) ||
              (o.nodes[e.dst].kind != OrderKind::State)) {
            continue;
          }
          uint32_t const ca{ piece_of[a] };
          uint32_t const cb{ piece_of[b] };
          if ((imin(ca, cb) != (k - 1)) || (imax(ca, cb) != k)) { continue; }
          cut = q;
          above = (ca < cb) ? a : b;
          below = (ca < cb) ? b : a;
        }
        if (cut == INVALID) { continue; }
        OrderEdge const &e{ o.edges[espan.off + cut] };
        Wide const x{ node_x(above) };
        Wide reach{ packed.w };
        Wide const mid{ x + (imin(width_of_node(above), width_of_node(below)) / 2) };
        if ((paired[cut] == 0) && (room_of(e) != 0) && (mid < room_of(e))) {
          reach = imax(reach, x + imax(seat_inset(above), seat_inset(below)) + room_of(e));
        }
        Wide const want{ imin(x - shape.at[below].x, reach - pieces[k].w) };
        if ((want <= here.x) || ((want + pieces[k].w) > COORD_MAX)) { continue; }
        scav_rect const moved{ .x = static_cast<int32_t>(want),
                               .y = here.y,
                               .w = pieces[k].w,
                               .h = here.h };
        bool clear{ true };
        for (uint32_t j = 0; j < chunks.size(); ++j) {
          clear = clear && ((j == k) || !overlaps(grow(moved, p.node_sep), packed.at[j]));
        }
        if (!clear) { continue; }
        here = moved;
        packed.w =
            static_cast<int32_t>(imin(imax(Wide{ packed.w }, Wide{ moved.x } + moved.w),
                                      Wide{ PACK_SATURATED }));
      }
      Wide lead{ 0 };
      for (uint32_t k = 0; k < espan.len; ++k) {
        OrderEdge const &e{ o.edges[espan.off + k] };
        uint32_t a{ 0 };
        uint32_t b{ 0 };
        if ((paired[k] == 0) || !cut_ends(e, a, b)) { continue; }
        Wide const leg{ leg_of(node_x(a), node_x(b), a, b) };
        if (leg >= 0) { lead = imax(lead, room_of(e) - leg); }
      }
      Wide stack_w{ Wide{ packed.w } + lead };
      for (uint32_t k = 0; k < espan.len; ++k) {
        OrderEdge const &e{ o.edges[espan.off + k] };
        uint32_t a{ 0 };
        uint32_t b{ 0 };
        if (!cut_ends(e, a, b)) { continue; }
        Wide const leg{ lead + leg_of(node_x(a), node_x(b), a, b) };
        if (leg < lead) { continue; }
        bool const leading{ (paired[k] == 0) && (leg >= room_of(e)) };
        if (leading) { continue; }
        // A lone trailing label takes its room from the leading end of the run both faces
        // can seat, where `lean` has the router seat the leg.
        Wide seat{ leg };
        Wide alo{ 0 };
        Wide ahi{ 0 };
        Wide blo{ 0 };
        Wide bhi{ 0 };
        if ((paired[k] == 0) && seat_run(a, alo, ahi) && seat_run(b, blo, bhi) &&
            (imax(alo, blo) <= imin(ahi, bhi)) && ((lead + imax(alo, blo)) < leg)) {
          seat = lead + imax(alo, blo);
          shape.lean[k] = 1;
        }
        stack_w = imax(stack_w, seat + room_of(e));
      }
      for (scav_rect &at : packed.at) { at.x = static_cast<int32_t>(Wide{ at.x } + lead); }
      packed.w = static_cast<int32_t>(imin(stack_w, Wide{ PACK_SATURATED }));
      // Clears `drawable` when an edge between stacked pieces ends at a non-state or a
      // composite.
      for (uint32_t k = 0; k < espan.len; ++k) {
        OrderEdge const &e{ o.edges[espan.off + k] };
        uint32_t const a{ index[e.src - span.off] };
        uint32_t const b{ index[e.dst - span.off] };
        if ((a == INVALID) || (b == INVALID)) { continue; }
        scav_rect const &pa{ packed.at[piece_of[a]] };
        scav_rect const &pb{ packed.at[piece_of[b]] };
        bool const stacked{ (pa.y >= (pb.y + pb.h)) || (pb.y >= (pa.y + pa.h)) };
        if (!stacked) { continue; }
        for (uint32_t const end : { e.src, e.dst }) {
          OrderNode const &nd{ o.nodes[end] };
          if ((nd.kind != OrderKind::State) ||
              (c.states[nd.subject].submachines.len != 0)) {
            shape.drawable = false;
          }
        }
      }
      for (uint32_t k = 0; (k < chunks.size()) && (chunks.size() > 1); ++k) {
        shape.packed.push_back({ .rank = global_rank[chunks[k]],
                                 .x = packed.at[k].x,
                                 .y = packed.at[k].y,
                                 .w = packed.at[k].w,
                                 .h = packed.at[k].h,
                                 .carried = static_cast<int32_t>(applied[k]) });
      }
      shape.w = packed.w;
      shape.h = packed.h;
      shape.ok = fits(packed);
      for (uint32_t k = 1; k < packed.at.size(); ++k) {
        shape.wraps = shape.wraps || (packed.at[k].y != packed.at[0].y) ||
                      (packed.at[k].x <= packed.at[k - 1].x);
      }
      // Returns before adding piece offsets to a shape outside the domain.
      if (!shape.ok) { return; }
      for (uint32_t i = 0; i < nodes.size(); ++i) {
        scav_rect const &at{ packed.at[piece_of[i]] };
        shape.at[i].x += at.x;
        shape.at[i].y += at.y;
      }
    };

    Wide area{ 0 };
    int32_t widest{ 0 };
    for (uint32_t r = 0; r < layers; ++r) {
      area += (Wide{ layer_w[r] } + p.rank_sep + boundary_gap(r)) * layer_h[r];
      widest = imax(widest, layer_w[r]);
    }
    Wide const target{ imax(Wide{ widest },
                            static_cast<Wide>(isqrt(static_cast<uint64_t>(
                                floor_div(area * dar.num, Wide{ dar.den }))))) };

    auto const better = [&](Shape const &a, Shape const &b) {
      return pack_better(
          { .at = {}, .w = static_cast<int32_t>(a.w), .h = static_cast<int32_t>(a.h) },
          { .at = {}, .w = static_cast<int32_t>(b.w), .h = static_cast<int32_t>(b.h) },
          dar.num,
          dar.den,
          p.sm_tiebreak != 0);
    };
    // A wrap width past any run; a frame running down never folds.
    Wide const unwrapped{ Wide{ COORD_MAX } * 2 };
    Shape &best{ sc.best };
    Shape &folded{ sc.folded };
    Shape &stacked{ sc.stacked };
    Fold const rule{ fold_of(m) };
    bool const always{ rule == Fold::Always };
    if ((id == 0) && (m < o.sub_fold.size()) && (o.sub_fold[m] != 0)) {
      trace_emit({ .kind = TraceKind::FoldPinned,
                   .pass = static_cast<uint16_t>(rule),
                   .frame = m,
                   .fold = { .rank = cut_of(m), .refused = 0, .carried = 0 } });
    }
    lay_out(best, unwrapped, 0, false);
    if (rule != Fold::Never) {
      uint32_t const cut_at{ down ? 0U : cut_of(m) };
      lay_out(folded, down ? unwrapped : target, cut_at, false);
      lay_out(stacked, down ? unwrapped : target, cut_at, true);
    }
    // Keeps the better of the packed and stacked folds in `folded`, preferring a usable
    // then a drawable one.
    auto const usable = [always](Shape const &f) {
      return f.ok && f.wraps && (f.drawable || always);
    };
    if ((rule != Fold::Never) && usable(stacked) &&
        (!usable(folded) || (stacked.drawable && !folded.drawable) ||
         ((stacked.drawable == folded.drawable) && better(stacked, folded)))) {
      std::swap(folded, stacked);
    }
    // Takes the fold if the flat run fails, or if it wraps and `Always` holds or it is
    // drawable and scales better.
    bool const swap{
      (rule != Fold::Never) && folded.ok &&
      (!best.ok || (folded.wraps && (always || (folded.drawable && better(folded, best)))))
    };
    if (swap) { std::swap(best, folded); }
    if (best.wraps) { out.folded[m] = 1; }
    for (TraceFold const &cut : best.cuts) {
      trace_emit({ .kind = TraceKind::FoldCut, .frame = m, .fold = cut });
    }
    for (TraceCarry const &ride : best.carried) {
      trace_emit({ .kind = TraceKind::BoundaryCarried, .frame = m, .carry = ride });
    }
    for (TracePiece const &piece : best.packed) {
      trace_emit({ .kind = TraceKind::PiecePacked, .frame = m, .piece = piece });
    }
    for (TraceShift const &shift : best.centred) {
      trace_emit({ .kind = TraceKind::ColumnCentred, .frame = m, .shift = shift });
    }
    for (Seated const &seat : best.seated) {
      trace_emit({ .kind = TraceKind::PseudostateSeated,
                   .pass = static_cast<uint16_t>(seat.how),
                   .frame = m,
                   .rank = { .state = seat.state, .rank = 0 } });
    }
    for (TraceGap const &gap : best.lanes) {
      trace_emit({ .kind = TraceKind::GapCharged,
                   .pass = static_cast<uint16_t>(GapCause::Lanes),
                   .frame = m,
                   .gap = gap });
    }
    if (!best.ok) {
      overflow(diags, ElemKind::Submachine, m);
      ok = false;
      return;
    }
    // Inside an owner, a state an outer self-loop leaves sits `box_clearance` in from the
    // component's edges, the loop's room off the owner's band.
    Wide lead{ 0 };
    Wide trail{ 0 };
    Wide top{ 0 };
    Wide bottom{ 0 };
    for (uint32_t i = 0; (c.submachines[m].owner.v != INVALID) && (i < nodes.size());
         ++i) {
      OrderNode const &nd{ o.nodes[span.off + nodes[i]] };
      if ((nd.kind != OrderKind::State) || (looped[nd.subject] == 0)) { continue; }
      Wide const a_lo{ best.at[i].x };
      Wide const c_lo{ Wide{ best.at[i].y } - (across(nd.subject) / 2) };
      lead = imax(lead, box_clearance(p) - a_lo);
      trail = imax(trail, (a_lo + along(nd.subject) + box_clearance(p)) - best.w);
      top = imax(top, box_clearance(p) - c_lo);
      bottom = imax(bottom, (c_lo + across(nd.subject) + box_clearance(p)) - best.h);
    }
    boxes[id] = {
      .x = 0,
      .y = 0,
      .w = static_cast<int32_t>(imin(best.w + lead + trail, Wide{ COORD_MAX })),
      .h = static_cast<int32_t>(imin(best.h + top + bottom, Wide{ COORD_MAX }))
    };
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      local[nodes[i]] = { .x = static_cast<int32_t>(best.at[i].x + lead),
                          .y = static_cast<int32_t>(best.at[i].y + top) };
    }
    for (uint32_t k = 0; k < espan.len; ++k) {
      uint32_t const seg{ o.edges[espan.off + k].segment };
      if ((best.lean[k] != 0) && (seg < out.lean.size())) { out.lean[seg] = 1; }
    }
  }

  place_sub(m, down, dar, boxes, sc.component, local);
}

// Packs the frame's components, then places each node and state, mapping along and
// across the ranks to x and y.
void Sizer::place_sub(uint32_t m,
                      bool down,
                      FrameDar dar,
                      Vector<scav_rect> const &boxes,
                      Vector<uint32_t> const &component,
                      Vector<scav_point> const &local) {
  Span const span{ o.sub_nodes[m] };
  Span const espan{ o.sub_edges[m] };
  Packing &packed{ sc.placed };
  pack_best(packed, sc.row, boxes, p.node_sep, p, dar, compaction);
  if (!fits(packed)) {
    overflow(diags, ElemKind::Submachine, m);
    ok = false;
    return;
  }
  out.sub[m].w = down ? packed.h : packed.w;
  out.sub[m].h = down ? packed.w : packed.h;

  Vector<uint32_t> &out_deg{ sc.out_deg };
  out_deg.assign(span.len, 0);
  for (uint32_t k = 0; k < espan.len; ++k) {
    ++out_deg[o.edges[espan.off + k].src - span.off];
  }

  for (uint32_t k = 0; k < span.len; ++k) {
    scav_rect const &at{ packed.at[component[k]] };
    OrderNode const &nd{ o.nodes[span.off + k] };
    int32_t lead{ local[k].x + at.x };  // along the ranks
    if (nd.kind == OrderKind::Boundary) {
      // A source sits on the frame's leading edge, a sink on its trailing edge.
      lead = (out_deg[k] != 0) ? 0 : packed.w;
    }
    int32_t const cross{ local[k].y + at.y };  // across them, a centre
    out.node[span.off + k] =
        down ? scav_point{ .x = cross, .y = lead } : scav_point{ .x = lead, .y = cross };
    if (nd.kind == OrderKind::State) {
      scav_rect &box{ out.state[nd.subject] };
      box.x = down ? (cross - (box.w / 2)) : lead;
      box.y = down ? lead : (cross - (box.h / 2));
    }
  }
  level_rank_ports(m, down);

  // A port on a cross border sits on the frame's edge across the ranks, and along them
  // level with where its flat edge meets the state it joins.
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    bool const at_src{ on_cross_border(e.src) };
    uint32_t const port{ at_src ? e.src : e.dst };
    if (!on_cross_border(port)) { continue; }
    uint32_t const other{ at_src ? e.dst : e.src };
    OrderNode const &nn{ o.nodes[other] };
    Wide along_at{ down ? out.node[other].y : out.node[other].x };
    if (nn.kind == OrderKind::State) {
      along_at += (down ? out.state[nn.subject].h : out.state[nn.subject].w) / 2;
      along_at += attach_at(e.segment, nn.subject, !down);
    }
    int32_t const lead{ static_cast<int32_t>(along_at) };
    int32_t const cross{ (cross_of(o.nodes[port].subject) == 1) ? 0 : packed.h };
    out.node[port] =
        down ? scav_point{ .x = cross, .y = lead } : scav_point{ .x = lead, .y = cross };
  }
}

// Moves a rank-border port and its chain's bends across the ranks to where its route meets
// the far state, where no other node lies in the band it sweeps.
void Sizer::level_rank_ports(uint32_t m, bool down) {
  Span const span{ o.sub_nodes[m] };
  Span const espan{ o.sub_edges[m] };
  Wide const clear{ route_clearance(p) };
  Wide const bumper{ box_clearance(p) };
  Wide const pitch{ label_line_height(p) };
  auto const lead = [&](uint32_t n) {
    return Wide{ down ? out.node[n].y : out.node[n].x };
  };
  auto const cross = [&](uint32_t n) -> int32_t & {
    return down ? out.node[n].x : out.node[n].y;
  };
  auto const chained = [&](uint32_t n, uint32_t seg) {
    return (o.nodes[n].kind == OrderKind::Bend) && (o.nodes[n].subject == seg);
  };
  for (uint32_t b = span.off; b < (span.off + span.len); ++b) {
    if ((o.nodes[b].kind != OrderKind::Boundary) || on_cross_border(b)) { continue; }
    uint32_t const seg{ o.nodes[b].subject };
    uint32_t far{ INVALID };
    for (uint32_t k = 0; k < espan.len; ++k) {
      OrderEdge const &e{ o.edges[espan.off + k] };
      if (e.segment != seg) { continue; }
      for (uint32_t const end : { e.src, e.dst }) {
        if (o.nodes[end].kind == OrderKind::State) { far = end; }
      }
    }
    if (far == INVALID) { continue; }
    uint32_t const st{ o.nodes[far].subject };
    scav_rect const &box{ out.state[st] };
    Wide const lo{ down ? box.x : box.y };
    Wide const len{ down ? box.w : box.h };
    StateKind const kind{ c.states[st].kind };
    Wide const was{ cross(b) };
    Wide to{ lo + (len / 2) };
    if (ported(seg, st)) {
      int32_t at{ 0 };
      if (!port_at(seg, st, down, at)) { continue; }
      to += at;
    } else if (!kind_inscribed(kind)) {
      Wide const inset{ imin(imax(clear, Wide{ state_corner_radius(kind, box, p.pad) }),
                             len / 2) };
      to = imin(imax(was, lo + inset), (lo + len) - inset);
    }

    // Along the ranks, the stretch between the border and the state's near
    // face; across them, what the port and its bends sweep through.
    bool const leading{ lead(b) <= lead(far) };
    Wide const near{ leading ? lead(far) : (lead(far) + (down ? box.h : box.w)) };
    Wide const from_a{ imin(lead(b), near) };
    Wide const to_a{ imax(lead(b), near) };
    Wide band_lo{ imin(was, to) };
    Wide band_hi{ imax(was, to) };
    bool inside{ true };
    for (uint32_t n = span.off; n < (span.off + span.len); ++n) {
      if (!chained(n, seg)) { continue; }
      band_lo = imin(band_lo, Wide{ cross(n) });
      band_hi = imax(band_hi, Wide{ cross(n) });
      inside = inside && (lead(n) >= from_a) && (lead(n) <= to_a);
    }
    if (!inside || (band_lo == band_hi)) { continue; }
    bool free{ true };
    for (uint32_t n = span.off; free && (n < (span.off + span.len)); ++n) {
      OrderNode const &nd{ o.nodes[n] };
      if ((n == b) || (n == far) || chained(n, seg) || on_cross_border(n)) { continue; }
      if (nd.kind == OrderKind::State) {
        scav_rect const &r{ out.state[nd.subject] };
        Wide const a{ down ? r.y : r.x };
        Wide const a_len{ down ? r.h : r.w };
        Wide const cr{ down ? r.x : r.y };
        Wide const cr_len{ down ? r.w : r.h };
        free = ((a + a_len) <= from_a) || (a >= to_a) ||
               ((cr + cr_len + bumper) <= band_lo) || ((cr - bumper) >= band_hi);
        continue;
      }
      Wide const at{ cross(n) };
      free = (lead(n) < from_a) || (lead(n) > to_a) || ((at + pitch) <= band_lo) ||
             ((at - pitch) >= band_hi);
    }
    if (!free) { continue; }
    for (uint32_t n = span.off; n < (span.off + span.len); ++n) {
      if ((n == b) || chained(n, seg)) { cross(n) = static_cast<int32_t>(to); }
    }
  }
}

// `lay_out_sub` through a memo keyed on every value it reads, with frame-local node
// indices; a traced run lays every frame out.
void Sizer::size_sub(uint32_t m) {
  if (trace_sink() != nullptr) {
    lay_out_sub(m);
    return;
  }
  Span const span{ o.sub_nodes[m] };
  Span const espan{ o.sub_edges[m] };
  Span const gspan{ o.sub_gaps[m] };
  bool const down{ runs_down(m) };
  FrameDar const dar{ owner_dar(m) };
  thread_local Vector<uint32_t> key;
  thread_local Vector<int32_t> value;
  key.clear();
  auto const put = [](int32_t v) { key.push_back(static_cast<uint32_t>(v)); };
  key.push_back(span.len);
  key.push_back(o.sub_ranks[m]);
  key.push_back(down ? 1U : 0U);
  put(dar.num);
  put(dar.den);
  key.push_back(static_cast<uint32_t>(compaction));
  key.push_back(static_cast<uint32_t>(fold_of(m)));
  key.push_back(cut_of(m));
  key.push_back(g.serial);
  key.push_back(profile_word);
  key.push_back((c.submachines[m].owner.v != INVALID) ? 1U : 0U);
  for (uint32_t k = 0; k < span.len; ++k) {
    OrderNode const &nd{ o.nodes[span.off + k] };
    key.push_back(static_cast<uint32_t>(nd.kind));
    key.push_back(nd.subject);
    key.push_back(nd.rank);
    key.push_back(nd.pos);
    if (nd.kind == OrderKind::Boundary) { key.push_back(cross_of(nd.subject)); }
    if (nd.kind != OrderKind::State) { continue; }
    put(out.state[nd.subject].w);
    put(out.state[nd.subject].h);
    if (g.serial != 0) { continue; }  // the chart a serial names fixes these three
    key.push_back(static_cast<uint32_t>(c.states[nd.subject].kind));
    key.push_back(c.states[nd.subject].submachines.len);
    key.push_back(looped[nd.subject]);
  }
  key.push_back(espan.len);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    key.push_back(e.src - span.off);
    key.push_back(e.dst - span.off);
    key.push_back(e.segment);
    key.push_back(e.reversed);
    bool const has{ e.segment < seg_label_h.size() };
    put(has ? seg_label_h[e.segment] : 0);
    put(has ? seg_label_w[e.segment] : 0);
    for (uint32_t const end : { e.src, e.dst }) {
      OrderNode const &nd{ o.nodes[end] };
      bool const state{ nd.kind == OrderKind::State };
      int32_t at{ 0 };
      int32_t across_at{ 0 };
      bool const faced{ state && port_at(e.segment, nd.subject, down, at) };
      bool const flat_faced{ state && port_at(e.segment, nd.subject, !down, across_at) };
      key.push_back(((state && ported(e.segment, nd.subject)) ? 1U : 0U) |
                    (faced ? 2U : 0U) | (flat_faced ? 4U : 0U));
      put(at);
      put(across_at);
    }
  }
  key.push_back(gspan.len);
  bool const labelled{ o.labels.size() == o.gaps.size() };
  for (uint32_t k = 0; k < gspan.len; ++k) {
    put(o.gaps[gspan.off + k]);
    put(labelled ? o.labels[gspan.off + k] : o.gaps[gspan.off + k]);
  }

  // The memo value: the frame's extent, whether it folded, each node's and state's place,
  // and each edge's lean.
  Memo &memo{ frame_memo() };
  int32_t const *hit{ nullptr };
  uint32_t len{ 0 };
  auto const seg_of = [&](uint32_t k) { return o.edges[espan.off + k].segment; };
  if (memo.find(key, hit, len)) {
    out.sub[m].w = hit[0];
    out.sub[m].h = hit[1];
    out.folded[m] = static_cast<uint8_t>(hit[2]);
    uint32_t at{ 3 };
    for (uint32_t k = 0; k < span.len; ++k) {
      OrderNode const &nd{ o.nodes[span.off + k] };
      out.node[span.off + k] = { .x = hit[at], .y = hit[at + 1] };
      at += 2;
      if (nd.kind != OrderKind::State) { continue; }
      out.state[nd.subject].x = hit[at];
      out.state[nd.subject].y = hit[at + 1];
      at += 2;
    }
    for (uint32_t k = 0; k < espan.len; ++k) {
      if ((hit[at + k] != 0) && (seg_of(k) < out.lean.size())) { out.lean[seg_of(k)] = 1; }
    }
    return;
  }
  bool const was{ ok };
  lay_out_sub(m);
  if (!was || !ok) { return; }
  value.clear();
  value.push_back(out.sub[m].w);
  value.push_back(out.sub[m].h);
  value.push_back(out.folded[m]);
  for (uint32_t k = 0; k < span.len; ++k) {
    OrderNode const &nd{ o.nodes[span.off + k] };
    value.push_back(out.node[span.off + k].x);
    value.push_back(out.node[span.off + k].y);
    if (nd.kind != OrderKind::State) { continue; }
    value.push_back(out.state[nd.subject].x);
    value.push_back(out.state[nd.subject].y);
  }
  for (uint32_t k = 0; k < espan.len; ++k) {
    value.push_back((seg_of(k) < out.lean.size()) ? out.lean[seg_of(k)] : 0);
  }
  memo.insert(key, value);
}

void Sizer::size_state(uint32_t i) {
  Vector<scav_rect> &kids{ sc.kids };
  kids.clear();
  Vector<uint32_t> &ids{ sc.ids };
  ids.clear();
  Span const subs{ c.states[i].submachines };
  for (uint32_t k = 0; k < subs.len; ++k) {
    uint32_t const m{ c.submachine_ids[subs.off + k].v };
    if (c.submachines[m].live == 0) { continue; }
    kids.push_back(out.sub[m]);
    ids.push_back(m);
  }
  Packing &packed{ sc.placed };
  packed.at.clear();
  packed.w = 0;
  packed.h = 0;
  if (!kids.empty()) {
    pack_best(packed, sc.row, kids, p.sub_sep, p, dar_of(i), compaction);
    for (uint32_t k = 0; k < ids.size(); ++k) {
      uint32_t const m{ ids[k] };
      sub_local[m] = { .x = packed.at[k].x, .y = packed.at[k].y };
      // Whitespace elimination may grow the frame; boundary nodes on its trailing rank or
      // cross border move to the grown edge.
      Span const span{ o.sub_nodes[m] };
      for (uint32_t u = 0; u < span.len; ++u) {
        if (o.nodes[span.off + u].kind != OrderKind::Boundary) { continue; }
        scav_point &at{ out.node[span.off + u] };
        uint8_t const cross{ cross_of(o.nodes[span.off + u].subject) };
        bool const by_y{ runs_down(m) != (cross != 0) };
        if (cross == 1) { continue; }
        if (by_y && (at.y == out.sub[m].h)) { at.y = packed.at[k].h; }
        if (!by_y && (at.x == out.sub[m].w)) { at.x = packed.at[k].w; }
      }
      out.sub[m].w = packed.at[k].w;
      out.sub[m].h = packed.at[k].h;
    }
  }

  scav_box_space const b{ box_of(s.box_state, s.n_box_state, i) };
  uint32_t const kind{ static_cast<uint32_t>(c.states[i].kind) };
  // A fork or join bar in a frame running down swaps its minimum width and height.
  StateKind const sk{ c.states[i].kind };
  bool const lies{ ((sk == StateKind::Fork) || (sk == StateKind::Join)) &&
                   runs_down(c.states[i].parent.v) };
  int32_t const min_w{ lies ? p.kind_min_h[kind] : p.kind_min_w[kind] };
  int32_t const min_h{ lies ? p.kind_min_w[kind] : p.kind_min_h[kind] };
  Wide const ring{ bare(b, i) ? Wide{ 0 } : (2 * static_cast<Wide>(p.pad)) };
  scav_extent const room{ room_of(i) };
  std::array<int32_t, 4> const gap{ band_gaps(b, i) };
  Wide const centre{ Wide{ b.w_before } + gap[0] + imax(Wide{ packed.w }, Wide{ room.w }) +
                     gap[1] + b.w_after };
  Wide const body{ Wide{ packed.h } + room.h +
                   (((packed.h > 0) && (room.h > 0)) ? p.sub_sep : 0) };
  Wide const w{ imax(imax(Wide{ b.min_w }, centre), Wide{ min_w }) + ring };
  Wide const h{
    imax(Wide{ b.h_before } + gap[2] + body + gap[3] + b.h_after, Wide{ min_h }) + ring
  };
  if ((w > COORD_MAX) || (h > COORD_MAX)) {
    overflow(diags, ElemKind::State, i);
    ok = false;
    return;
  }
  out.state[i].w = static_cast<int32_t>(w);
  out.state[i].h = static_cast<int32_t>(h);
}

// One whole sizing. `hole[i]` is the ratio packings inside state `i` aim at; `num` 0 or
// a short vector falls back to the profile's ratio.
bool size_pass(Chart const &c,
               SplitGraph const &g,
               SubmachineOrders const &o,
               scav_spaces const &s,
               scav_profile const &p,
               Vector<FrameDar> const &hole,
               Compaction compaction,
               Fold fold,
               SizedLayout &out,
               std::vector<Diagnostic> &diags) {
  Sizer x{ .c = c,
           .g = g,
           .o = o,
           .s = s,
           .p = p,
           .hole = hole,
           .compaction = compaction,
           .fold = fold,
           .out = out,
           .diags = diags };
  out.state.assign(c.states.size(), {});
  out.before.assign(c.states.size(), {});
  out.after.assign(c.states.size(), {});
  out.lead.assign(c.states.size(), {});
  out.trail.assign(c.states.size(), {});
  out.loop.assign(c.states.size(), {});
  out.loop_place.assign(c.states.size(), 0);
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    uint8_t const pinned{ (i < o.state_loop.size()) ? o.state_loop[i] : uint8_t{ 0 } };
    LoopPlace const d{ loop_place_default(s, i) };
    out.loop_place[i] =
        static_cast<uint8_t>((pinned != 0) ? (pinned - 1U) : ((d.face * 2) + d.end));
  }
  out.sub.assign(c.submachines.size(), {});
  out.node.assign(o.nodes.size(), {});
  out.lean.assign(g.segments.size(), 0);
  out.folded.assign(c.submachines.size(), 0);
  out.chart = {};
  Vector<scav_point> &sub_local{ x.sub_local };
  sub_local.assign(c.submachines.size(), scav_point{});

  uint32_t max_depth{ 0 };
  for (uint32_t const d : g.state_depth) { max_depth = imax(max_depth, d); }
  std::vector<Vector<uint32_t>> &states_at{ x.sc.states_at };
  clear_buckets(states_at, max_depth + 1);
  std::vector<Vector<uint32_t>> &subs_at{ x.sc.subs_at };
  clear_buckets(subs_at, max_depth + 2);
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (c.states[i].live != 0) { states_at[g.state_depth[i]].push_back(i); }
  }
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    StateId const owner{ c.submachines[m].owner };
    subs_at[(owner.v == INVALID) ? 0 : g.state_depth[owner.v] + 1].push_back(m);
  }

  x.seg_label_h.assign(g.segments.size(), 0);
  x.seg_label_w.assign(g.segments.size(), 0);
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    scav_path_box const &box{ s.path_box[i] };
    uint32_t const at{ label_segment(c, g, box.subject) };
    if (at == INVALID) { continue; }
    bool const down{ x.runs_down(g.segments[at].frame.v) };
    x.seg_label_h[at] = imax(x.seg_label_h[at], down ? box.w : box.h);
    x.seg_label_w[at] = imax(x.seg_label_w[at], down ? box.h : box.w);
  }
  loop_rooms(c, s, p, out.loop_place, x.loop_label, x.loop_room);
  x.looped.assign(c.states.size(), 0);
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    Transition const &tr{ c.transitions[t] };
    if ((t < g.trans_segments.size()) && (g.trans_segments[t].len != 0) &&
        (tr.src == tr.dst) && !inner_loop(c, t)) {
      x.looped[tr.src.v] = 1;
    }
  }
  x.port_seg.assign(g.ports.size(), INVALID);
  for (uint32_t seg = 0; seg < o.seg_port.size(); ++seg) {
    if (o.seg_port[seg] < x.port_seg.size()) { x.port_seg[o.seg_port[seg]] = seg; }
  }

  // Levels interleave: the submachines whose children sit at this depth, then
  // the states one level up that wrap them; level 0 sizes the document roots.
  for (uint32_t level = max_depth + 2; level-- > 0;) {
    for (uint32_t const m : subs_at[level]) { x.size_sub(m); }
    if (level > 0) {
      for (uint32_t const i : states_at[level - 1]) { x.size_state(i); }
    }
  }
  if (!x.ok) { return false; }

  // One descent from the root, adding each frame's origin; every sum stays inside the
  // sized extents.
  Vector<Frame> &work{ x.sc.work };
  work.clear();
  if (c.root_submachine.v != INVALID) {
    out.chart = out.sub[c.root_submachine.v];
    work.push_back({ .sub = c.root_submachine.v, .x = 0, .y = 0 });
  }
  while (!work.empty()) {
    Frame const at{ work.back() };
    work.pop_back();
    out.sub[at.sub].x = at.x;
    out.sub[at.sub].y = at.y;
    Span const span{ o.sub_nodes[at.sub] };
    for (uint32_t k = 0; k < span.len; ++k) {
      out.node[span.off + k].x += at.x;
      out.node[span.off + k].y += at.y;
      OrderNode const &nd{ o.nodes[span.off + k] };
      // Root-absolute, as the router receives it.
      trace_emit(
          { .kind = TraceKind::NodePlaced,
            .frame = at.sub,
            .place = { .state = (nd.kind == OrderKind::State) ? nd.subject : INVALID,
                       .seg = (nd.kind == OrderKind::State) ? INVALID : nd.subject,
                       .x = out.node[span.off + k].x,
                       .y = out.node[span.off + k].y } });
      if (nd.kind != OrderKind::State) { continue; }
      uint32_t const i{ nd.subject };
      scav_rect &r{ out.state[i] };
      r.x += at.x;
      r.y += at.y;

      scav_box_space const b{ box_of(s.box_state, s.n_box_state, i) };
      int32_t const pad{ x.bare(b, i) ? 0 : p.pad };
      int32_t const ix{ r.x + pad };
      int32_t const iw{ r.w - (2 * pad) };
      out.before[i] = { .x = ix, .y = r.y + pad, .w = iw, .h = b.h_before };
      std::array<int32_t, 4> const gap{ x.band_gaps(b, i) };
      int32_t const sy{ r.y + pad + b.h_before + gap[2] };
      int32_t packed_h{ 0 };
      Span const subs{ c.states[i].submachines };
      for (uint32_t u = 0; u < subs.len; ++u) {
        uint32_t const m{ c.submachine_ids[subs.off + u].v };
        if (c.submachines[m].live == 0) { continue; }
        packed_h = imax(packed_h, sub_local[m].y + out.sub[m].h);
      }
      scav_extent const room{ x.room_of(i) };
      int32_t const sub_y{ sy + x.room_shift(i, packed_h) };
      for (uint32_t u = 0; u < subs.len; ++u) {
        uint32_t const m{ c.submachine_ids[subs.off + u].v };
        if (c.submachines[m].live == 0) { continue; }
        work.push_back({ .sub = m,
                         .x = ix + b.w_before + gap[0] + sub_local[m].x,
                         .y = sub_y + sub_local[m].y });
      }
      int32_t const sep{ ((packed_h > 0) && (room.h > 0)) ? p.sub_sep : 0 };
      int32_t const body{ (room.h > 0) ? (packed_h + sep + room.h) : packed_h };
      int32_t const centre_end{ (ix + iw) - b.w_after };
      // The side bands run between the other two, across the gaps.
      int32_t const side_y{ sy - gap[2] };
      int32_t const side_h{ gap[2] + body + gap[3] };
      out.lead[i] = { .x = ix, .y = side_y, .w = b.w_before, .h = side_h };
      out.trail[i] = { .x = centre_end, .y = side_y, .w = b.w_after, .h = side_h };
      out.after[i] = { .x = ix, .y = sy + body + gap[3], .w = iw, .h = b.h_after };
      LoopPlace const place{ loop_place(out, i) };
      bool const leading{ (place.face == 0) || ((place.face >= 2) && (place.end == 0)) };
      int32_t const room_x{ leading ? (ix + b.w_before + gap[0])
                                    : (centre_end - gap[1] - room.w) };
      int32_t const room_y{ (x.room_shift(i, packed_h) > 0) ? sy
                                                            : ((sy + body) - room.h) };
      out.loop[i] = { .x = room_x, .y = room_y, .w = room.w, .h = room.h };
    }
  }
  return true;
}

}  // namespace

SCAV_INTERNAL_BEGIN

// A hole's aspect as a pair in `[1, 1024]`: the longer axis takes 1024, the shorter
// floors at 1; `num` 0 for a hole with no extent on an axis.
FrameDar size_hole_ratio(int32_t w, int32_t h) {
  constexpr Wide CAP{ 1024 };
  if ((w <= 0) || (h <= 0)) { return {}; }
  Wide const num{ (w >= h) ? CAP : imax(floor_div(CAP * w, Wide{ h }), Wide{ 1 }) };
  Wide const den{ (w >= h) ? imax(floor_div(CAP * h, Wide{ w }), Wide{ 1 }) : CAP };
  return { .num = static_cast<int32_t>(num), .den = static_cast<int32_t>(den) };
}

// Per state, the aspect of the hole its submachines pack into: inside its bands, above
// any loop room. `num` 0 for a state with no live submachine.
void size_owner_holes(Chart const &c,
                      SizedLayout const &z,
                      int32_t sep,
                      Vector<FrameDar> &hole) {
  hole.assign(c.states.size(), FrameDar{});
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (c.states[i].live == 0) { continue; }
    bool packed{ false };
    Span const subs{ c.states[i].submachines };
    for (uint32_t k = 0; k < subs.len; ++k) {
      packed = packed || (c.submachines[c.submachine_ids[subs.off + k].v].live != 0);
    }
    if (!packed) { continue; }
    // The ring: `pad`, or 0 for a bare pseudostate.
    int32_t const pad{ z.before[i].y - z.state[i].y };
    // Each band keeps `sep` from the contents beside it.
    auto const gap = [sep](int32_t band) { return (band > 0) ? sep : 0; };
    int32_t top{ z.before[i].y + z.before[i].h + gap(z.before[i].h) };
    int32_t bottom{ (z.state[i].y + z.state[i].h) - pad - z.after[i].h -
                    gap(z.after[i].h) };
    if ((i < z.loop.size()) && (z.loop[i].h > 0)) {
      bool const above{ z.loop[i].y == top };
      top = above ? (z.loop[i].y + z.loop[i].h) : top;
      bottom = above ? bottom : z.loop[i].y;
    }
    int32_t const lead{ (i < z.lead.size()) ? z.lead[i].w : 0 };
    int32_t const trail{ (i < z.trail.size()) ? z.trail[i].w : 0 };
    int32_t const sides{ lead + gap(lead) + trail + gap(trail) };
    hole[i] = size_hole_ratio(z.before[i].w - sides, bottom - top);
  }
}

SCAV_INTERNAL_END

namespace {

// Clamps `v` to COORD_MAX + 1, which the box formula still rejects.
int32_t saturate(Wide v) { return static_cast<int32_t>(imin(v, Wide{ COORD_MAX } + 1)); }

}  // namespace

std::array<scav_rect, 5> state_walls(SizedLayout const &z, uint32_t st) {
  auto const row = [st](Vector<scav_rect> const &v) {
    return (st < v.size()) ? v[st] : scav_rect{};
  };
  scav_rect const top{ row(z.before) };
  scav_rect const bottom{ row(z.after) };
  auto const sealed = [&](scav_rect side) {
    if ((side.w <= 0) || (top.w <= 0)) { return side; }
    side.y = top.y;
    side.h = (bottom.y + bottom.h) - top.y;
    return side;
  };
  scav_rect room{ row(z.loop) };
  if ((room.w > 0) && (room.h > 0)) {  // out to the boundary its loops' legs reach
    uint32_t const face{ loop_place(z, st).face };
    int32_t const edge{ loop_boundary(z, st, face) };
    int32_t const x1{ (face == 0) ? (room.x + room.w) : imax(room.x + room.w, edge) };
    int32_t const y1{ (face == 2) ? (room.y + room.h) : imax(room.y + room.h, edge) };
    room.x = (face == 0) ? edge : room.x;
    room.y = (face == 2) ? edge : room.y;
    room.w = ((face < 2) ? x1 : (room.x + room.w)) - room.x;
    room.h = ((face >= 2) ? y1 : (room.y + room.h)) - room.y;
  }
  return { top, bottom, sealed(row(z.lead)), sealed(row(z.trail)), room };
}

namespace {

// Bands in face order (left, right, top, bottom) and `ruled`'s bit for each.
constexpr std::array<uint32_t, 4> RULED_BIT{ 4U, 8U, 1U, 2U };

bool anchored(std::array<int32_t, 4> const &band, uint32_t ruled, uint32_t face) {
  return (face < 4) && ((band[face] == 0) || ((ruled & RULED_BIT[face]) != 0));
}

// The trailing end of the first anchored face of right, left, bottom, top; the right face
// where none is.
LoopPlace first_anchored(std::array<int32_t, 4> const &band, uint32_t ruled) {
  for (uint32_t const face : { 1U, 0U, 3U, 2U }) {
    if (anchored(band, ruled, face)) { return { .face = face, .end = 1 }; }
  }
  return {};
}

std::array<int32_t, 4> bands_of(scav_box_space const &b) {
  return { b.w_before, b.w_after, b.h_before, b.h_after };
}

}  // namespace

LoopPlace loop_place_default(scav_spaces const &s, uint32_t state) {
  scav_box_space const b{ box_of(s.box_state, s.n_box_state, state) };
  return first_anchored(bands_of(b), b.ruled);
}

bool loop_anchored(scav_spaces const &s, uint32_t state, uint32_t face) {
  scav_box_space const b{ box_of(s.box_state, s.n_box_state, state) };
  return anchored(bands_of(b), b.ruled, face);
}

LoopPlace loop_place(SizedLayout const &z, uint32_t st) {
  if (st < z.loop_place.size()) {
    return { .face = z.loop_place[st] / 2U, .end = z.loop_place[st] % 2U };
  }
  auto const w = [st](Vector<scav_rect> const &v) {
    return (st < v.size()) ? v[st].w : 0;
  };
  auto const h = [st](Vector<scav_rect> const &v) {
    return (st < v.size()) ? v[st].h : 0;
  };
  return first_anchored({ w(z.lead), w(z.trail), h(z.before), h(z.after) }, 0);
}

int32_t loop_boundary(SizedLayout const &z, uint32_t st, uint32_t face) {
  auto const row = [st](Vector<scav_rect> const &v) {
    return (st < v.size()) ? v[st] : scav_rect{};
  };
  scav_rect const box{ row(z.state) };
  switch (face) {
    case 0: {
      scav_rect const b{ row(z.lead) };
      return (b.w > 0) ? (b.x + b.w) : box.x;
    }
    case 1: {
      scav_rect const b{ row(z.trail) };
      return (b.w > 0) ? b.x : (box.x + box.w);
    }
    case 2: {
      scav_rect const b{ row(z.before) };
      return (b.h > 0) ? (b.y + b.h) : box.y;
    }
    default: {
      scav_rect const b{ row(z.after) };
      return (b.h > 0) ? b.y : (box.y + box.h);
    }
  }
}

SCAV_COLD bool loop_room_unmoved(Chart const &c,
                                 SizedLayout const &z,
                                 uint32_t st,
                                 uint32_t place) {
  if ((st >= c.states.size()) || (st >= z.loop.size()) || (st >= z.loop_place.size()) ||
      (st >= z.lead.size()) || (st >= z.trail.size()) ||
      ((place / 2U) != (z.loop_place[st] / 2U))) {
    return false;
  }
  // Top or bottom: either end when the room spans the width between the side bands.
  if ((place / 2U) >= 2) {
    return (Wide{ z.lead[st].x } + z.lead[st].w + z.loop[st].w) == Wide{ z.trail[st].x };
  }
  // Left or right: either end when no live submachine shares the body with the room.
  Span const subs{ c.states[st].submachines };
  for (uint32_t u = 0; u < subs.len; ++u) {
    if (c.submachines[c.submachine_ids[subs.off + u].v].live != 0) { return false; }
  }
  return true;
}

bool face_lined(scav_spaces const &s, uint32_t state, uint32_t face) {
  scav_box_space const b{ box_of(s.box_state, s.n_box_state, state) };
  return (face < 4) && (bands_of(b)[face] > 0);
}

int32_t loop_reach(scav_profile const &p) { return imax(p.pad, 1); }

int32_t loop_gap(scav_profile const &p) {
  return imin(imax(p.pad / 2, 1), label_leader(p));
}

int32_t loop_lane(scav_profile const &p) {
  return imax(label_line_height(p), 2 * route_clearance(p));
}

LoopRow loop_row(scav_profile const &p, scav_extent label, bool vertical) {
  int32_t const deep{ vertical ? label.h : label.w };
  int32_t const lane{ imax(loop_lane(p), vertical ? label.w : label.h) };
  return { .label_w = label.w,
           .label_h = label.h,
           .lane = lane,
           .cross =
               saturate(Wide{ lane } +
                        (Wide{ 2 } * route_clearance(p))),  // a clearance off each edge
           .along = ((deep > 0) ? (deep + loop_gap(p)) : 0) + loop_reach(p) };
}

void loop_labels(Chart const &c, scav_spaces const &s, Vector<scav_extent> &label) {
  label.assign(c.transitions.size(), scav_extent{});
  for (uint32_t i = 0; (s.path_box != nullptr) && (i < s.n_path_box); ++i) {
    uint32_t const t{ s.path_box[i].subject };
    if ((t >= c.transitions.size()) || !inner_loop(c, t)) { continue; }
    label[t].w = imax(label[t].w, s.path_box[i].w);
    label[t].h = saturate(Wide{ label[t].h } + s.path_box[i].h);
  }
}

void loop_rooms(Chart const &c,
                scav_spaces const &s,
                scav_profile const &p,
                Vector<uint8_t> const &place,
                Vector<scav_extent> &label,
                Vector<scav_extent> &room) {
  loop_labels(c, s, label);
  room.assign(c.states.size(), scav_extent{});
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if (!inner_loop(c, t)) { continue; }
    uint32_t const st{ c.transitions[t].src.v };
    bool const vertical{ (st < place.size()) && ((place[st] / 2U) >= 2) };
    LoopRow const row{ loop_row(p, label[t], vertical) };
    scav_extent &r{ room[st] };
    int32_t &across{ vertical ? r.w : r.h };
    int32_t &deep{ vertical ? r.h : r.w };
    deep = imax(deep, row.along);
    across = saturate(Wide{ across } + row.cross);
  }
}

void loop_rows(Chart const &c,
               SizedLayout const &z,
               scav_spaces const &s,
               scav_profile const &p,
               Vector<scav_extent> &label,
               Vector<scav_rect> &row) {
  loop_labels(c, s, label);
  row.assign(c.transitions.size(), scav_rect{});
  thread_local Vector<int32_t> cursor;
  cursor.assign(c.states.size(), 0);
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if (!inner_loop(c, t)) { continue; }
    uint32_t const st{ c.transitions[t].src.v };
    if (st >= z.loop.size()) { continue; }
    scav_rect const &r{ z.loop[st] };
    bool const vertical{ loop_place(z, st).face >= 2 };
    int32_t const across{ loop_row(p, label[t], vertical).cross };
    row[t] = vertical
                 ? scav_rect{ .x = r.x + cursor[st], .y = r.y, .w = across, .h = r.h }
                 : scav_rect{ .x = r.x, .y = r.y + cursor[st], .w = r.w, .h = across };
    cursor[st] += across;
  }
}

SCAV_COLD RowReads size_row_reads(Chart const &c, SubmachineOrders const &o) {
  RowReads out;
  Vector<uint32_t> root;  // union-find over one frame's nodes
  auto const find = [&root](uint32_t v) {
    while (root[v] != v) {
      root[v] = root[root[v]];
      v = root[v];
    }
    return v;
  };
  for (uint32_t m = 0; (m < c.submachines.size()) && (m < o.sub_nodes.size()); ++m) {
    Span const ns{ o.sub_nodes[m] };
    Span const es{ o.sub_edges[m] };
    if ((c.submachines[m].live == 0) || (ns.len == 0)) { continue; }
    bool const edged{ es.len != 0 };  // a component can take two layers
    root.resize(ns.len);
    for (uint32_t k = 0; k < ns.len; ++k) { root[k] = k; }
    uint32_t parts{ ns.len };
    for (uint32_t k = 0; k < es.len; ++k) {
      OrderEdge const &e{ o.edges[es.off + k] };
      uint32_t const a{ find(e.src - ns.off) };
      uint32_t const b{ find(e.dst - ns.off) };
      if (a != b) {
        root[a] = b;
        --parts;
      }
    }
    bool const packs{ edged || (parts >= 2) };
    out.fold = out.fold || edged;
    out.trybox = out.trybox || packs;
    out.pack = out.pack || packs;
    out.dar = out.dar || (packs && (c.submachines[m].owner.v != INVALID));
  }
  for (State const &st : c.states) {
    if (st.live == 0) { continue; }
    uint32_t regions{ 0 };
    for (uint32_t k = 0; k < st.submachines.len; ++k) {
      regions +=
          (c.submachines[c.submachine_ids[st.submachines.off + k].v].live != 0) ? 1U : 0U;
    }
    bool const packs{ regions >= 2 };
    out.trybox = out.trybox || packs;
    out.pack = out.pack || packs;
    out.dar = out.dar || packs;
  }
  return out;
}

SCAV_COLD Row size_row_canonical(Row const &row,
                                 RowReads const &reads,
                                 scav_profile const &base) {
  Row out{ row };
  if (!reads.trybox) { out.knobs.trybox = base.trybox; }
  if (!reads.pack) { out.pack = Compaction::Off; }
  if (!reads.dar) { out.dar = DarSource::Profile; }
  if (!reads.fold) { out.fold = Fold::Scale; }
  return out;
}

bool size_layout(Chart const &c,
                 SplitGraph const &g,
                 SubmachineOrders const &o,
                 scav_spaces const &s,
                 scav_profile const &p,
                 SizedLayout &out,
                 std::vector<Diagnostic> &diags,
                 DarSource dar,
                 Compaction compaction,
                 Fold fold) {
  if (dar == DarSource::Profile) {
    return size_pass(c, g, o, s, p, {}, compaction, fold, out, diags);
  }
  // Owner holes come off a first pass at the profile's ratio, packed the same way.
  SizedLayout &first{ size_scratch().first };
  if (!size_pass(c, g, o, s, p, {}, compaction, fold, first, diags)) { return false; }
  Vector<FrameDar> &hole{ size_scratch().hole };
  size_owner_holes(c, first, p.sub_sep, hole);
  return size_pass(c, g, o, s, p, hole, compaction, fold, out, diags);
}

}  // namespace scav
