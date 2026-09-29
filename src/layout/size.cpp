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
SCAV_INTERNAL_END

namespace {

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

// The profile is read whole into a key, so it has to be words and no padding.
static_assert((sizeof(scav_profile) % sizeof(uint32_t)) == 0);

// Inside the coordinate domain on both axes. A row or a column whose sum
// overflowed saturates at `PACK_SATURATED`, which is far past `COORD_MAX`.
bool fits(Packing const &p) { return (p.w <= COORD_MAX) && (p.h <= COORD_MAX); }

// The better-scaling of the two packings, among those inside the domain. A
// packing outside it cannot compose a box inside it, so it is no candidate.
// The ratio and the compaction knob are arguments rather than the profile's
// fields, because a frame aims at the hole it fills and not every hole is
// 16:10, and because compaction is a row of the portfolio's table (11.4).
Packing pack_best(std::vector<scav_rect> const &rects,
                  int32_t sep,
                  scav_profile const &p,
                  FrameDar dar,
                  Compaction compaction) {
  Packing packed{ pack_lr(rects, sep, dar.num, dar.den, compaction) };
  if (p.trybox != 0) {
    Packing const row{ pack_box(rects, sep) };
    if (fits(row) && (!fits(packed) ||
                      pack_better(row, packed, dar.num, dar.den, p.sm_tiebreak != 0))) {
      packed = row;
    }
  }
  return packed;
}

void overflow(std::vector<Diagnostic> &diags, ElemKind kind, uint32_t ordinal) {
  diags.push_back({ .code = DiagCode::CoordinateOverflow,
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
                      uint32_t i) {
  if ((c.states[i].kind == StateKind::Normal) || (b.h_before != 0) || (b.h_after != 0)) {
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
  // For the trace, of the shape that is kept: where pieces started and
  // where a cut was refused, and what seating each pseudostate got.
  std::vector<TraceFold> cuts;
  std::vector<std::pair<uint32_t, SeatHow>> seated;
  std::vector<std::pair<uint32_t, int32_t>> centred;
  std::vector<TracePiece> packed;
  std::vector<TraceGap> lanes;
  // Some piece is packed other than beside the one before it.
  bool wraps{ false };
  // Every edge a cut crosses between stacked pieces joins two plain nodes.
  bool drawable{ true };

  // A default shape of `n` nodes, reusing every buffer's storage.
  void reset(size_t n) {
    at.assign(n, scav_point{});
    w = 0;
    h = 0;
    ok = true;
    cuts.clear();
    seated.clear();
    centred.clear();
    packed.clear();
    lanes.clear();
    wraps = false;
    drawable = true;
  }
};

// One chunk of one component as `lay_out_sub` holds it, for the steps along
// its ranks: `nodes` is the component in frame-local indices, `index` a frame
// node's place in it, and `chunk_index` a component node's place in the
// chunk, whose `centre`, `seat_at` and layers these are.
struct ChunkView {
  Span span, espan, gspan;
  bool down;
  uint32_t first, last;
  std::vector<uint32_t> const &nodes, &index, &local_rank, &global_rank;
  std::vector<uint32_t> const &chunk_index, &chunk_nodes;
  std::vector<int32_t> const &line, &centre, &seat_at;
  CoordGraph const &cg;
};

// A frame the descent has yet to visit, and its root-absolute origin.
struct Frame {
  uint32_t sub;
  int32_t x, y;
};

// Every buffer a sizing pass uses, kept per thread and reassigned in place, so
// a pass after the first allocates only its result and what outgrows the
// passes before it. Sizing never waits on the pool and lays out one frame at a
// time, so no second pass or frame on this thread can start while one is
// using these.
struct SizeScratch {
  // The pass, and the states it sizes.
  std::vector<scav_point> sub_local;
  std::vector<int32_t> seg_label_h, seg_label_w;
  std::vector<uint32_t> port_seg;
  std::vector<std::vector<uint32_t>> states_at, subs_at;
  std::vector<scav_rect> kids;
  std::vector<uint32_t> ids;
  std::vector<Frame> work;
  // What an `OwnerHole` sizing's first pass sized.
  SizedLayout first;
  // One frame.
  std::vector<int32_t> reserve;
  std::vector<uint32_t> adj_count, adj_off, adj, fill, component, queue;
  std::vector<uint32_t> member_off, member, out_deg;
  std::vector<scav_point> local;
  std::vector<scav_rect> boxes;
  // One component, and one layout of it.
  std::vector<uint32_t> nodes, global_rank, local_rank, index, in_layer;
  std::vector<uint32_t> group, labelled, grouped, chunk_of, chunks;
  std::vector<int32_t> extent, layer_w, widest_label, group_w, line;
  std::vector<Wide> layer_h, carry, applied;
  std::vector<uint8_t> glued, paired, bare;
  std::vector<scav_rect> pieces;
  Packing packed;
  Shape best, folded, stacked;
  // One chunk. `spare_layers` holds the layers a smaller chunk dropped.
  CoordGraph cg;
  std::vector<std::vector<uint32_t>> spare_layers;
  std::vector<uint32_t> chunk_index, chunk_nodes, arrivals, nearest;
  std::vector<int32_t> seat_at, enter_at;
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
  if (buckets.size() < n) { buckets.resize(n); }
  for (std::vector<uint32_t> &b : buckets) { b.clear(); }
}

// Exactly `n` empty layers, moving storage to and from `spare` rather than
// freeing it.
void clear_layers(std::vector<std::vector<uint32_t>> &layers,
                  std::vector<std::vector<uint32_t>> &spare,
                  size_t n) {
  while (layers.size() > n) {
    spare.push_back(std::move(layers.back()));
    layers.pop_back();
  }
  while ((layers.size() < n) && !spare.empty()) {
    layers.push_back(std::move(spare.back()));
    spare.pop_back();
  }
  layers.resize(n);
  for (std::vector<uint32_t> &l : layers) { l.clear(); }
}

// The inputs one sizing pass shares between its frames and states, and what
// it has sized so far: each of `size_pass`'s steps is a member, so the frame
// and state layouts read the same names they read as the pass's own locals.
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
  bool ok{ true };

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
  // `seg_cross` at a segment, and whether a node is a boundary node on a cross
  // border. Hand-built orders carry no column.
  [[nodiscard]] uint8_t cross_of(uint32_t seg) const {
    return (seg < o.seg_cross.size()) ? o.seg_cross[seg] : uint8_t{ 0 };
  }
  [[nodiscard]] bool on_cross_border(uint32_t node) const {
    return (o.nodes[node].kind == OrderKind::Boundary) &&
           (cross_of(o.nodes[node].subject) != 0);
  }
  [[nodiscard]] int32_t attach_at(uint32_t seg, uint32_t state, bool down) const;
  void trace_ports(uint32_t m, bool down) const;
  [[nodiscard]] uint32_t connected_components(Span span, Span espan);
  void count_turns(ChunkView const &v);
  void step_layers(ChunkView const &v, std::vector<TraceGap> &lanes);
  void lay_out_sub(uint32_t m);
  void place_sub(uint32_t m,
                 bool down,
                 FrameDar dar,
                 std::vector<scav_rect> const &boxes,
                 std::vector<uint32_t> const &component,
                 std::vector<scav_point> const &local);
  void size_sub(uint32_t m);
  void size_state(uint32_t i);
};

// Where `seg` meets `state`, from the state's centre across the ranks of the
// frame `state` sits in: where the boundary node inside it stands for a port
// on its border, which is sized by now because a state is sized before the
// frame it sits in. Zero where the segment meets the state's box rather than
// a port, and where the frame inside runs the other way, whose ports are on
// the faces this frame's edges do not arrive at.
int32_t Sizer::attach_at(uint32_t seg, uint32_t state, bool down) const {
  if (seg >= g.segments.size()) { return 0; }  // a hand-built frame
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
    Wide const pad{ bare_pseudostate(c, out.sub, b, state) ? 0 : p.pad };
    if (down) {
      Wide const x{ pad + sub_local[frame].x + out.node[node].x };
      return static_cast<int32_t>(x - (out.state[state].w / 2));
    }
    Wide const y{ pad + b.h_before + sub_local[frame].y + out.node[node].y };
    return static_cast<int32_t>(y - (out.state[state].h / 2));
  }
  return 0;
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

// Components in first-node order, and nodes within a component in (rank, pos)
// order, so both are the reading order the frame was emitted in: `component`
// per node, and component `id`'s nodes from `member[member_off[id]]`.
uint32_t Sizer::connected_components(Span span, Span espan) {
  std::vector<uint32_t> &adj_count{ sc.adj_count };
  adj_count.assign(span.len, 0);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    ++adj_count[e.src - span.off];
    ++adj_count[e.dst - span.off];
  }
  std::vector<uint32_t> &adj_off{ sc.adj_off };
  adj_off.assign(size_t{ span.len } + 1, 0);
  for (uint32_t k = 0; k < span.len; ++k) { adj_off[k + 1] = adj_off[k] + adj_count[k]; }
  std::vector<uint32_t> &adj{ sc.adj };
  adj.assign(adj_off[span.len], 0);
  std::vector<uint32_t> &fill{ sc.fill };
  fill.assign(adj_off.begin(), adj_off.end() - 1);
  for (uint32_t k = 0; k < espan.len; ++k) {
    OrderEdge const &e{ o.edges[espan.off + k] };
    adj[fill[e.src - span.off]++] = e.dst - span.off;
    adj[fill[e.dst - span.off]++] = e.src - span.off;
  }

  std::vector<uint32_t> &component{ sc.component };
  component.assign(span.len, INVALID);
  std::vector<uint32_t> &queue{ sc.queue };
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
  std::vector<uint32_t> &member_off{ sc.member_off };
  member_off.assign(size_t{ components } + 1, 0);
  for (uint32_t k = 0; k < span.len; ++k) { ++member_off[component[k] + 1]; }
  for (uint32_t id = 0; id < components; ++id) { member_off[id + 1] += member_off[id]; }
  std::vector<uint32_t> &member{ sc.member };
  member.assign(span.len, 0);
  fill.assign(member_off.begin(), member_off.end() - 1);
  for (uint32_t k = 0; k < span.len; ++k) { member[fill[component[k]]++] = k; }
  return components;
}

// The lanes the alignment leaves turning in each boundary of the chunk, into
// `turning` and `turned_by`. An edge whose two ends meet across the ranks runs
// straight through its boundary and takes no lane there: an end is its port
// where the edge meets a composite at one, the mark's centre on a glyph reached
// at the middle of a face, and the straight zone of the face on any other box.
// A point holds one straight edge a side, and a face whose seats are spread
// across it meets a point at no height it can be sure of, so an end shared with
// another edge on that side runs straight only between two faces. A
// pseudostate whose every edge here joins one neighbour is levelled with it by
// the seating, so its edge runs straight, and it sits in the corridor beside
// that neighbour, so every other edge on that side of the neighbour goes round
// it. An edge across several layers turns in its first boundary and its last,
// as phase 1 counts it.
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

  // Per chunk node, the node the seating moves it beside, or INVALID: an
  // initial before the one node it joins in the next layer, a final after the
  // one it joins in the layer before, wherever the box lies inside the piece.
  Wide chunk_h{ 0 };
  for (uint32_t i = 0; i < n; ++i) {
    chunk_h = imax(chunk_h, (Wide{ v.centre[i] } - (v.cg.extent[i] / 2)) + v.cg.extent[i]);
  }
  std::vector<uint32_t> &seat_to{ sc.seat_to };
  seat_to.assign(n, INVALID);
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

  std::vector<uint32_t> &turning{ sc.turning };
  turning.assign(v.last - v.first, 0);
  std::vector<uint32_t> &turned_by{ sc.turned_by };
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

// Each layer's place along the ranks inside the chunk, into `layer_x`, and the
// room each keeps, into `kept_w`, after `count_turns`. A boundary is given its
// labels, or its turning lanes a line of type apart where two or more turn
// there; the lanes it holds go to `lanes` for the trace.
//
// A pseudostate the seating moves beside its neighbour goes `rank_sep` before
// it, or after it for a final. Where that box, grown by half `node_sep`, lies
// inside the neighbour's own layer, nothing is left of the pseudostate in its
// own and the layer is sized without it; a layer left holding only boundary
// nodes is open, as `sep_after` has it. Otherwise it keeps its own layer's
// room, and the step to its neighbour's layer grows until the box clears every
// node of its own layer at the height it moves to.
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
    lanes.push_back({ .boundary = v.global_rank[r],
                      .seg = sc.turned_by[r - v.first],
                      .width = static_cast<int32_t>(lanes_at(r)) });
  }

  auto const inset_of = [&](uint32_t i) {
    return (v.line[i] == 0) ? Wide{ 0 }
                            : ((Wide{ v.line[i] } - along(node_of(i).subject)) / 2);
  };
  std::vector<uint32_t> const &seat_to{ sc.seat_to };
  std::vector<uint8_t> &left{ sc.left };
  left.assign(v.chunk_nodes.size(), 0);
  std::vector<Wide> &kept_w{ sc.kept_w };
  kept_w.assign(v.last - v.first, 0);
  for (uint32_t r = v.first; r < v.last; ++r) {
    for (uint32_t const i : v.cg.layers[r - v.first]) {
      OrderNode const &nd{ node_of(v.chunk_nodes[i]) };
      if (nd.kind != OrderKind::State) { continue; }
      uint32_t const one{ seat_to[i] };
      if (one != INVALID) {
        uint32_t const near{ v.chunk_nodes[one] };
        uint32_t const nr{ v.local_rank[v.nodes[near]] };
        Wide const need{ Wide{ p.rank_sep } + along(nd.subject) + (p.node_sep / 2) };
        Wide const room{ (nr > r) ? inset_of(near)
                                  : (kept_w[nr - v.first] - inset_of(near) -
                                     along(node_of(near).subject)) };
        left[i] = (room >= need) ? 1U : 0U;
      }
      if (left[i] != 0) { continue; }
      kept_w[r - v.first] =
          imax(kept_w[r - v.first],
               imax(Wide{ along(nd.subject) }, Wide{ v.line[v.chunk_nodes[i]] }));
    }
  }
  std::vector<uint8_t> &open{ sc.open };
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
      if ((seat_to[i] == INVALID) || (left[i] != 0)) { continue; }
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

  // An anchored label needs `leader + box height` beside its leg, and phase 3
  // has only the cross-axis room phase 2 left (11.9.5). Carried on the extent
  // of both ends -- the slot `cross_coordinates` separates by, not the drawn
  // rect -- so half lands each side and the pair opens the whole distance.
  //
  // The whole distance, not its shortfall against `node_sep`: the leg leaves
  // its node's *centre*, and the shortfall opens the gap between node *edges*.
  int32_t const leader{ label_leader(p) };
  std::vector<int32_t> &reserve{ sc.reserve };
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
  std::vector<uint32_t> const &member{ sc.member };
  std::vector<uint32_t> const &member_off{ sc.member_off };
  std::vector<scav_point> &local{ sc.local };
  local.assign(span.len, scav_point{});
  std::vector<scav_rect> &boxes{ sc.boxes };
  boxes.assign(components, scav_rect{});
  for (uint32_t id = 0; id < components; ++id) {
    // A port on a cross border is placed on the frame's edge by `place_sub`
    // and takes no room in its rank.
    sc.nodes.clear();
    for (uint32_t k = member_off[id]; k < member_off[id + 1]; ++k) {
      if (!on_cross_border(span.off + member[k])) { sc.nodes.push_back(member[k]); }
    }
    std::vector<uint32_t> const &nodes{ sc.nodes };

    // Ranks renumbered from zero, with the frame's rank kept alongside so a label's
    // gap still lands where it was charged.
    std::vector<uint32_t> &global_rank{ sc.global_rank };
    global_rank.clear();
    std::vector<uint32_t> &local_rank{ sc.local_rank };
    local_rank.assign(span.len, INVALID);
    for (uint32_t const k : nodes) {
      uint32_t const rank{ o.nodes[span.off + k].rank };
      if (global_rank.empty() || (global_rank.back() != rank)) {
        global_rank.push_back(rank);
      }
      local_rank[k] = static_cast<uint32_t>(global_rank.size()) - 1;
    }
    uint32_t const layers{ static_cast<uint32_t>(global_rank.size()) };
    std::vector<uint32_t> &index{ sc.index };
    index.assign(span.len, INVALID);
    std::vector<int32_t> &extent{ sc.extent };
    extent.assign(nodes.size(), 0);
    std::vector<int32_t> &layer_w{ sc.layer_w };
    layer_w.assign(layers, 0);
    std::vector<Wide> &layer_h{ sc.layer_h };
    layer_h.assign(layers, 0);
    // The reserve keeps a label clear of the node beside its end in the same
    // layer. A frame running down puts a label's width beside its leg, so a
    // node alone in its layer there takes none: the room from its frame's
    // edge is taken on one side of the leg below, and the half the reserve
    // put on the other side was empty (11.10g). Across, a label's height is
    // small and the reserve is also the gap a fold's stacked pieces keep.
    std::vector<uint32_t> &in_layer{ sc.in_layer };
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
    // States joined by an edge inside one column share one centre line, so
    // the route between any two runs straight down the overlap of their
    // faces; left-aligned, the narrower one's own width at the column's
    // leading edge was the only face they shared. The line is the centre of
    // the widest of them, or of the room a label on one of those edges
    // needs beside its leg on either side, whichever is wider, and the
    // column grows to hold that room. Initial and final pseudostates are
    // seated beside the state they join below; a bar keeps the leading
    // edge, because a route to a state stacked on it would leave through its
    // cap rather than its length (11.10g).
    std::vector<uint32_t> &group{ sc.group };
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
    // A group with two labelled edges has a label either side of its line,
    // so each side needs the widest one's room; one label takes whichever
    // side has it.
    std::vector<uint32_t> &labelled{ sc.labelled };
    labelled.assign(nodes.size(), 0);
    std::vector<int32_t> &widest_label{ sc.widest_label };
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
    std::vector<int32_t> &group_w{ sc.group_w };
    group_w.assign(nodes.size(), 0);
    std::vector<uint32_t> &grouped{ sc.grouped };
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
    // Parallel to `nodes`: the width whose centre a grouped state sits on,
    // zero for one in no group.
    std::vector<int32_t> &line{ sc.line };
    line.assign(nodes.size(), 0);
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      uint32_t const root_of{ columnar(i) ? find(i) : INVALID };
      if ((root_of == INVALID) || (grouped[root_of] < 2)) { continue; }
      line[i] = group_w[root_of];
      uint32_t const r{ local_rank[nodes[i]] };
      layer_w[r] = imax(layer_w[r], group_w[root_of]);
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
    // A layer of boundary nodes alone takes no room along the ranks: each is
    // the point a route crosses the frame's border at, drawn on the frame's
    // edge rather than in the layer. The layer beside it keeps the room a
    // route keeps from a box, which also holds one turn, rather than
    // `rank_sep`; a label charged between them keeps `rank_sep`.
    std::vector<uint8_t> &bare{ sc.bare };
    bare.assign(layers, 1);
    for (uint32_t const k : nodes) {
      if (o.nodes[span.off + k].kind != OrderKind::Boundary) { bare[local_rank[k]] = 0; }
    }
    auto const sep_after = [&](uint32_t r) {
      bool const open{ (bare[r] != 0) || (bare[r + 1] != 0) };
      return (open && (label_gap(r) == 0)) ? Wide{ route_clearance(p) }
                                           : Wide{ p.rank_sep };
    };

    // A cut before a layer is refused where it would separate an initial
    // pseudostate from the state it enters in that layer, or a final one in
    // that layer from the state it leaves: the fold packs its pieces apart,
    // and the pseudostate would be drawn as a piece of its own, away from
    // the state it joins. Likewise a boundary node, the point a route
    // crosses the frame's border at: a piece of its own is packed below the
    // node it joins, and the port with it, which dropped `vac`'s `battery
    // low` a whole state's height off `Ready` (11.10g).
    std::vector<uint8_t> &glued{ sc.glued };
    glued.assign(layers, 0);
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
        glued[imax(ra, rb)] = 1;
      }
    }
    // Where `e`'s leg runs when it runs down the overlap of its two ends at
    // `xa` and `xb`, or -1 where they do not overlap and there is no straight
    // leg; and the room a label on it needs beside it. Zero room for an
    // unlabelled edge.
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
    // How wide a piece or a stack must be for `e`'s label to sit beside its
    // leg: the leg plus the room where the leading side has too little of
    // it, else zero.
    auto const beside_leg =
        [&](OrderEdge const &e, Wide xa, Wide xb, uint32_t a, uint32_t b) {
          Wide const leg{ leg_of(xa, xb, a, b) };
          Wide const need{ room_of(e) };
          return ((need == 0) || (leg < 0) || (leg >= need)) ? Wide{ 0 } : (leg + need);
        };
    // A rank run grows unbounded along one axis and nesting multiplies it by depth.
    // Cutting helps only sometimes -- at two ranks it worsens the aspect -- so both
    // shapes are laid out and the scale measure picks, as `trybox` picks a packer.
    auto const lay_out = [&](Shape &shape, Wide wrap_at, bool stack) {
      shape.reset(nodes.size());
      std::vector<uint32_t> &chunk_of{ sc.chunk_of };
      chunk_of.assign(layers, 0);
      std::vector<uint32_t> &chunks{ sc.chunks };
      chunks.assign(1, 0);
      Wide run{ 0 };
      for (uint32_t r = 0; r < layers; ++r) {
        Wide const step{ (r == 0) ? Wide{ layer_w[r] }
                                  : (Wide{ layer_w[r] } + sep_after(r - 1) +
                                     boundary_gap(r - 1)) };
        bool const wants_cut{ (r > 0) && ((run + step) > wrap_at) };
        if (wants_cut && (glued[r] != 0)) {
          shape.cuts.push_back({ .rank = global_rank[r], .refused = 1, .carried = 0 });
        }
        if (wants_cut && (glued[r] == 0)) {
          shape.cuts.push_back({ .rank = global_rank[r], .refused = 0, .carried = 0 });
          chunks.push_back(r);
          run = layer_w[r];
        } else {
          run += step;
        }
        chunk_of[r] = static_cast<uint32_t>(chunks.size()) - 1;
      }

      std::vector<scav_rect> &pieces{ sc.pieces };
      pieces.assign(chunks.size(), scav_rect{});
      std::vector<Wide> &carry{ sc.carry };
      carry.assign(chunks.size(), 0);
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
        chunk_index.assign(nodes.size(), INVALID);
        std::vector<uint32_t> &chunk_nodes{ sc.chunk_nodes };
        chunk_nodes.clear();
        for (uint32_t i = 0; i < nodes.size(); ++i) {
          uint32_t const r{ local_rank[nodes[i]] };
          if (chunk_of[r] != chunk) { continue; }
          chunk_index[i] = static_cast<uint32_t>(chunk_nodes.size());
          chunk_nodes.push_back(i);
          cg.extent.push_back(extent[i]);
          cg.layers[r - first].push_back(chunk_index[i]);
        }
        // An initial's target that one other edge enters by the same face
        // gives the two their own heights on it. By centres, the target
        // lined up with the initial -- which the seating below moves anyway
        // -- and the other route jogged to reach it, as `vac`'s `battery
        // low` did into `Seated`; level, the initial's dot would sit on that
        // route. So the other edge meets the target `half` off its centre and
        // the initial `half` the other way, where it is seated, and the
        // initial's edge is weak: it anchors the target only where the other
        // cannot, as in `estop`, where a pinned rank takes `Latched` from
        // `Clear` (11.10g). A composite is met at its ports, whose heights
        // are its own.
        std::vector<int32_t> &seat_at{ sc.seat_at };
        seat_at.assign(chunk_nodes.size(), 0);
        std::vector<int32_t> &enter_at{ sc.enter_at };
        enter_at.assign(chunk_nodes.size(), 0);
        std::vector<uint8_t> &apart{ sc.apart };
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
        // Per node, what enters its leading face other than an initial, and
        // the position of the nearest of those in the layer before.
        std::vector<uint32_t> &arrivals{ sc.arrivals };
        arrivals.assign(chunk_nodes.size(), 0);
        std::vector<uint32_t> &nearest{ sc.nearest };
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
        std::vector<int32_t> const centre{ cross_coordinates(cg) };

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
          // An initial pseudostate is ranked just before the state it enters,
          // so it sits flush against its layer's trailing edge: left-aligned
          // in a layer as wide as its widest member, its arrow would run that
          // whole width.
          OrderNode const &nd{ o.nodes[span.off + nodes[at]] };
          Wide const flush{ ((nd.kind == OrderKind::State) &&
                             (c.states[nd.subject].kind == StateKind::Initial))
                                ? imax(kept_w[r - first] - along(nd.subject), Wide{ 0 })
                                : Wide{ 0 } };
          // On its group's centre line, where it has one.
          Wide const inset{ (line[at] == 0)
                                ? flush
                                : ((Wide{ line[at] } - along(nd.subject)) / 2) };
          if (inset != flush) {
            shape.centred.emplace_back(nd.subject, static_cast<int32_t>(inset));
          }
          // Local to the piece; the packing below decides where the piece
          // itself goes.
          shape.at[at] = { .x = static_cast<int32_t>(layer_x[r - first] + inset),
                           .y = static_cast<int32_t>(centre[i]) };
        }

        // An initial or final pseudostate with one neighbour in this piece is
        // level with it and `rank_sep` from it -- before it for an initial,
        // after it for a final -- rather than wherever its layer's edge falls,
        // which is past the widest member of the layer between. Each only
        // where the box it moves to is clear of every other node here and
        // inside the piece (11.10g).
        auto const box_of_node = [&](uint32_t i, Wide x, Wide y) {
          OrderNode const &nd{ o.nodes[span.off + nodes[chunk_nodes[i]]] };
          Wide const w{ (nd.kind == OrderKind::State) ? Wide{ along(nd.subject) }
                                                      : Wide{ 0 } };
          Wide const h{ cg.extent[i] };
          return scav_rect{ .x = static_cast<int32_t>(x),
                            .y = static_cast<int32_t>(y - (h / 2)),
                            .w = static_cast<int32_t>(w),
                            .h = static_cast<int32_t>(h) };
        };
        for (uint32_t i = 0; i < chunk_nodes.size(); ++i) {
          uint32_t const at{ chunk_nodes[i] };
          OrderNode const &nd{ o.nodes[span.off + nodes[at]] };
          if (nd.kind != OrderKind::State) { continue; }
          StateKind const kind{ c.states[nd.subject].kind };
          if ((kind != StateKind::Initial) && (kind != StateKind::Final)) { continue; }
          uint32_t other{ INVALID };
          bool alone{ true };
          for (uint32_t k = 0; k < espan.len; ++k) {
            OrderEdge const &e{ o.edges[espan.off + k] };
            uint32_t const a{ index[e.src - span.off] };
            uint32_t const b{ index[e.dst - span.off] };
            uint32_t far{ INVALID };
            if (a == at) {
              far = b;
            } else if (b == at) {
              far = a;
            }
            if ((far == INVALID) || (far == at)) { continue; }
            if ((far >= chunk_index.size()) || (chunk_index[far] == INVALID)) {
              alone = false;
              continue;
            }
            if ((other != INVALID) && (other != chunk_index[far])) { alone = false; }
            other = chunk_index[far];
          }
          if (!alone || (other == INVALID)) { continue; }
          uint32_t const r{ local_rank[nodes[at]] };
          uint32_t const near_rank{ local_rank[nodes[chunk_nodes[other]]] };
          OrderNode const &nn{ o.nodes[span.off + nodes[chunk_nodes[other]]] };
          Wide const nw{ (nn.kind == OrderKind::State) ? Wide{ along(nn.subject) }
                                                       : Wide{ 0 } };
          Wide const nx{ shape.at[chunk_nodes[other]].x };
          // `rank_sep` from the state it joins, not a whole rank gap: the gap
          // also holds room other transitions' labels were charged.
          Wide x{ shape.at[at].x };
          if ((kind == StateKind::Final) && (r == (near_rank + 1))) {
            x = nx + nw + p.rank_sep;
          }
          if ((kind == StateKind::Initial) && (near_rank == (r + 1))) {
            x = nx - p.rank_sep - along(nd.subject);
          }
          Wide const y{ Wide{ shape.at[chunk_nodes[other]].y } + seat_at[i] };
          auto const clear = [&](Wide cx, Wide cy) {
            scav_rect const want{ box_of_node(i, cx, cy) };
            if ((want.x < 0) || (want.y < 0) || ((Wide{ want.x } + want.w) > chunk_w) ||
                ((Wide{ want.y } + want.h) > chunk_h)) {
              return false;
            }
            // A boundary node is drawn on the frame's edge, not where its
            // layer puts it, and the bounds above already keep the box off
            // that edge.
            scav_rect const room{ grow(want, p.node_sep / 2) };
            for (uint32_t j = 0; j < chunk_nodes.size(); ++j) {
              if ((j == i) || (o.nodes[span.off + nodes[chunk_nodes[j]]].kind ==
                               OrderKind::Boundary)) {
                continue;
              }
              scav_rect const there{
                box_of_node(j, shape.at[chunk_nodes[j]].x, shape.at[chunk_nodes[j]].y)
              };
              if (overlaps(room, there)) { return false; }
            }
            return true;
          };
          if (clear(x, y)) {
            shape.at[at] = { .x = static_cast<int32_t>(x), .y = static_cast<int32_t>(y) };
            shape.seated.emplace_back(nd.subject, SeatHow::Moved);
          } else if (clear(shape.at[at].x, y)) {
            shape.at[at].y = static_cast<int32_t>(y);
            shape.seated.emplace_back(nd.subject, SeatHow::Levelled);
          } else {
            shape.seated.emplace_back(nd.subject, SeatHow::Declined);
          }
        }
        // A label beside an edge inside one column, whose leg runs down it,
        // needs its own width and leader on one side of the leg inside the
        // piece; where neither side has it the piece grows on its trailing
        // side, as `dock`'s `contacts closed` needed once `On` stopped
        // carrying the label as a rank gap (11.10g).
        for (uint32_t k = 0; k < espan.len; ++k) {
          OrderEdge const &e{ o.edges[espan.off + k] };
          uint32_t const a{ in_chunk(e.src) };
          uint32_t const b{ in_chunk(e.dst) };
          if ((a == INVALID) || (b == INVALID) ||
              (local_rank[nodes[a]] != local_rank[nodes[b]])) {
            continue;
          }
          chunk_w = imax(chunk_w, beside_leg(e, shape.at[a].x, shape.at[b].x, a, b));
        }
        // Across the ranks the same: a label beside a leg between two ranks
        // needs its extent across them and the leader on one side of the
        // leg. The reserve above gives each side half of it, which holds a
        // label's height beside a leg running across and not a label's width
        // beside one running down, so where neither side has the whole the
        // piece grows on its trailing side (11.10g).
        for (uint32_t k = 0; k < espan.len; ++k) {
          OrderEdge const &e{ o.edges[espan.off + k] };
          uint32_t const a{ in_chunk(e.src) };
          uint32_t const b{ in_chunk(e.dst) };
          if ((a == INVALID) || (b == INVALID) ||
              (local_rank[nodes[a]] == local_rank[nodes[b]]) ||
              (e.segment >= seg_label_h.size()) || (seg_label_h[e.segment] == 0)) {
            continue;
          }
          auto const span_of = [&](uint32_t i, Wide &lo, Wide &hi) {
            OrderNode const &nd{ o.nodes[span.off + nodes[i]] };
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

      // Packed, not stacked: stacking left-aligned gives every piece the width of the
      // widest. They are rectangles sharing an area, which is `pack_lr`'s job (11.4).
      // `stack` is the other candidate: each piece under the one before at
      // the leading edge, like a wrapped line. It needs none of the room
      // below, and a piece narrower than the one it follows sits under it
      // rather than past a label's width to its right.
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
        packed = pack_best(pieces, p.node_sep, p, dar, compaction);
      }
      // A cut's label room goes on the new piece's leading edge only where the
      // packing puts that piece beside the one before it, so the leg between
      // them runs across the gap. Stacked, the leg runs down the gap the two
      // ends' own reserve already opened, and the room pushed the piece away
      // from the state it joins: `brew`'s `Pumping`, `dock`'s `Charging`.
      std::vector<Wide> &applied{ sc.applied };
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
          if (chunk_of[local_rank[nodes[i]]] == k) {
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
      if (widened) { packed = pack_best(pieces, p.node_sep, p, dar, compaction); }
      // Room for a label on an edge a cut crosses where the packing put its
      // two pieces one above the other, so its leg runs down the gap between
      // them. Two labelled edges between one pair run as a pair of legs with
      // a label outside each, so they need the room on both sides, and the
      // leading side is made by moving every piece along: one side each left
      // `ota`'s pair printing over both its states (11.10g).
      auto const node_x = [&](uint32_t i) {
        return Wide{ shape.at[i].x } + packed.at[chunk_of[local_rank[nodes[i]]]].x;
      };
      auto const cut_ends = [&](OrderEdge const &e, uint32_t &a, uint32_t &b) {
        a = index[e.src - span.off];
        b = index[e.dst - span.off];
        if ((a == INVALID) || (b == INVALID) || (room_of(e) == 0)) { return false; }
        uint32_t const ca{ chunk_of[local_rank[nodes[a]]] };
        uint32_t const cb{ chunk_of[local_rank[nodes[b]]] };
        scav_rect const &pa{ packed.at[ca] };
        scav_rect const &pb{ packed.at[cb] };
        return (ca != cb) && ((pa.y >= (pb.y + pb.h)) || (pb.y >= (pa.y + pa.h)));
      };
      std::vector<uint8_t> &paired{ sc.paired };
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
        if (!leading) { stack_w = imax(stack_w, leg + room_of(e)); }
      }
      for (scav_rect &at : packed.at) { at.x = static_cast<int32_t>(Wide{ at.x } + lead); }
      packed.w = static_cast<int32_t>(imin(stack_w, Wide{ PACK_SATURATED }));
      // A fold is drawable only where every edge a cut crosses between two
      // stacked pieces joins two plain nodes: an edge into a composite enters
      // by a port on its left or right border, and stacked it goes round to
      // reach it, as `vac`'s `full` did once `dock`'s `lamp` sat under `On`
      // (11.10g). The scale measure cannot weigh that against the area the
      // fold saves, so where it chooses such a fold is no candidate; where
      // the row always folds, `Cost` over the whole chart does the weighing.
      for (uint32_t k = 0; k < espan.len; ++k) {
        OrderEdge const &e{ o.edges[espan.off + k] };
        uint32_t const a{ index[e.src - span.off] };
        uint32_t const b{ index[e.dst - span.off] };
        if ((a == INVALID) || (b == INVALID)) { continue; }
        scav_rect const &pa{ packed.at[chunk_of[local_rank[nodes[a]]]] };
        scav_rect const &pb{ packed.at[chunk_of[local_rank[nodes[b]]]] };
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
      // A saturated position would leave int32 when the offset below added
      // to it, and no caller reads a shape this phase goes on to diagnose.
      if (!shape.ok) { return; }
      for (uint32_t i = 0; i < nodes.size(); ++i) {
        scav_rect const &at{ packed.at[chunk_of[local_rank[nodes[i]]]] };
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
    // A frame turned to run down is one column, which is what turning it was
    // for: folded, its run wraps back into columns side by side, the drawing
    // the frame running across already offers.
    Wide const unwrapped{ Wide{ COORD_MAX } * 2 };
    Shape &best{ sc.best };
    Shape &folded{ sc.folded };
    Shape &stacked{ sc.stacked };
    lay_out(best, unwrapped, false);
    lay_out(folded, down ? unwrapped : target, false);
    lay_out(stacked, down ? unwrapped : target, true);
    // The two folds are one cut in two arrangements, so the scale measure
    // picks between them before either is weighed against the flat run.
    bool const always{ fold == Fold::Always };
    auto const usable = [always](Shape const &f) {
      return f.ok && f.wraps && (f.drawable || always);
    };
    if (usable(stacked) &&
        (!usable(folded) || (stacked.drawable && !folded.drawable) ||
         ((stacked.drawable == folded.drawable) && better(stacked, folded)))) {
      std::swap(folded, stacked);
    }
    // `Always` takes the folded shape wherever it laid out, so what chose is
    // `Cost` over the row rather than the scale measure inside the frame. A
    // fold whose pieces pack back into one row in their own order is the
    // run unfolded at `node_sep` rather than `rank_sep`, with every edge the
    // cuts cross dropped from the alignment, so it is no candidate.
    bool const swap{
      folded.ok &&
      (!best.ok || (folded.wraps && (always || (folded.drawable && better(folded, best)))))
    };
    if (swap) { std::swap(best, folded); }
    for (TraceFold const &cut : best.cuts) {
      trace_emit({ .kind = TraceKind::FoldCut, .frame = m, .fold = cut });
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
  Packing const packed{ pack_best(boxes, p.node_sep, p, dar, compaction) };
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
  out_deg.assign(span.len, 0);
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

  // A port on a cross border sits on the frame's edge across the ranks, and
  // along them level with where its flat edge meets the state it joins: the
  // state's centre, or the port on its border the edge continues through.
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

// `lay_out_sub` from the memo where it has seen the frame's inputs before.
// The key is every value it reads -- the frame's nodes and edges, the sizes
// and kinds of the states in it, where each edge meets a composite's port,
// its labels, gaps, ratio, and the whole profile -- so a hit is the layout it
// would compute. Node indices in the key are the frame's own, since where a
// frame starts in the merged arrays moves whenever a frame before it changes.
// A traced run lays every frame out, since a hit would emit nothing.
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
  auto const put = [](int32_t v) { key.push_back(static_cast<uint32_t>(v)); };
  key.push_back(span.len);
  key.push_back(o.sub_ranks[m]);
  key.push_back(down ? 1U : 0U);
  put(dar.num);
  put(dar.den);
  key.push_back(static_cast<uint32_t>(compaction));
  key.push_back(static_cast<uint32_t>(fold));
  std::array<uint32_t, sizeof(scav_profile) / sizeof(uint32_t)> knobs{};
  std::memcpy(knobs.data(), &p, sizeof(scav_profile));
  key.insert(key.end(), knobs.begin(), knobs.end());
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
    key.push_back(static_cast<uint32_t>(c.states[nd.subject].kind));
    key.push_back(c.states[nd.subject].submachines.len);
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
      put((nd.kind == OrderKind::State) ? attach_at(e.segment, nd.subject, down) : 0);
      put((nd.kind == OrderKind::State) ? attach_at(e.segment, nd.subject, !down) : 0);
    }
  }
  key.push_back(gspan.len);
  for (uint32_t k = 0; k < gspan.len; ++k) { put(o.gaps[gspan.off + k]); }

  // What it writes: the frame's extent, each node's place, and each state's.
  Memo &memo{ frame_memo() };
  int32_t const *hit{ nullptr };
  uint32_t len{ 0 };
  if (memo.find(key, hit, len)) {
    out.sub[m].w = hit[0];
    out.sub[m].h = hit[1];
    uint32_t at{ 2 };
    for (uint32_t k = 0; k < span.len; ++k) {
      OrderNode const &nd{ o.nodes[span.off + k] };
      out.node[span.off + k] = { .x = hit[at], .y = hit[at + 1] };
      at += 2;
      if (nd.kind != OrderKind::State) { continue; }
      out.state[nd.subject].x = hit[at];
      out.state[nd.subject].y = hit[at + 1];
      at += 2;
    }
    return;
  }
  bool const was{ ok };
  lay_out_sub(m);
  if (!was || !ok) { return; }
  value.clear();
  value.push_back(out.sub[m].w);
  value.push_back(out.sub[m].h);
  for (uint32_t k = 0; k < span.len; ++k) {
    OrderNode const &nd{ o.nodes[span.off + k] };
    value.push_back(out.node[span.off + k].x);
    value.push_back(out.node[span.off + k].y);
    if (nd.kind != OrderKind::State) { continue; }
    value.push_back(out.state[nd.subject].x);
    value.push_back(out.state[nd.subject].y);
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
    kids.push_back(out.sub[m]);
    ids.push_back(m);
  }
  Packing packed;
  if (!kids.empty()) {
    packed = pack_best(kids, p.sub_sep, p, dar_of(i), compaction);
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
  Wide const ring{ bare_pseudostate(c, out.sub, b, i) ? Wide{ 0 }
                                                      : (2 * static_cast<Wide>(p.pad)) };
  Wide const w{ imax(imax(Wide{ b.min_w }, Wide{ packed.w }), Wide{ min_w }) + ring };
  Wide const h{ imax(Wide{ b.h_before } + packed.h + b.h_after, Wide{ min_h }) + ring };
  if ((w > COORD_MAX) || (h > COORD_MAX)) {
    overflow(diags, ElemKind::State, i);
    ok = false;
    return;
  }
  out.state[i].w = static_cast<int32_t>(w);
  out.state[i].h = static_cast<int32_t>(h);
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
  out.sub.assign(c.submachines.size(), {});
  out.node.assign(o.nodes.size(), {});
  out.chart = {};
  std::vector<scav_point> &sub_local{ x.sub_local };
  sub_local.assign(c.submachines.size(), scav_point{});

  uint32_t max_depth{ 0 };
  for (uint32_t const d : g.state_depth) { max_depth = imax(max_depth, d); }
  std::vector<std::vector<uint32_t>> &states_at{ x.sc.states_at };
  clear_buckets(states_at, max_depth + 1);
  std::vector<std::vector<uint32_t>> &subs_at{ x.sc.subs_at };
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

  // One descent from the root, adding each frame's origin. Everything stays inside
  // the sized extents, so int32 cannot leave the domain here.
  std::vector<Frame> &work{ x.sc.work };
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
      // Root-absolute and after the descent, so a bend reads as the coordinate
      // the router will be handed rather than a frame-local one (11.16).
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
      int32_t const pad{ bare_pseudostate(c, out.sub, b, i) ? 0 : p.pad };
      int32_t const ix{ r.x + pad };
      int32_t const iw{ r.w - (2 * pad) };
      out.before[i] = { .x = ix, .y = r.y + pad, .w = iw, .h = b.h_before };
      int32_t const sy{ r.y + pad + b.h_before };
      int32_t packed_h{ 0 };
      Span const subs{ c.states[i].submachines };
      for (uint32_t u = 0; u < subs.len; ++u) {
        uint32_t const m{ c.submachine_ids[subs.off + u].v };
        if (c.submachines[m].live == 0) { continue; }
        work.push_back({ .sub = m, .x = ix + sub_local[m].x, .y = sy + sub_local[m].y });
        packed_h = imax(packed_h, sub_local[m].y + out.sub[m].h);
      }
      out.after[i] = { .x = ix, .y = sy + packed_h, .w = iw, .h = b.h_after };
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
std::vector<FrameDar> size_owner_holes(Chart const &c, SizedLayout const &z) {
  std::vector<FrameDar> hole(c.states.size(), FrameDar{});
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
    int32_t const bottom{ (z.state[i].y + z.state[i].h) - pad - z.after[i].h };
    hole[i] = size_hole_ratio(z.before[i].w, bottom - top);
  }
  return hole;
}

SCAV_INTERNAL_END

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
  // A hole is only knowable once its owner is sized, and sizing is bottom-up,
  // so the ratios come off a first pass at the profile's own ratio. That pass
  // packs the same way, or the holes would be a different packer's.
  SizedLayout &first{ size_scratch().first };
  if (!size_pass(c, g, o, s, p, {}, compaction, fold, first, diags)) { return false; }
  return size_pass(c,
                   g,
                   o,
                   s,
                   p,
                   size_owner_holes(c, first),
                   compaction,
                   fold,
                   out,
                   diags);
}

}  // namespace scav
