// One pass per depth level, deepest first: a submachine needs its children sized
// and a state needs its submachines. Ranks give one axis, `cross_coordinates`
// the other; the box formula then sizes the state.

#include "layout/size.h"
#include "layout/trace.h"

#include "layout/coords.h"
#include "layout/geom.h"
#include "layout/memo.h"
#include "layout/pack.h"
#include "layout/router.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_thread.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace scav {

// Bracketed so a test reaches the hole a frame is handed without sizing a
// whole chart. The prototypes a test uses are its own; see scav_internal.h.
SCAV_INTERNAL_BEGIN
FrameDar size_hole_ratio(int32_t w, int32_t h);
std::vector<FrameDar> size_owner_holes(Chart const &c, SizedLayout const &z);
void size_owner_holes(Chart const &c, SizedLayout const &z, std::vector<FrameDar> &hole);
SCAV_INTERNAL_END

#ifdef SCAV_TESTING
void size_test_reuse(bool on);
void size_test_reuse_verify(bool on);
void size_test_reuse_ignore_dar(bool on);
uint64_t size_test_reused();
uint64_t size_test_framed();
uint64_t size_test_reuse_mismatches();
#endif

namespace {

#ifdef SCAV_TESTING
// Whether a pass copies frames from its `base`, whether a sizing that copied any is sized
// again without one to check it, whether the owner's ratio is left out of the comparison
// as a planted defect, and the frames copied, the frames compared and the sizings that
// came out different.
bool test_reuse{ true };
bool test_reuse_verify{ false };
bool test_reuse_ignore_dar{ false };
Mutex test_reuse_lock;
uint64_t test_reused{ 0 };
uint64_t test_framed{ 0 };
uint64_t test_reuse_mismatches{ 0 };
#endif

scav_box_space box_of(scav_box_space const *rows, uint32_t count, uint32_t i) {
  return ((rows != nullptr) && (i < count)) ? rows[i] : scav_box_space{};
}

// Every frame this thread has laid out, by what laying it out read. A search's
// candidates differ in one frame and the frames that hold it, so nearly every
// other frame a candidate sizes is one an earlier candidate already sized.
Memo &frame_memo() {
  thread_local Memo m{ size_t{ 1 } << 20 };
  return m;
}

// Inside the coordinate domain on both axes. A row or a column whose sum
// overflowed saturates at `PACK_SATURATED`, which is far past `COORD_MAX`.
bool fits(Packing const &p) { return (p.w <= COORD_MAX) && (p.h <= COORD_MAX); }

// The better-scaling of the two packings, among those inside the domain. A
// packing outside it cannot compose a box inside it, so it is no candidate.
// The ratio and the compaction knob are arguments rather than the profile's
// fields, because a frame aims at the hole it fills and not every hole is
// 16:10, and because compaction is a row of the portfolio's table (11.4).
// Into `packed`, with `row` the caller's scratch for the other packer.
void pack_best(Packing &packed,
               Packing &row,
               std::vector<scav_rect> const &rects,
               int32_t sep,
               scav_profile const &p,
               FrameDar dar,
               Compaction compaction) {
  pack_lr(packed, rects, sep, dar.num, dar.den, compaction);
  if (p.trybox != 0) {
    pack_box(row, rects, sep);
    if (fits(row) && (!fits(packed) ||
                      pack_better(row, packed, dar.num, dar.den, p.sm_tiebreak != 0))) {
      vec_assign(packed.at, row.at.begin(), row.at.end());
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

// `pad` rings a box's *contents*, and a bare pseudostate has none; the route
// attaches to the box, so padding leaves the arrow short of the glyph.
// Pseudostates only: an ordinary box is a container even when empty. The box
// formula and the descent that insets the bands both ask here, so a band's
// width is the box's own width less the ring the formula actually reserved,
// and never negative.
bool bare_pseudostate(Chart const &c,
                      std::vector<scav_rect> const &sub,
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

// One component laid out at one wrap width, packed or stacked.
struct Shape {
  std::vector<scav_point> at;
  Wide w{ 0 }, h{ 0 };
  bool ok{ true };
  // Trace records of the kept shape: piece starts, refused cuts and pseudostate seats.
  std::vector<TraceFold> cuts;
  std::vector<std::pair<uint32_t, SeatHow>> seated;
  std::vector<std::pair<uint32_t, int32_t>> centred;
  std::vector<TracePiece> packed;
  std::vector<TraceGap> lanes;
  std::vector<TraceCarry> carried;
  // Some piece is packed other than beside the one before it.
  bool wraps{ false };
  // Every edge a cut crosses between stacked pieces joins two plain nodes.
  bool drawable{ true };
  std::vector<uint8_t> lean;  // `SizedLayout::lean`, per frame edge

  // A default shape of `n` nodes and `edges` edges, keeping each buffer's storage.
  void reset(size_t n, size_t edges) {
    vec_assign(at, n, scav_point{});
    vec_assign(lean, edges, 0);
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
  std::vector<uint32_t> const &nodes, &index, &local_rank, &global_rank;
  std::vector<uint32_t> const &chunk_index, &chunk_nodes;
  std::vector<int32_t> const &line, &inset, &centre, &seat_at;
  CoordGraph const &cg;
};

// A frame the descent has yet to visit, and its root-absolute origin.
struct Frame {
  uint32_t sub;
  int32_t x, y;
};

// Every buffer a sizing pass uses, reassigned in place. Per-thread; sizing never waits on
// the pool and lays out one frame at a time.
struct SizeScratch {
  // The pass, and the states it sizes.
  std::vector<scav_point> sub_local;
  std::vector<int32_t> seg_label_h, seg_label_w;
  std::vector<uint32_t> port_seg;
  std::vector<std::vector<uint32_t>> states_at, subs_at;
  std::vector<scav_rect> kids;
  std::vector<uint32_t> ids;
  std::vector<Frame> work;
  std::vector<uint8_t> reuse, kept;  // per submachine copied from a base; per state
  std::vector<scav_extent> loop_label, loop_room;  // `loop_rooms`
  // What an `OwnerHole` sizing's first pass sized.
  SizedLayout first;
  std::vector<FrameDar> hole;  // `size_owner_holes` of `first`
  // One frame.
  std::vector<int32_t> reserve;
  std::vector<uint32_t> adj_count, adj_off, adj, fill, component, queue;
  std::vector<uint32_t> member_off, member, out_deg;
  std::vector<scav_point> local;
  std::vector<scav_rect> boxes;
  // One component, and one layout of it.
  std::vector<uint32_t> nodes, global_rank, local_rank, index, in_layer;
  std::vector<uint32_t> group, labelled, grouped, chunk_of, chunks;
  std::vector<int32_t> extent, layer_w, widest_label, group_w, line, inset;
  std::vector<Wide> layer_h, carry, applied, layer_lead;
  std::vector<uint8_t> glued, ridden, paired, bare, rides;
  std::vector<uint32_t> piece_of, ride_edge;
  std::vector<scav_rect> pieces;
  Packing packed, row, placed;  // a frame's pieces, `pack_best`'s other, a frame's boxes
  Shape best, folded, stacked;
  // One chunk.
  CoordGraph cg;
  std::vector<std::vector<uint32_t>> spare_layers;  // the layers a smaller chunk dropped
  std::vector<uint32_t> chunk_index, chunk_nodes, arrivals, nearest;
  std::vector<int32_t> seat_at, enter_at, centre;
  std::vector<uint8_t> apart, open, left;
  std::vector<uint32_t> mate, turning, turned_by, seat_to, fan;
  std::vector<Wide> layer_x, kept_w;
};

SizeScratch &size_scratch() {
  thread_local SizeScratch s;
  return s;
}

// At least `n` empty buckets, each keeping its storage.
void clear_buckets(std::vector<std::vector<uint32_t>> &buckets, size_t n) {
  if (buckets.size() < n) { vec_resize(buckets, n); }
  for (std::vector<uint32_t> &b : buckets) { b.clear(); }
}

// Exactly `n` empty layers, moving storage to and from `spare`.
void clear_layers(std::vector<std::vector<uint32_t>> &layers,
                  std::vector<std::vector<uint32_t>> &spare,
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
  for (std::vector<uint32_t> &l : layers) { l.clear(); }
}

// One sizing pass: what its frames and states share, and what it has sized so far.
struct Sizer {
  Chart const &c;
  SplitGraph const &g;
  SubmachineOrders const &o;
  scav_spaces const &s;
  scav_profile const &p;
  std::vector<FrameDar> const &hole;
  Compaction compaction;
  Fold fold;
  SizedLayout &out;
  std::vector<Diagnostic> &diags;
  FrameDar profile_dar{ .num = p.dar_num, .den = p.dar_den };
  uint32_t profile_word{ memo_profile(p) };
  SizeScratch &sc{ size_scratch() };
  // Where each submachine sits inside its owner's packed area, which the
  // descent at the end turns into an absolute origin.
  std::vector<scav_point> &sub_local{ sc.sub_local };
  // The label's extent across its frame's ranks and along them, on the middle
  // segment 11.3 charges it to, so one label reserves in one frame.
  std::vector<int32_t> &seg_label_h{ sc.seg_label_h };
  std::vector<int32_t> &seg_label_w{ sc.seg_label_w };
  // Per port, the segment on the border's inner side, whose boundary node is
  // where the router seats that port's slot.
  std::vector<uint32_t> &port_seg{ sc.port_seg };
  std::vector<scav_extent> &loop_label{ sc.loop_label };
  std::vector<scav_extent> &loop_room{ sc.loop_room };
  bool ok{ true };
  SizePassRecord const *base{ nullptr };
  SizePassRecord *record{ nullptr };

  // Every packing inside one state's interior fills the same hole, so the state
  // carries the ratio and its frames read their owner's.
  [[nodiscard]] FrameDar dar_of(uint32_t state) const {
    return ((state < hole.size()) && (hole[state].num != 0)) ? hole[state] : profile_dar;
  }
  [[nodiscard]] FrameDar owner_dar(uint32_t m) const {
    StateId const owner{ c.submachines[m].owner };
    return (owner.v == INVALID) ? profile_dar : dar_of(owner.v);
  }
  // Whether a frame's ranks run down the page. Its layout is computed along
  // the ranks and across them, and only placed as x and y at the end: across
  // is y and along is x for a frame that runs across, and the reverse for one
  // that runs down (11.10g).
  [[nodiscard]] bool runs_down(uint32_t m) const {
    return (m < o.sub_down.size()) && (o.sub_down[m] != 0);
  }
  // The row's rule, or the frame's fold pin where it has one.
  static_assert((static_cast<uint32_t>(Fold::Scale) == FOLD_SCALE) &&
                (static_cast<uint32_t>(Fold::Always) == FOLD_ALWAYS) &&
                (static_cast<uint32_t>(Fold::Never) == FOLD_NEVER));
  [[nodiscard]] Fold fold_of(uint32_t m) const {
    return ((m < o.sub_fold.size()) && (o.sub_fold[m] != 0))
               ? static_cast<Fold>(o.sub_fold[m] - 1U)
               : fold;
  }
  // The frame rank a fold pin's single cut falls before, or 0 for the width target's cuts.
  [[nodiscard]] uint32_t cut_of(uint32_t m) const {
    return (m < o.sub_fold_cut.size()) ? o.sub_fold_cut[m] : 0U;
  }
  // `seg_cross` at a segment, and whether a node is a boundary node on a cross
  // border. Hand-built orders carry no column.
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
  void step_layers(ChunkView const &v, std::vector<TraceGap> &lanes);
  void glue_layers(Span span,
                   Span espan,
                   std::vector<uint32_t> const &local_rank,
                   uint32_t layers);
  void assign_pieces(Span span,
                     Span espan,
                     std::vector<uint32_t> const &nodes,
                     std::vector<uint32_t> const &index,
                     std::vector<uint32_t> const &local_rank,
                     bool carry);
  void seat_riders(ChunkView const &v, Shape &shape, uint32_t chunk, Wide chunk_w);
  void seat_pseudostates(ChunkView const &v, Shape &shape, Wide chunk_w, Wide chunk_h);
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
                 std::vector<scav_rect> const &boxes,
                 std::vector<uint32_t> const &component,
                 std::vector<scav_point> const &local);
  void level_rank_ports(uint32_t m, bool down);
  void size_sub(uint32_t m);
  void size_state(uint32_t i);
  [[nodiscard]] bool same_frame(uint32_t m) const;
  uint32_t mark_reuse(std::vector<std::vector<uint32_t>> const &subs_at, uint32_t levels);
  void copy_frame(uint32_t m);
  void copy_state(uint32_t i);
  void note_frame(uint32_t m);
  void note_pass();
};

// Where `seg` meets `state`, across the ranks from its centre: its port's boundary node,
// or zero for the box itself or a frame inside running the other way.
int32_t Sizer::attach_at(uint32_t seg, uint32_t state, bool down) const {
  int32_t at{ 0 };
  static_cast<void>(port_at(seg, state, down, at));
  return at;
}

// `attach_at`, and whether `seg` meets `state` at a port on the faces this
// frame's edges arrive at, rather than at its box.
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
    // On the top or bottom border where the frame inside runs down and the port
    // sits where its rank puts it, or runs across and it sits on a cross border.
    bool const top_or_bottom{ runs_down(frame) != (cross_of(inner) != 0) };
    if (top_or_bottom != down) { continue; }
    scav_box_space const b{ box_of(s.box_state, s.n_box_state, state) };
    Wide const pad{ bare(b, state) ? 0 : p.pad };
    if (down) {
      Wide const x{ pad + b.w_before + sub_local[frame].x + out.node[node].x };
      at = static_cast<int32_t>(x - (out.state[state].w / 2));
      return true;
    }
    Wide const y{ pad + b.h_before + sub_local[frame].y + out.node[node].y };
    at = static_cast<int32_t>(y - (out.state[state].h / 2));
    return true;
  }
  return false;
}

// Each edge end that meets a composite at a port off its centre.
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
  std::vector<uint32_t> &adj_count{ sc.adj_count };
  vec_assign(adj_count, span.len, 0);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    ++adj_count[e.src - span.off];
    ++adj_count[e.dst - span.off];
  }
  std::vector<uint32_t> &adj_off{ sc.adj_off };
  vec_assign(adj_off, size_t{ span.len } + 1, 0);
  for (uint32_t k = 0; k < span.len; ++k) { adj_off[k + 1] = adj_off[k] + adj_count[k]; }
  std::vector<uint32_t> &adj{ sc.adj };
  vec_assign(adj, adj_off[span.len], 0);
  std::vector<uint32_t> &fill{ sc.fill };
  vec_assign(fill, adj_off.begin(), adj_off.end() - 1);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    adj[fill[e.src - span.off]++] = e.dst - span.off;
    adj[fill[e.dst - span.off]++] = e.src - span.off;
  }

  std::vector<uint32_t> &component{ sc.component };
  vec_assign(component, span.len, INVALID);
  std::vector<uint32_t> &queue{ sc.queue };
  uint32_t components{ 0 };
  for (uint32_t seed = 0; seed < span.len; ++seed) {
    if (component[seed] != INVALID) { continue; }
    uint32_t const id{ components++ };
    component[seed] = id;
    vec_assign(queue, 1, seed);
    for (uint32_t at = 0; at < queue.size(); ++at) {
      uint32_t const node{ queue[at] };
      for (uint32_t k = adj_off[node]; k < adj_off[node + 1]; ++k) {
        if (component[adj[k]] == INVALID) {
          component[adj[k]] = id;
          vec_push_back(queue, adj[k]);
        }
      }
    }
  }
  std::vector<uint32_t> &member_off{ sc.member_off };
  vec_assign(member_off, size_t{ components } + 1, 0);
  for (uint32_t k = 0; k < span.len; ++k) { ++member_off[component[k] + 1]; }
  for (uint32_t id = 0; id < components; ++id) { member_off[id + 1] += member_off[id]; }
  std::vector<uint32_t> &member{ sc.member };
  vec_assign(member, span.len, 0);
  vec_assign(fill, member_off.begin(), member_off.end() - 1);
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
  std::vector<uint32_t> &mate{ sc.mate };
  vec_assign(mate, n, INVALID);
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
  // next layer or a final's in the one before, where the moved box stays inside the chunk.
  Wide chunk_h{ 0 };
  for (uint32_t i = 0; i < n; ++i) {
    chunk_h = imax(chunk_h, (Wide{ v.centre[i] } - (v.cg.extent[i] / 2)) + v.cg.extent[i]);
  }
  std::vector<uint32_t> &seat_to{ sc.seat_to };
  vec_assign(seat_to, n, INVALID);
  for (uint32_t i = 0; i < n; ++i) {
    OrderNode const &nd{ node_of(v.chunk_nodes[i]) };
    uint32_t const one{ mate[i] };
    if ((nd.kind != OrderKind::State) || (one >= n)) { continue; }
    StateKind const kind{ c.states[nd.subject].kind };
    uint32_t const r{ v.local_rank[v.nodes[v.chunk_nodes[i]]] };
    uint32_t const nr{ v.local_rank[v.nodes[v.chunk_nodes[one]]] };
    bool const toward{ ((kind == StateKind::Initial) && (nr == (r + 1))) ||
                       ((kind == StateKind::Final) && ((nr + 1) == r)) };
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
  std::vector<uint32_t> &fan{ sc.fan };
  vec_assign(fan, 2 * n, 0);
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

  std::vector<uint32_t> &turning{ sc.turning };
  vec_assign(turning, v.last - v.first, 0);
  std::vector<uint32_t> &turned_by{ sc.turned_by };
  vec_assign(turned_by, v.last - v.first, INVALID);
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
void Sizer::step_layers(ChunkView const &v, std::vector<TraceGap> &lanes) {
  auto const along = [&](uint32_t st) {
    return v.down ? out.state[st].h : out.state[st].w;
  };
  auto const node_of = [&](uint32_t i) -> OrderNode const & {
    return o.nodes[v.span.off + v.nodes[i]];
  };
  std::vector<int32_t> const &label_row{ (o.labels.size() == o.gaps.size()) ? o.labels
                                                                            : o.gaps };
  auto const label_gap = [&](uint32_t r) {
    uint32_t const b{ v.global_rank[r] };
    return (b < v.gspan.len) ? Wide{ label_row[v.gspan.off + b] } : Wide{ 0 };
  };
  std::vector<uint32_t> const &turning{ sc.turning };
  int32_t const pitch{ label_line_height(p) };
  auto const lanes_at = [&](uint32_t r) {
    uint32_t const n{ turning[r - v.first] };
    return (n < 2) ? Wide{ 0 } : imin(Wide{ n } * pitch, Wide{ SPACE_MAX });
  };
  for (uint32_t r = v.first; (r + 1) < v.last; ++r) {
    if (turning[r - v.first] < 2) { continue; }
    vec_push_back(lanes,
                  { .boundary = v.global_rank[r],
                    .seg = sc.turned_by[r - v.first],
                    .width = static_cast<int32_t>(lanes_at(r)) });
  }

  auto const inset_of = [&](uint32_t i) {
    return (v.line[i] == 0) ? Wide{ 0 } : Wide{ v.inset[i] };
  };
  std::vector<uint32_t> const &seat_to{ sc.seat_to };
  std::vector<uint8_t> &left{ sc.left };
  vec_assign(left, v.chunk_nodes.size(), 0);
  std::vector<Wide> &kept_w{ sc.kept_w };
  vec_assign(kept_w, v.last - v.first, 0);
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
  std::vector<uint8_t> &open{ sc.open };
  vec_assign(open, v.last - v.first, 1);
  for (uint32_t r = v.first; r < v.last; ++r) {
    for (uint32_t const i : v.cg.layers[r - v.first]) {
      if ((node_of(v.chunk_nodes[i]).kind != OrderKind::Boundary) && (left[i] == 0)) {
        open[r - v.first] = 0;
      }
    }
  }
  auto const sep_in = [&](uint32_t r) {
    bool const clear{ (open[r - v.first] != 0) || (open[r + 1 - v.first] != 0) };
    return (clear && (label_gap(r) == 0)) ? Wide{ route_clearance(p) }
                                          : Wide{ p.rank_sep };
  };
  // Where a node of the chunk sits across its layer, and how wide it is.
  auto const x_in_layer = [&](uint32_t q) {
    OrderNode const &nd{ node_of(v.chunk_nodes[q]) };
    bool const dot{ (nd.kind == OrderKind::State) &&
                    (c.states[nd.subject].kind == StateKind::Initial) };
    uint32_t const r{ v.local_rank[v.nodes[v.chunk_nodes[q]]] };
    return dot ? (kept_w[r - v.first] - along(nd.subject)) : inset_of(v.chunk_nodes[q]);
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
  std::vector<Wide> &layer_x{ sc.layer_x };
  vec_assign(layer_x, v.last - v.first, 0);
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

// A pseudostate with one neighbour here sits level with it and `rank_sep` away, where its
// box is clear of every other node and inside the piece.
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
    // `rank_sep` from the state it joins, not a whole rank gap: the gap
    // also holds room other transitions' labels were charged.
    Wide x{ shape.at[at].x };
    if ((kind == StateKind::Final) && (r == (near_rank + 1))) { x = nx + nw + p.rank_sep; }
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
      // Boundary nodes sit on the frame's edge, which the bounds above clear.
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
      vec_emplace_back(shape.seated, nd.subject, SeatHow::Moved);
    } else if (clear(shape.at[at].x, y)) {
      shape.at[at].y = static_cast<int32_t>(y);
      vec_emplace_back(shape.seated, nd.subject, SeatHow::Levelled);
    } else {
      vec_emplace_back(shape.seated, nd.subject, SeatHow::Declined);
    }
  }
}

// A label beside a leg between ranks needs the leader on one side; where neither side
// holds it, the piece grows on its trailing side.
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

// A label's leader and width beside a leg, with half a node gap; zero for an unlabelled
// edge.
Wide Sizer::label_room(OrderEdge const &e) const {
  bool const has{ (e.segment < seg_label_w.size()) && (seg_label_w[e.segment] != 0) };
  return has ? (Wide{ label_leader(p) } + seg_label_w[e.segment] + (p.node_sep / 2))
             : Wide{ 0 };
}

// A labelled edge into a later piece passes the states below its end on their leading
// side, so that layer moves along; a label on a leg inside one column grows the trailing
// side.
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
  std::vector<Wide> &layer_lead{ sc.layer_lead };
  vec_assign(layer_lead, v.last - v.first, 0);
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
    Wide const grow{ (Wide{ route_clearance(p) } + need) - (lead - trail) };
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
                        std::vector<uint32_t> const &local_rank,
                        uint32_t layers) {
  std::vector<uint8_t> &glued{ sc.glued };
  vec_assign(glued, layers, 0);
  std::vector<uint8_t> &rides{ sc.rides };
  vec_assign(rides, span.len, 0);
  vec_assign(sc.ridden, layers, 0);
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
                          std::vector<uint32_t> const &nodes,
                          std::vector<uint32_t> const &index,
                          std::vector<uint32_t> const &local_rank,
                          bool carry) {
  std::vector<uint32_t> const &chunk_of{ sc.chunk_of };
  std::vector<uint32_t> &piece_of{ sc.piece_of };
  std::vector<uint32_t> &ride_edge{ sc.ride_edge };
  vec_resize(piece_of, nodes.size());
  vec_assign(ride_edge, nodes.size(), INVALID);
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
    vec_push_back(shape.carried,
                  { .seg = o.nodes[v.span.off + v.nodes[i]].subject,
                    .rank = v.global_rank[v.first] });
  }
}

// A frame's graph need not be connected, and unconnected states all rank 0, so
// one graph would stack them in a column. Components are laid out and packed.
void Sizer::lay_out_sub(uint32_t m) {
  Span const span{ o.sub_nodes[m] };
  uint32_t const ranks{ o.sub_ranks[m] };
  if ((span.len == 0) || (ranks == 0)) { return; }
  // Everything below is along the ranks and across them, placed as x and y
  // at the end, so a frame running down aims at its hole's ratio inverted.
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

  // Each end's extent carries the label's `leader + box height`, half on each side of the
  // leg.
  int32_t const leader{ label_leader(p) };
  std::vector<int32_t> &reserve{ sc.reserve };
  vec_assign(reserve, span.len, 0);
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
  std::vector<uint32_t> const &member{ sc.member };
  std::vector<uint32_t> const &member_off{ sc.member_off };
  std::vector<scav_point> &local{ sc.local };
  vec_assign(local, span.len, scav_point{});
  std::vector<scav_rect> &boxes{ sc.boxes };
  vec_assign(boxes, components, scav_rect{});
  for (uint32_t id = 0; id < components; ++id) {
    // A port on a cross border is placed on the frame's edge by `place_sub`
    // and takes no room in its rank.
    sc.nodes.clear();
    for (uint32_t k = member_off[id]; k < member_off[id + 1]; ++k) {
      if (!on_cross_border(span.off + member[k])) { vec_push_back(sc.nodes, member[k]); }
    }
    std::vector<uint32_t> const &nodes{ sc.nodes };

    // Ranks renumbered from zero; `global_rank` keeps the frame's rank for the gap
    // charges.
    std::vector<uint32_t> &global_rank{ sc.global_rank };
    global_rank.clear();
    std::vector<uint32_t> &local_rank{ sc.local_rank };
    vec_assign(local_rank, span.len, INVALID);
    for (uint32_t const k : nodes) {
      uint32_t const rank{ o.nodes[span.off + k].rank };
      if (global_rank.empty() || (global_rank.back() != rank)) {
        vec_push_back(global_rank, rank);
      }
      local_rank[k] = static_cast<uint32_t>(global_rank.size()) - 1;
    }
    uint32_t const layers{ static_cast<uint32_t>(global_rank.size()) };
    std::vector<uint32_t> &index{ sc.index };
    vec_assign(index, span.len, INVALID);
    std::vector<int32_t> &extent{ sc.extent };
    vec_assign(extent, nodes.size(), 0);
    std::vector<int32_t> &layer_w{ sc.layer_w };
    vec_assign(layer_w, layers, 0);
    std::vector<Wide> &layer_h{ sc.layer_h };
    vec_assign(layer_h, layers, 0);
    // The reserve keeps a label clear of the node beside its end; running down, a node
    // alone in its layer takes none.
    std::vector<uint32_t> &in_layer{ sc.in_layer };
    vec_assign(in_layer, layers, 0);
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
    std::vector<uint32_t> &group{ sc.group };
    vec_assign(group, nodes.size(), 0);
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
    // A group with two labelled edges has a label either side of its line,
    // so each side needs the widest one's room; one label takes whichever
    // side has it.
    std::vector<uint32_t> &labelled{ sc.labelled };
    vec_assign(labelled, nodes.size(), 0);
    std::vector<int32_t> &widest_label{ sc.widest_label };
    vec_assign(widest_label, nodes.size(), 0);
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
    std::vector<int32_t> &group_w{ sc.group_w };
    vec_assign(group_w, nodes.size(), 0);
    std::vector<uint32_t> &grouped{ sc.grouped };
    vec_assign(grouped, nodes.size(), 0);
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
    // Parallel to `nodes`: the width whose centre a grouped state sits on,
    // zero for one in no group.
    std::vector<int32_t> &line{ sc.line };
    vec_assign(line, nodes.size(), 0);
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      uint32_t const root_of{ columnar(i) ? find(i) : INVALID };
      if ((root_of == INVALID) || (grouped[root_of] < 2)) { continue; }
      line[i] = group_w[root_of];
      uint32_t const r{ local_rank[nodes[i]] };
      layer_w[r] = imax(layer_w[r], group_w[root_of]);
    }
    // Per node, where a grouped state starts along its column; a state meeting a
    // composite's port centres on that port, clamped to the line.
    std::vector<int32_t> &inset{ sc.inset };
    vec_assign(inset, nodes.size(), 0);
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
    std::vector<int32_t> const &label_row{ (o.labels.size() == o.gaps.size()) ? o.labels
                                                                              : o.gaps };
    auto const label_gap = [&](uint32_t r) {
      return (global_rank[r] < gspan.len) ? Wide{ label_row[gspan.off + global_rank[r]] }
                                          : Wide{ 0 };
    };
    // A layer of boundary nodes alone is open: it has no width, and the steps beside it
    // keep `route_clearance` rather than `rank_sep` unless a label sits between.
    std::vector<uint8_t> &bare{ sc.bare };
    vec_assign(bare, layers, 1);
    for (uint32_t const k : nodes) {
      if (o.nodes[span.off + k].kind != OrderKind::Boundary) { bare[local_rank[k]] = 0; }
    }
    auto const sep_after = [&](uint32_t r) {
      bool const open{ (bare[r] != 0) || (bare[r + 1] != 0) };
      return (open && (label_gap(r) == 0)) ? Wide{ route_clearance(p) }
                                           : Wide{ p.rank_sep };
    };

    glue_layers(span, espan, local_rank, layers);
    std::vector<uint8_t> const &glued{ sc.glued };
    // A leg's position down the overlap of its two ends, or -1 with no overlap, and the
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
    // Both the straight run and its fold are laid out; the scale measure picks. A nonzero
    // `cut_at` is the one frame rank a cut falls before, in place of `wrap_at`'s.
    auto const lay_out = [&](Shape &shape, Wide wrap_at, uint32_t cut_at, bool stack) {
      shape.reset(nodes.size(), espan.len);
      std::vector<uint32_t> &chunk_of{ sc.chunk_of };
      vec_assign(chunk_of, layers, 0);
      std::vector<uint32_t> &chunks{ sc.chunks };
      vec_assign(chunks, 1, 0);
      Wide run{ 0 };
      for (uint32_t r = 0; r < layers; ++r) {
        Wide const step{ (r == 0) ? Wide{ layer_w[r] }
                                  : (Wide{ layer_w[r] } + sep_after(r - 1) +
                                     boundary_gap(r - 1)) };
        bool const wants_cut{ (r > 0) && ((cut_at != 0) ? (global_rank[r] == cut_at)
                                                        : ((run + step) > wrap_at)) };
        bool const refused{ (glued[r] != 0) || ((cut_at == 0) && (sc.ridden[r] != 0)) };
        if (wants_cut && refused) {
          vec_push_back(shape.cuts,
                        { .rank = global_rank[r], .refused = 1, .carried = 0 });
        }
        if (wants_cut && !refused) {
          vec_push_back(shape.cuts,
                        { .rank = global_rank[r], .refused = 0, .carried = 0 });
          vec_push_back(chunks, r);
          run = layer_w[r];
        } else {
          run += step;
        }
        chunk_of[r] = static_cast<uint32_t>(chunks.size()) - 1;
      }
      assign_pieces(span, espan, nodes, index, local_rank, cut_at != 0);
      std::vector<uint32_t> const &piece_of{ sc.piece_of };

      std::vector<scav_rect> &pieces{ sc.pieces };
      vec_assign(pieces, chunks.size(), scav_rect{});
      std::vector<Wide> &carry{ sc.carry };
      vec_assign(carry, chunks.size(), 0);
      bool pieces_fit{ true };
      for (uint32_t chunk = 0; chunk < chunks.size(); ++chunk) {
        uint32_t const first{ chunks[chunk] };
        uint32_t const last{ ((chunk + 1) < chunks.size()) ? chunks[chunk + 1] : layers };

        // Only this chunk's nodes and the edges wholly inside it: an edge the cut
        // crosses has its ends in two pieces, as a wrapped line's does.
        CoordGraph &cg{ sc.cg };
        clear_layers(cg.layers, sc.spare_layers, last - first);
        cg.extent.clear();
        cg.edges.clear();
        std::vector<uint32_t> &chunk_index{ sc.chunk_index };
        vec_assign(chunk_index, nodes.size(), INVALID);
        std::vector<uint32_t> &chunk_nodes{ sc.chunk_nodes };
        chunk_nodes.clear();
        for (uint32_t i = 0; i < nodes.size(); ++i) {
          uint32_t const r{ local_rank[nodes[i]] };
          if ((chunk_of[r] != chunk) || (piece_of[i] != chunk)) { continue; }
          chunk_index[i] = static_cast<uint32_t>(chunk_nodes.size());
          vec_push_back(chunk_nodes, i);
          vec_push_back(cg.extent, extent[i]);
          vec_push_back(cg.layers[r - first], chunk_index[i]);
        }
        // An initial and one other edge into the same face each meet it `half` off centre;
        // the initial's edge is weak.
        std::vector<int32_t> &seat_at{ sc.seat_at };
        vec_assign(seat_at, chunk_nodes.size(), 0);
        std::vector<int32_t> &enter_at{ sc.enter_at };
        vec_assign(enter_at, chunk_nodes.size(), 0);
        std::vector<uint8_t> &apart{ sc.apart };
        vec_assign(apart, espan.len, 0);
        auto const in_chunk = [&](uint32_t node) {
          uint32_t const i{ index[node - span.off] };
          return ((i != INVALID) && (chunk_index[i] != INVALID)) ? i : INVALID;
        };
        auto const initial = [&](uint32_t node) {
          OrderNode const &nd{ o.nodes[node] };
          return (nd.kind == OrderKind::State) &&
                 (c.states[nd.subject].kind == StateKind::Initial);
        };
        // Per node, what enters its leading face other than an initial, and
        // the position of the nearest of those in the layer before.
        std::vector<uint32_t> &arrivals{ sc.arrivals };
        vec_assign(arrivals, chunk_nodes.size(), 0);
        std::vector<uint32_t> &nearest{ sc.nearest };
        vec_assign(nearest, chunk_nodes.size(), INVALID);
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
          int32_t const half{ ceil_div((across(nd.subject) / 2) + route_clearance(p), 2) };
          // One other arrival: with more, the initial is the median the
          // alignment keeps the target steady on.
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
          // Brandes-Kopf reads a segment between consecutive layers, earlier
          // end first. A rank pin can leave an edge within one layer or
          // pointing back, and a cut leaves one spanning several; aligned, a
          // flat edge's two ends became one block at one coordinate, which
          // drew `ota`'s `Writing` over `Fetching` (11.10g).
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
          vec_push_back(cg.edges,
                        { .from = chunk_index[from],
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
        std::vector<int32_t> &centre{ sc.centre };
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

        // The gap a label was charged to the boundary this cut falls on, less
        // what the packing between two pieces already gives it. Inside a
        // chunk the charge is `rank_sep + boundary_gap`; across a cut the
        // packer separates by `node_sep` alone and the charge was being
        // dropped, which is why `estop` draws a 384-wide label across a
        // 288-unit gap (11.9.3). Held back until the packing says where the
        // piece went: see below.
        carry[chunk] = (first == 0)
                           ? Wide{ 0 }
                           : imax(boundary_gap(first - 1) - p.node_sep, Wide{ 0 });
        step_layers(view, shape.lanes);
        std::vector<Wide> const &kept_w{ sc.kept_w };
        std::vector<Wide> const &layer_x{ sc.layer_x };
        Wide chunk_w{ layer_x[last - first - 1] + kept_w[last - first - 1] };
        Wide chunk_h{ 0 };
        for (uint32_t i = 0; i < chunk_nodes.size(); ++i) {
          // A node's trailing edge, not its centre plus half its extent: an odd extent
          // halves down, so the box would not contain its own rects.
          chunk_h = imax(chunk_h, (Wide{ centre[i] } - (cg.extent[i] / 2)) + cg.extent[i]);
        }
        for (uint32_t i = 0; i < chunk_nodes.size(); ++i) {
          uint32_t const at{ chunk_nodes[i] };
          uint32_t const r{ local_rank[nodes[at]] };
          // An initial sits flush against its layer's trailing edge, beside the state it
          // enters.
          OrderNode const &nd{ o.nodes[span.off + nodes[at]] };
          Wide const flush{ ((nd.kind == OrderKind::State) &&
                             (c.states[nd.subject].kind == StateKind::Initial))
                                ? imax(kept_w[r - first] - along(nd.subject), Wide{ 0 })
                                : Wide{ 0 } };
          // On its group's centre line, where it has one.
          Wide const from_edge{ (line[at] == 0) ? flush : Wide{ inset[at] } };
          if (from_edge != flush) {
            vec_emplace_back(shape.centred, nd.subject, static_cast<int32_t>(from_edge));
          }
          // Local to the piece; the packing below decides where the piece
          // itself goes.
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

      // Pieces are packed by `pack_lr`; `stack` is the other candidate, each piece under
      // the one before at the leading edge.
      Packing &packed{ sc.packed };
      if (stack) {
        packed.at.clear();
        Wide y{ 0 };
        Wide w{ 0 };
        for (scav_rect const &piece : pieces) {
          vec_push_back(packed.at,
                        { .x = 0,
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
      // A cut's label room goes on the new piece's leading edge only where the
      // packing puts that piece beside the one before it, so the leg between
      // them runs across the gap. Stacked, the leg runs down the gap the two
      // ends' own reserve already opened, and the room pushed the piece away
      // from the state it joins: `brew`'s `Pumping`, `dock`'s `Charging`.
      std::vector<Wide> &applied{ sc.applied };
      vec_assign(applied, chunks.size(), 0);
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
      // Room for a label on an edge a cut crosses where the packing put its
      // two pieces one above the other, so its leg runs down the gap between
      // them. Two labelled edges between one pair run as a pair of legs with
      // a label outside each, so they need the room on both sides, and the
      // leading side is made by moving every piece along: one side each left
      // `ota`'s pair printing over both its states (11.10g).
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
      std::vector<uint8_t> &paired{ sc.paired };
      vec_assign(paired, espan.len, 0);
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
      // corner arc and at least one unit, or half the face for an inscribed glyph.
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
      // A fold is drawable only where each edge across a cut joins two plain nodes;
      // otherwise the scale measure's fold is no candidate and `Always` defers to `Cost`.
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
        vec_push_back(shape.packed,
                      { .rank = global_rank[chunks[k]],
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
      // A saturated position would leave int32 when the offset below added
      // to it, and no caller reads a shape this phase goes on to diagnose.
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
    // A frame running down never folds.
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
    // The two folds are one cut in two arrangements, so the scale measure
    // picks between them before either is weighed against the flat run.
    auto const usable = [always](Shape const &f) {
      return f.ok && f.wraps && (f.drawable || always);
    };
    if ((rule != Fold::Never) && usable(stacked) &&
        (!usable(folded) || (stacked.drawable && !folded.drawable) ||
         ((stacked.drawable == folded.drawable) && better(stacked, folded)))) {
      std::swap(folded, stacked);
    }
    // `Always` takes the folded shape; a fold whose pieces repack into one row is the run
    // unfolded, so it is no candidate.
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
    for (auto const &[state, by] : best.centred) {
      trace_emit({ .kind = TraceKind::ColumnCentred,
                   .frame = m,
                   .shift = { .state = state, .seg = INVALID, .by = by } });
    }
    for (auto const &[state, how] : best.seated) {
      trace_emit({ .kind = TraceKind::PseudostateSeated,
                   .pass = static_cast<uint16_t>(how),
                   .frame = m,
                   .rank = { .state = state, .rank = 0 } });
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
    boxes[id] = { .x = 0,
                  .y = 0,
                  .w = static_cast<int32_t>(best.w),
                  .h = static_cast<int32_t>(best.h) };
    for (uint32_t i = 0; i < nodes.size(); ++i) { local[nodes[i]] = best.at[i]; }
    for (uint32_t k = 0; k < espan.len; ++k) {
      uint32_t const seg{ o.edges[espan.off + k].segment };
      if ((best.lean[k] != 0) && (seg < out.lean.size())) { out.lean[seg] = 1; }
    }
  }

  place_sub(m, down, dar, boxes, sc.component, local);
}

// The frame's packing of its pieces, and each node's and state's place in it
// along the ranks and across them, as x and y.
void Sizer::place_sub(uint32_t m,
                      bool down,
                      FrameDar dar,
                      std::vector<scav_rect> const &boxes,
                      std::vector<uint32_t> const &component,
                      std::vector<scav_point> const &local) {
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

  // A boundary node's rank puts it on the frame's border, and the folding and
  // the two packings above leave a piece's own edges mid-frame, so its place
  // along the ranks comes from the frame: sources where a route arrives,
  // sinks where one leaves.
  std::vector<uint32_t> &out_deg{ sc.out_deg };
  vec_assign(out_deg, span.len, 0);
  for (uint32_t k = 0; k < espan.len; ++k) {
    ++out_deg[o.edges[espan.off + k].src - span.off];
  }

  for (uint32_t k = 0; k < span.len; ++k) {
    scav_rect const &at{ packed.at[component[k]] };
    OrderNode const &nd{ o.nodes[span.off + k] };
    int32_t lead{ local[k].x + at.x };  // along the ranks
    if (nd.kind == OrderKind::Boundary) {
      // The frame's edge, not the piece's: sources where a route arrives,
      // sinks where one leaves.
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
               ((cr + cr_len + clear) <= band_lo) || ((cr - clear) >= band_hi);
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
  thread_local std::vector<uint32_t> key;
  thread_local std::vector<int32_t> value;
  key.clear();
  auto const put = [](int32_t v) { vec_push_back(key, static_cast<uint32_t>(v)); };
  vec_push_back(key, span.len);
  vec_push_back(key, o.sub_ranks[m]);
  vec_push_back(key, down ? 1U : 0U);
  put(dar.num);
  put(dar.den);
  vec_push_back(key, static_cast<uint32_t>(compaction));
  vec_push_back(key, static_cast<uint32_t>(fold_of(m)));
  vec_push_back(key, cut_of(m));
  vec_push_back(key, g.serial);
  vec_push_back(key, profile_word);
  for (uint32_t k = 0; k < span.len; ++k) {
    OrderNode const &nd{ o.nodes[span.off + k] };
    vec_push_back(key, static_cast<uint32_t>(nd.kind));
    vec_push_back(key, nd.subject);
    vec_push_back(key, nd.rank);
    vec_push_back(key, nd.pos);
    if (nd.kind == OrderKind::Boundary) { vec_push_back(key, cross_of(nd.subject)); }
    if (nd.kind != OrderKind::State) { continue; }
    put(out.state[nd.subject].w);
    put(out.state[nd.subject].h);
    if (g.serial != 0) { continue; }  // the chart a serial names fixes these two
    vec_push_back(key, static_cast<uint32_t>(c.states[nd.subject].kind));
    vec_push_back(key, c.states[nd.subject].submachines.len);
  }
  vec_push_back(key, espan.len);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    vec_push_back(key, e.src - span.off);
    vec_push_back(key, e.dst - span.off);
    vec_push_back(key, e.segment);
    vec_push_back(key, e.reversed);
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
      vec_push_back(key,
                    ((state && ported(e.segment, nd.subject)) ? 1U : 0U) |
                        (faced ? 2U : 0U) | (flat_faced ? 4U : 0U));
      put(at);
      put(across_at);
    }
  }
  vec_push_back(key, gspan.len);
  bool const labelled{ o.labels.size() == o.gaps.size() };
  for (uint32_t k = 0; k < gspan.len; ++k) {
    put(o.gaps[gspan.off + k]);
    put(labelled ? o.labels[gspan.off + k] : o.gaps[gspan.off + k]);
  }

  // What it writes: the frame's extent, whether it folded, each node's place, each
  // state's, and each edge's lean.
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
  vec_push_back(value, out.sub[m].w);
  vec_push_back(value, out.sub[m].h);
  vec_push_back(value, out.folded[m]);
  for (uint32_t k = 0; k < span.len; ++k) {
    OrderNode const &nd{ o.nodes[span.off + k] };
    vec_push_back(value, out.node[span.off + k].x);
    vec_push_back(value, out.node[span.off + k].y);
    if (nd.kind != OrderKind::State) { continue; }
    vec_push_back(value, out.state[nd.subject].x);
    vec_push_back(value, out.state[nd.subject].y);
  }
  for (uint32_t k = 0; k < espan.len; ++k) {
    vec_push_back(value, (seg_of(k) < out.lean.size()) ? out.lean[seg_of(k)] : 0);
  }
  memo.insert(key, value);
}

void Sizer::size_state(uint32_t i) {
  std::vector<scav_rect> &kids{ sc.kids };
  kids.clear();
  std::vector<uint32_t> &ids{ sc.ids };
  ids.clear();
  Span const subs{ c.states[i].submachines };
  for (uint32_t k = 0; k < subs.len; ++k) {
    uint32_t const m{ c.submachine_ids[subs.off + k].v };
    if (c.submachines[m].live == 0) { continue; }
    vec_push_back(kids, out.sub[m]);
    vec_push_back(ids, m);
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
      // Whitespace elimination grew the frame to fill its row, and this is
      // the one packing whose rect is a box a reader sees. A sink boundary
      // node sits on the frame's trailing edge, so the growth moves it; a
      // source sits at zero and does not.
      // One on the trailing cross border moves with it the other way.
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
  // A bar is thin across the axis the flow crosses it on, which for a frame
  // running down is the other one, so it lies down there (11.10g).
  StateKind const sk{ c.states[i].kind };
  bool const lies{ ((sk == StateKind::Fork) || (sk == StateKind::Join)) &&
                   runs_down(c.states[i].parent.v) };
  int32_t const min_w{ lies ? p.kind_min_h[kind] : p.kind_min_w[kind] };
  int32_t const min_h{ lies ? p.kind_min_w[kind] : p.kind_min_h[kind] };
  Wide const ring{ bare(b, i) ? Wide{ 0 } : (2 * static_cast<Wide>(p.pad)) };
  scav_extent const room{ room_of(i) };
  Wide const centre{ Wide{ b.w_before } + imax(Wide{ packed.w }, Wide{ room.w }) +
                     b.w_after };
  Wide const body{ Wide{ packed.h } + room.h +
                   (((packed.h > 0) && (room.h > 0)) ? p.sub_sep : 0) };
  Wide const w{ imax(imax(Wide{ b.min_w }, centre), Wide{ min_w }) + ring };
  Wide const h{ imax(Wide{ b.h_before } + body + b.h_after, Wide{ min_h }) + ring };
  if ((w > COORD_MAX) || (h > COORD_MAX)) {
    overflow(diags, ElemKind::State, i);
    ok = false;
    return;
  }
  out.state[i].w = static_cast<int32_t>(w);
  out.state[i].h = static_cast<int32_t>(h);
}

// Whether frame `m` reads from this pass's orders and labels what it read from the
// base's: its nodes and edges frame-locally, its gaps, ranks, orientation and fold pin.
bool Sizer::same_frame(uint32_t m) const {
  SubmachineOrders const &bo{ base->orders };
  Span const a{ o.sub_nodes[m] };
  Span const b{ bo.sub_nodes[m] };
  Span const ea{ o.sub_edges[m] };
  Span const eb{ bo.sub_edges[m] };
  Span const ga{ o.sub_gaps[m] };
  Span const gb{ bo.sub_gaps[m] };
  if ((a.len != b.len) || (ea.len != eb.len) || (ga.len != gb.len) ||
      (o.sub_ranks[m] != bo.sub_ranks[m]) || (o.sub_down[m] != bo.sub_down[m]) ||
      (o.sub_fold[m] != bo.sub_fold[m]) || (o.sub_fold_cut[m] != bo.sub_fold_cut[m])) {
    return false;
  }
  for (uint32_t k = 0; k < a.len; ++k) {
    OrderNode const &x{ o.nodes[a.off + k] };
    if (!(x == bo.nodes[b.off + k])) { return false; }
    if ((x.kind == OrderKind::Boundary) && (x.subject < o.seg_cross.size()) &&
        (o.seg_cross[x.subject] != bo.seg_cross[x.subject])) {
      return false;
    }
  }
  for (uint32_t k = 0; k < ea.len; ++k) {
    OrderEdge const &x{ o.edges[ea.off + k] };
    OrderEdge const &y{ bo.edges[eb.off + k] };
    if (((x.src - a.off) != (y.src - b.off)) || ((x.dst - a.off) != (y.dst - b.off)) ||
        (x.segment != y.segment) || (x.reversed != y.reversed)) {
      return false;
    }
    if ((x.segment < seg_label_h.size()) &&
        ((seg_label_h[x.segment] != base->seg_label_h[x.segment]) ||
         (seg_label_w[x.segment] != base->seg_label_w[x.segment]))) {
      return false;
    }
  }
  bool const la{ o.labels.size() == o.gaps.size() };
  bool const lb{ bo.labels.size() == bo.gaps.size() };
  for (uint32_t k = 0; k < ga.len; ++k) {
    if ((o.gaps[ga.off + k] != bo.gaps[gb.off + k]) ||
        ((la ? o.labels[ga.off + k] : o.gaps[ga.off + k]) !=
         (lb ? bo.labels[gb.off + k] : bo.gaps[gb.off + k]))) {
      return false;
    }
  }
  return true;
}

// Into `sc.reuse`, 1 for each live frame whose inputs and every descendant's match the
// base's, and into `sc.kept`, 1 for each state whose sizing's do; the frames marked.
uint32_t Sizer::mark_reuse(std::vector<std::vector<uint32_t>> const &subs_at,
                           uint32_t levels) {
  SubmachineOrders const &bo{ base->orders };
  std::vector<uint8_t> &dirty{ sc.reuse };  // inverted at the end
  std::vector<uint8_t> &kept{ sc.kept };
  vec_assign(dirty, c.submachines.size(), 0);
  vec_assign(kept, c.states.size(), 0);
  auto const mark = [&](uint32_t m) {
    if (m < dirty.size()) { dirty[m] = 1; }
  };
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if ((c.submachines[m].live != 0) && !same_frame(m)) { dirty[m] = 1; }
  }
  // `port_at` reads a port through its state's frame: the inner segment, its boundary
  // node's place in its own frame and its cross border.
  for (uint32_t port = 0; port < g.ports.size(); ++port) {
    uint32_t const state{ g.ports[port].state.v };
    if (state >= c.states.size()) { continue; }
    uint32_t const inner{ port_seg[port] };
    bool same{ inner == base->port_seg[port] };
    if (same && (inner < g.segments.size())) {
      uint32_t const frame{ g.segments[inner].frame.v };
      uint32_t const at{ o.seg_node[inner] };
      uint32_t const was{ bo.seg_node[inner] };
      if (frame < c.submachines.size()) {
        Span const a{ o.sub_nodes[frame] };
        Span const b{ bo.sub_nodes[frame] };
        bool const in_a{ (at >= a.off) && (at < (a.off + a.len)) };
        bool const in_b{ (was >= b.off) && (was < (b.off + b.len)) };
        same = (((at == INVALID) && (was == INVALID)) ||
                (in_a && in_b && ((at - a.off) == (was - b.off)))) &&
               (o.seg_cross[inner] == bo.seg_cross[inner]);
      } else {
        same = (at == was) && (o.seg_cross[inner] == bo.seg_cross[inner]);
      }
    }
    if (!same) { mark(c.states[state].parent.v); }
  }
  bool ignore_dar{ false };
#ifdef SCAV_TESTING
  ignore_dar = test_reuse_ignore_dar;
#endif
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (c.states[i].live == 0) { continue; }
    uint32_t const parent{ c.states[i].parent.v };
    FrameDar const d{ dar_of(i) };
    if (!ignore_dar && ((d.num != base->dar[i].num) || (d.den != base->dar[i].den))) {
      Span const subs{ c.states[i].submachines };
      for (uint32_t k = 0; k < subs.len; ++k) { mark(c.submachine_ids[subs.off + k].v); }
    }
    scav_box_space const b{ box_of(s.box_state, s.n_box_state, i) };
    scav_box_space const &bb{ base->box[i] };
    bool const boxed{ (b.min_w == bb.min_w) && (b.h_before == bb.h_before) &&
                      (b.h_after == bb.h_after) && (b.w_before == bb.w_before) &&
                      (b.w_after == bb.w_after) };
    if (!boxed) { mark(parent); }
    kept[i] = (boxed && ((parent >= c.submachines.size()) ||
                         (o.sub_down[parent] == bo.sub_down[parent])))
                  ? 1
                  : 0;
  }
  // Deepest first, a frame laid out again lays out its owner's frame again.
  for (uint32_t level = levels; level-- > 0;) {
    for (uint32_t const m : subs_at[level]) {
      uint32_t const owner{ c.submachines[m].owner.v };
      if ((dirty[m] != 0) && (owner < c.states.size())) { mark(c.states[owner].parent.v); }
    }
  }
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    Span const subs{ c.states[i].submachines };
    for (uint32_t k = 0; (kept[i] != 0) && (k < subs.len); ++k) {
      uint32_t const m{ c.submachine_ids[subs.off + k].v };
      if ((c.submachines[m].live != 0) && (dirty[m] != 0)) { kept[i] = 0; }
    }
  }
  uint32_t taken{ 0 };
  for (uint32_t m = 0; m < dirty.size(); ++m) {
    dirty[m] = ((c.submachines[m].live != 0) && (dirty[m] == 0)) ? 1 : 0;
    taken += dirty[m];
  }
  return taken;
}

// Frame `m` as the base laid it out: as its owner's packing left it where that state is
// copied too, else as it stood before.
void Sizer::copy_frame(uint32_t m) {
  SubmachineOrders const &bo{ base->orders };
  Span const a{ o.sub_nodes[m] };
  Span const b{ bo.sub_nodes[m] };
  Span const ea{ o.sub_edges[m] };
  Span const eb{ bo.sub_edges[m] };
  uint32_t const owner{ c.submachines[m].owner.v };
  bool const packed{ (owner < c.states.size()) && (sc.kept[owner] != 0) };
  std::vector<scav_point> const &from{ packed ? base->local.node : base->pre_node };
  for (uint32_t k = 0; k < a.len; ++k) {
    out.node[a.off + k] = from[b.off + k];
    OrderNode const &nd{ o.nodes[a.off + k] };
    if (nd.kind != OrderKind::State) { continue; }
    out.state[nd.subject].x = base->local.state[nd.subject].x;
    out.state[nd.subject].y = base->local.state[nd.subject].y;
  }
  scav_point const extent{ packed ? scav_point{ .x = base->local.sub[m].w,
                                                .y = base->local.sub[m].h }
                                  : base->pre_sub[m] };
  out.sub[m].w = extent.x;
  out.sub[m].h = extent.y;
  out.folded[m] = base->local.folded[m];
  for (uint32_t k = 0; k < ea.len; ++k) {
    uint32_t const seg{ o.edges[ea.off + k].segment };
    if ((base->edge_lean[eb.off + k] != 0) && (seg < out.lean.size())) {
      out.lean[seg] = 1;
    }
  }
  if (record == nullptr) { return; }
  for (uint32_t k = 0; k < a.len; ++k) {
    record->pre_node[a.off + k] = base->pre_node[b.off + k];
  }
  record->pre_sub[m] = base->pre_sub[m];
  for (uint32_t k = 0; k < ea.len; ++k) {
    record->edge_lean[ea.off + k] = base->edge_lean[eb.off + k];
  }
}

// State `i` as the base sized it, its frames already copied as its packing left them.
void Sizer::copy_state(uint32_t i) {
  out.state[i].w = base->local.state[i].w;
  out.state[i].h = base->local.state[i].h;
  Span const subs{ c.states[i].submachines };
  for (uint32_t k = 0; k < subs.len; ++k) {
    uint32_t const m{ c.submachine_ids[subs.off + k].v };
    if (c.submachines[m].live != 0) { sub_local[m] = base->sub_local[m]; }
  }
}

// Frame `m` into the record as it stands before its owner's packing.
void Sizer::note_frame(uint32_t m) {
  Span const span{ o.sub_nodes[m] };
  Span const espan{ o.sub_edges[m] };
  for (uint32_t k = 0; k < span.len; ++k) {
    record->pre_node[span.off + k] = out.node[span.off + k];
  }
  record->pre_sub[m] = { .x = out.sub[m].w, .y = out.sub[m].h };
  for (uint32_t k = 0; k < espan.len; ++k) {
    uint32_t const seg{ o.edges[espan.off + k].segment };
    record->edge_lean[espan.off + k] = (seg < out.lean.size()) ? out.lean[seg] : 0;
  }
}

// The pass's inputs and its frame-local results into the record, before the descent.
void Sizer::note_pass() {
  SizePassRecord &r{ *record };
  r.ok = ok;
  r.serial = g.serial;
  r.profile = p;
  r.compaction = compaction;
  r.fold = fold;
  SubmachineOrders &ro{ r.orders };
  vec_assign(ro.nodes, o.nodes.begin(), o.nodes.end());
  vec_assign(ro.edges, o.edges.begin(), o.edges.end());
  vec_assign(ro.sub_nodes, o.sub_nodes.begin(), o.sub_nodes.end());
  vec_assign(ro.sub_edges, o.sub_edges.begin(), o.sub_edges.end());
  vec_assign(ro.sub_ranks, o.sub_ranks.begin(), o.sub_ranks.end());
  vec_assign(ro.sub_down, o.sub_down.begin(), o.sub_down.end());
  vec_assign(ro.sub_fold, o.sub_fold.begin(), o.sub_fold.end());
  vec_assign(ro.sub_fold_cut, o.sub_fold_cut.begin(), o.sub_fold_cut.end());
  vec_assign(ro.sub_gaps, o.sub_gaps.begin(), o.sub_gaps.end());
  vec_assign(ro.gaps, o.gaps.begin(), o.gaps.end());
  vec_assign(ro.labels, o.labels.begin(), o.labels.end());
  vec_assign(ro.seg_node, o.seg_node.begin(), o.seg_node.end());
  vec_assign(ro.seg_cross, o.seg_cross.begin(), o.seg_cross.end());
  vec_assign(r.seg_label_h, seg_label_h.begin(), seg_label_h.end());
  vec_assign(r.seg_label_w, seg_label_w.begin(), seg_label_w.end());
  vec_assign(r.port_seg, port_seg.begin(), port_seg.end());
  vec_assign(r.dar, c.states.size(), FrameDar{});
  vec_assign(r.box, c.states.size(), scav_box_space{});
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    r.dar[i] = dar_of(i);
    r.box[i] = box_of(s.box_state, s.n_box_state, i);
  }
  vec_assign(r.sub_local, sub_local.begin(), sub_local.end());
  vec_assign(r.local.state, out.state.begin(), out.state.end());
  vec_assign(r.local.sub, out.sub.begin(), out.sub.end());
  vec_assign(r.local.node, out.node.begin(), out.node.end());
  vec_assign(r.local.lean, out.lean.begin(), out.lean.end());
  vec_assign(r.local.folded, out.folded.begin(), out.folded.end());
}

// Whether `base` recorded a pass over this chart, graph, profile and row, with every
// column a frame comparison reads.
bool comparable(Chart const &c,
                SplitGraph const &g,
                SubmachineOrders const &o,
                scav_profile const &p,
                Compaction compaction,
                Fold fold,
                SizePassRecord const *base) {
  if ((base == nullptr) || !base->ok || (trace_sink() != nullptr) || (g.serial == 0) ||
      (base->serial != g.serial) || (base->compaction != compaction) ||
      (base->fold != fold) ||
      (std::memcmp(&base->profile, &p, sizeof(scav_profile)) != 0)) {
    return false;
  }
#ifdef SCAV_TESTING
  if (!test_reuse) { return false; }
#endif
  size_t const subs{ c.submachines.size() };
  size_t const segs{ g.segments.size() };
  size_t const states{ c.states.size() };
  SubmachineOrders const &bo{ base->orders };
  for (SubmachineOrders const *x : { &o, &bo }) {
    if ((x->sub_nodes.size() != subs) || (x->sub_edges.size() != subs) ||
        (x->sub_gaps.size() != subs) || (x->sub_ranks.size() != subs) ||
        (x->sub_down.size() != subs) || (x->sub_fold.size() != subs) ||
        (x->sub_fold_cut.size() != subs) || (x->seg_node.size() != segs) ||
        (x->seg_cross.size() != segs)) {
      return false;
    }
  }
  return (base->seg_label_h.size() == segs) && (base->seg_label_w.size() == segs) &&
         (base->port_seg.size() == g.ports.size()) && (base->dar.size() == states) &&
         (base->box.size() == states) && (base->local.state.size() == states) &&
         (base->local.sub.size() == subs) && (base->local.folded.size() == subs) &&
         (base->local.lean.size() == segs) && (base->pre_sub.size() == subs) &&
         (base->sub_local.size() == subs) && (base->pre_node.size() == bo.nodes.size()) &&
         (base->local.node.size() == bo.nodes.size()) &&
         (base->edge_lean.size() == bo.edges.size());
}

// One whole sizing. `hole` is parallel to states -- the ratio every packing
// inside that state's interior aims at, a `num` of 0 or a short vector falling
// back to the profile's.
bool size_pass(Chart const &c,
               SplitGraph const &g,
               SubmachineOrders const &o,
               scav_spaces const &s,
               scav_profile const &p,
               std::vector<FrameDar> const &hole,
               Compaction compaction,
               Fold fold,
               SizedLayout &out,
               std::vector<Diagnostic> &diags,
               SizePassRecord const *base,
               SizePassRecord *record) {
  Sizer x{ .c = c,
           .g = g,
           .o = o,
           .s = s,
           .p = p,
           .hole = hole,
           .compaction = compaction,
           .fold = fold,
           .out = out,
           .diags = diags,
           .base = comparable(c, g, o, p, compaction, fold, base) ? base : nullptr,
           .record = record };
  vec_assign(out.state, c.states.size(), {});
  vec_assign(out.before, c.states.size(), {});
  vec_assign(out.after, c.states.size(), {});
  vec_assign(out.lead, c.states.size(), {});
  vec_assign(out.trail, c.states.size(), {});
  vec_assign(out.loop, c.states.size(), {});
  vec_assign(out.sub, c.submachines.size(), {});
  vec_assign(out.node, o.nodes.size(), {});
  vec_assign(out.lean, g.segments.size(), 0);
  vec_assign(out.folded, c.submachines.size(), 0);
  out.chart = {};
  std::vector<scav_point> &sub_local{ x.sub_local };
  vec_assign(sub_local, c.submachines.size(), scav_point{});

  uint32_t max_depth{ 0 };
  for (uint32_t const d : g.state_depth) { max_depth = imax(max_depth, d); }
  std::vector<std::vector<uint32_t>> &states_at{ x.sc.states_at };
  clear_buckets(states_at, max_depth + 1);
  std::vector<std::vector<uint32_t>> &subs_at{ x.sc.subs_at };
  clear_buckets(subs_at, max_depth + 2);
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (c.states[i].live != 0) { vec_push_back(states_at[g.state_depth[i]], i); }
  }
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    StateId const owner{ c.submachines[m].owner };
    vec_push_back(subs_at[(owner.v == INVALID) ? 0 : g.state_depth[owner.v] + 1], m);
  }

  vec_assign(x.seg_label_h, g.segments.size(), 0);
  vec_assign(x.seg_label_w, g.segments.size(), 0);
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    scav_path_box const &box{ s.path_box[i] };
    uint32_t const at{ label_segment(c, g, box.subject) };
    if (at == INVALID) { continue; }
    bool const down{ x.runs_down(g.segments[at].frame.v) };
    x.seg_label_h[at] = imax(x.seg_label_h[at], down ? box.w : box.h);
    x.seg_label_w[at] = imax(x.seg_label_w[at], down ? box.h : box.w);
  }
  loop_rooms(c, s, p, x.loop_label, x.loop_room);
  vec_assign(x.port_seg, g.ports.size(), INVALID);
  for (uint32_t seg = 0; seg < o.seg_port.size(); ++seg) {
    if (o.seg_port[seg] < x.port_seg.size()) { x.port_seg[o.seg_port[seg]] = seg; }
  }

  std::vector<uint8_t> &reuse{ x.sc.reuse };
  std::vector<uint8_t> &kept{ x.sc.kept };
  reuse.clear();
  if (x.base != nullptr) {
    uint32_t const taken{ x.mark_reuse(subs_at, max_depth + 2) };
#ifdef SCAV_TESTING
    uint32_t live{ 0 };
    for (Submachine const &sm : c.submachines) { live += (sm.live != 0) ? 1U : 0U; }
    ScopedLock const held{ test_reuse_lock };
    test_reused += taken;
    test_framed += live;
#else
    static_cast<void>(taken);
#endif
  }
  bool const reusing{ (x.base != nullptr) && !reuse.empty() };
  if (record != nullptr) {
    record->ok = false;
    vec_assign(record->pre_node, o.nodes.size(), scav_point{});
    vec_assign(record->pre_sub, c.submachines.size(), scav_point{});
    vec_assign(record->edge_lean, o.edges.size(), 0);
  }

  // Levels interleave: the submachines whose children sit at this depth, then
  // the states one level up that wrap them; level 0 sizes the document roots.
  for (uint32_t level = max_depth + 2; level-- > 0;) {
    for (uint32_t const m : subs_at[level]) {
      if (reusing && (reuse[m] != 0)) {
        x.copy_frame(m);
        continue;
      }
      x.size_sub(m);
      if (record != nullptr) { x.note_frame(m); }
    }
    if (level > 0) {
      for (uint32_t const i : states_at[level - 1]) {
        if (reusing && (kept[i] != 0)) {
          x.copy_state(i);
        } else {
          x.size_state(i);
        }
      }
    }
  }
  if (record != nullptr) { x.note_pass(); }
  if (!x.ok) { return false; }

  // One descent from the root, adding each frame's origin. Everything stays inside
  // the sized extents, so int32 cannot leave the domain here.
  std::vector<Frame> &work{ x.sc.work };
  work.clear();
  if (c.root_submachine.v != INVALID) {
    out.chart = out.sub[c.root_submachine.v];
    vec_push_back(work, { .sub = c.root_submachine.v, .x = 0, .y = 0 });
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
      int32_t const sy{ r.y + pad + b.h_before };
      int32_t packed_h{ 0 };
      Span const subs{ c.states[i].submachines };
      for (uint32_t u = 0; u < subs.len; ++u) {
        uint32_t const m{ c.submachine_ids[subs.off + u].v };
        if (c.submachines[m].live == 0) { continue; }
        vec_push_back(
            work,
            { .sub = m, .x = ix + b.w_before + sub_local[m].x, .y = sy + sub_local[m].y });
        packed_h = imax(packed_h, sub_local[m].y + out.sub[m].h);
      }
      scav_extent const room{ x.room_of(i) };
      int32_t const room_y{ sy + packed_h +
                            (((packed_h > 0) && (room.h > 0)) ? p.sub_sep : 0) };
      int32_t const body{ (room.h > 0) ? ((room_y + room.h) - sy) : packed_h };
      int32_t const centre_end{ (ix + iw) - b.w_after };
      out.loop[i] = { .x = centre_end - room.w, .y = room_y, .w = room.w, .h = room.h };
      out.lead[i] = { .x = ix, .y = sy, .w = b.w_before, .h = body };
      out.trail[i] = { .x = centre_end, .y = sy, .w = b.w_after, .h = body };
      out.after[i] = { .x = ix, .y = sy + body, .w = iw, .h = b.h_after };
    }
  }
  return true;
}

}  // namespace

// Bracketed so a test can hand the hole reader two rects and read the ratio
// back, rather than inferring it from a whole chart's extents. The prototypes a
// test uses are its own; see scav_internal.h.
SCAV_INTERNAL_BEGIN

// A hole's aspect as a pair inside the profile's own `[1, 1024]` bounds, which
// is where `pack.cpp` proved its products. The longer axis takes the cap and
// the shorter one floors at 1, so an extreme hole reads as 1024:1 rather than
// as no ratio at all; a hole with no extent on an axis has no aspect.
FrameDar size_hole_ratio(int32_t w, int32_t h) {
  constexpr Wide CAP{ 1024 };
  if ((w <= 0) || (h <= 0)) { return {}; }
  Wide const num{ (w >= h) ? CAP : imax(floor_div(CAP * w, Wide{ h }), Wide{ 1 }) };
  Wide const den{ (w >= h) ? imax(floor_div(CAP * h, Wide{ w }), Wide{ 1 }) : CAP };
  return { .num = static_cast<int32_t>(num), .den = static_cast<int32_t>(den) };
}

// Per state, the aspect of the interior region its submachines are packed into:
// the interior width by the height between the `before` and `after` bands,
// which is the whole of the interior where a state requests neither band and
// includes whatever slack `kind_min_h` left. Zero for a state with no live
// submachine, which is no hole for anything to fill.
[[maybe_unused]] std::vector<FrameDar> size_owner_holes(Chart const &c,
                                                        SizedLayout const &z) {
  std::vector<FrameDar> hole;
  size_owner_holes(c, z, hole);
  return hole;
}

void size_owner_holes(Chart const &c, SizedLayout const &z, std::vector<FrameDar> &hole) {
  vec_assign(hole, c.states.size(), FrameDar{});
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (c.states[i].live == 0) { continue; }
    bool packed{ false };
    Span const subs{ c.states[i].submachines };
    for (uint32_t k = 0; k < subs.len; ++k) {
      packed = packed || (c.submachines[c.submachine_ids[subs.off + k].v].live != 0);
    }
    if (!packed) { continue; }
    // The ring is what the band origin sits inside, so it comes back off the
    // rects rather than off `pad`, which a bare pseudostate does not take.
    int32_t const pad{ z.before[i].y - z.state[i].y };
    int32_t const top{ z.before[i].y + z.before[i].h };
    int32_t bottom{ (z.state[i].y + z.state[i].h) - pad - z.after[i].h };
    if ((i < z.loop.size()) && (z.loop[i].h > 0)) { bottom = z.loop[i].y; }
    int32_t const sides{ ((i < z.lead.size()) ? z.lead[i].w : 0) +
                         ((i < z.trail.size()) ? z.trail[i].w : 0) };
    hole[i] = size_hole_ratio(z.before[i].w - sides, bottom - top);
  }
}

SCAV_INTERNAL_END

namespace {

bool size_passes(Chart const &c,
                 SplitGraph const &g,
                 SubmachineOrders const &o,
                 scav_spaces const &s,
                 scav_profile const &p,
                 SizedLayout &out,
                 std::vector<Diagnostic> &diags,
                 DarSource dar,
                 Compaction compaction,
                 Fold fold,
                 SizeRecord const *base,
                 SizeRecord *record) {
  SizePassRecord const *was{ (base != nullptr) ? base->pass.data() : nullptr };
  SizePassRecord *now{ (record != nullptr) ? record->pass.data() : nullptr };
  if (record != nullptr) { record->pass[1].ok = false; }
  if (dar == DarSource::Profile) {
    return size_pass(c, g, o, s, p, {}, compaction, fold, out, diags, was, now);
  }
  // Owner holes come off a first pass at the profile's ratio, packed the same way.
  SizedLayout &first{ size_scratch().first };
  if (!size_pass(c, g, o, s, p, {}, compaction, fold, first, diags, was, now)) {
    return false;
  }
  std::vector<FrameDar> &hole{ size_scratch().hole };
  size_owner_holes(c, first, hole);
  return size_pass(c,
                   g,
                   o,
                   s,
                   p,
                   hole,
                   compaction,
                   fold,
                   out,
                   diags,
                   (was != nullptr) ? &base->pass[1] : nullptr,
                   (now != nullptr) ? &record->pass[1] : nullptr);
}

#ifdef SCAV_TESTING
template <typename T>
bool same_rows(std::vector<T> const &a, std::vector<T> const &b) {
  return (a.size() == b.size()) &&
         (a.empty() || (std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0));
}

bool same_sized(SizedLayout const &a, SizedLayout const &b) {
  return same_rows(a.state, b.state) && same_rows(a.before, b.before) &&
         same_rows(a.after, b.after) && same_rows(a.lead, b.lead) &&
         same_rows(a.trail, b.trail) && same_rows(a.loop, b.loop) &&
         same_rows(a.sub, b.sub) && same_rows(a.node, b.node) &&
         same_rows(a.lean, b.lean) && same_rows(a.folded, b.folded) &&
         (std::memcmp(&a.chart, &b.chart, sizeof(scav_rect)) == 0);
}
#endif

}  // namespace

namespace {

// Past the domain by one at most, so a sum the box formula reads still overflows there.
int32_t saturate(Wide v) { return static_cast<int32_t>(imin(v, Wide{ COORD_MAX } + 1)); }

}  // namespace

std::array<scav_rect, 5> state_walls(SizedLayout const &z, uint32_t st) {
  auto const row = [st](std::vector<scav_rect> const &v) {
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
  return { top, bottom, sealed(row(z.lead)), sealed(row(z.trail)), row(z.loop) };
}

bool face_lined(scav_spaces const &s, uint32_t state, uint32_t face) {
  scav_box_space const b{ box_of(s.box_state, s.n_box_state, state) };
  std::array<int32_t, 4> const band{ b.w_before, b.w_after, b.h_before, b.h_after };
  return (face < 4) && (band[face] > 0);
}

int32_t loop_reach(scav_profile const &p) { return imax(p.pad, 1); }

int32_t loop_gap(scav_profile const &p) { return imax(p.pad / 2, 1); }

int32_t loop_lane(scav_profile const &p) {
  return imax(label_line_height(p), 2 * route_clearance(p));
}

LoopRow loop_row(scav_profile const &p, scav_extent label) {
  int32_t const legs{ loop_lane(p) +
                      (2 * route_clearance(p)) };  // a clearance off each edge
  return { .label_w = label.w, .label_h = label.h, .h = imax(label.h, legs) };
}

void loop_rooms(Chart const &c,
                scav_spaces const &s,
                scav_profile const &p,
                std::vector<scav_extent> &label,
                std::vector<scav_extent> &room) {
  vec_assign(label, c.transitions.size(), scav_extent{});
  vec_assign(room, c.states.size(), scav_extent{});
  for (uint32_t i = 0; (s.path_box != nullptr) && (i < s.n_path_box); ++i) {
    uint32_t const t{ s.path_box[i].subject };
    if ((t >= c.transitions.size()) || !inner_loop(c, t)) { continue; }
    label[t].w = imax(label[t].w, s.path_box[i].w);
    label[t].h = saturate(Wide{ label[t].h } + s.path_box[i].h);
  }
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if (!inner_loop(c, t)) { continue; }
    LoopRow const row{ loop_row(p, label[t]) };
    scav_extent &r{ room[c.transitions[t].src.v] };
    int32_t const w{ ((row.label_w > 0) ? (row.label_w + loop_gap(p)) : 0) +
                     loop_reach(p) };
    r.w = imax(r.w, w);
    r.h = saturate(Wide{ r.h } + row.h);
  }
}

void loop_rows(Chart const &c,
               SizedLayout const &z,
               scav_spaces const &s,
               scav_profile const &p,
               std::vector<scav_extent> &label,
               std::vector<scav_rect> &row) {
  thread_local std::vector<scav_extent> room;
  loop_rooms(c, s, p, label, room);
  vec_assign(row, c.transitions.size(), scav_rect{});
  thread_local std::vector<int32_t> cursor;
  vec_assign(cursor, c.states.size(), 0);
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if (!inner_loop(c, t)) { continue; }
    uint32_t const st{ c.transitions[t].src.v };
    if (st >= z.loop.size()) { continue; }
    scav_rect const &r{ z.loop[st] };
    int32_t const h{ loop_row(p, label[t]).h };
    row[t] = { .x = r.x, .y = r.y + cursor[st], .w = r.w, .h = h };
    cursor[st] += h;
  }
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
                 Fold fold,
                 SizeRecord const *base,
                 SizeRecord *record) {
  if (static_cast<void const *>(base) == static_cast<void const *>(record)) {
    base = nullptr;
  }
  bool const done{
    size_passes(c, g, o, s, p, out, diags, dar, compaction, fold, base, record)
  };
#ifdef SCAV_TESTING
  if (test_reuse_verify && (base != nullptr)) {
    SizedLayout again;
    std::vector<Diagnostic> spilled;
    bool const redone{
      size_passes(c, g, o, s, p, again, spilled, dar, compaction, fold, nullptr, nullptr)
    };
    bool const same{ (done == redone) && (!done || same_sized(out, again)) };
    ScopedLock const held{ test_reuse_lock };
    test_reuse_mismatches += same ? 0U : 1U;
  }
#endif
  return done;
}

#ifdef SCAV_TESTING
void size_test_reuse(bool on) { test_reuse = on; }
void size_test_reuse_verify(bool on) {
  test_reuse_verify = on;
  ScopedLock const held{ test_reuse_lock };
  test_reused = 0;
  test_framed = 0;
  test_reuse_mismatches = 0;
}
void size_test_reuse_ignore_dar(bool on) { test_reuse_ignore_dar = on; }
uint64_t size_test_reused() {
  ScopedLock const held{ test_reuse_lock };
  return test_reused;
}
uint64_t size_test_framed() {
  ScopedLock const held{ test_reuse_lock };
  return test_framed;
}
uint64_t size_test_reuse_mismatches() {
  ScopedLock const held{ test_reuse_lock };
  return test_reuse_mismatches;
}
#endif

}  // namespace scav
