// One frame at a time: nodes from the frame's own children and from the ports
// on its enclosing border, edges from the segments routed there, ranks by
// longest path, bends for whatever spans more than one rank, then median
// sweeps against an inversion count.

#include "layout/order.h"

#include "layout/decompose.h"
#include "layout/memo.h"
#include "layout/partition.h"
#include "layout/shard.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"
#include "scav_shard.h"
#include "scav_stable_sort.h"
#include "scav_thread.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

namespace {

// Every frame this thread has ordered, by what ordering it read. A search's
// candidates differ in one frame, so every other frame a candidate orders is
// one an earlier candidate ordered from the same inputs.
Memo &frame_memo() {
  thread_local Memo m{ size_t{ 1 } << 20 };
  return m;
}

// Edge indices grouped by one endpoint. Built once per frame and rebuilt after
// chaining, because every traversal below would otherwise rescan every edge
// and turn a sweep quadratic.
struct Adjacency {
  std::vector<uint32_t> off, edge;
};

// `a.off[i]..a.off[i+1]` is node i's run of `a.edge`; `fill` is the caller's scratch.
void adjacency_of(Adjacency &a,
                  std::vector<OrderEdge> const &edges,
                  uint32_t nodes,
                  bool by_src,
                  std::vector<uint32_t> &fill) {
  vec_assign(a.off, size_t{ nodes } + 1, 0);
  for (OrderEdge const &e : edges) { ++a.off[size_t{ by_src ? e.src : e.dst } + 1]; }
  for (uint32_t i = 0; i < nodes; ++i) { a.off[i + 1] += a.off[i]; }
  vec_assign(a.edge, edges.size(), 0);
  vec_assign(fill, a.off.begin(), a.off.end() - 1);
  for (uint32_t i = 0; i < edges.size(); ++i) {
    a.edge[fill[by_src ? edges[i].src : edges[i].dst]++] = i;
  }
}

// One frame's graph in frame-local indices. Ranks and positions live beside
// the nodes until the frame is flushed into `SubmachineOrders`.
struct Frame {
  std::vector<OrderNode> nodes;
  std::vector<OrderEdge> edges;
  std::vector<std::vector<uint32_t>> ranks;  // rank -> node indices, in order
  Adjacency in, out;
};

// The port a boundary node stands for, recorded as the frame makes it: the
// node itself carries the segment, so only the port has to be carried out.
struct SegPort {
  uint32_t seg;
  uint32_t port;
};

// One submachine's answer, written by the shard that owns it and emitted in
// submachine order.
struct FrameOrder {
  Frame f;
  std::vector<int32_t> gaps, labels;
  std::vector<SegPort> seg_ports;
  std::vector<uint32_t> cyclic;  // -> segments, those on a cycle of this frame
};

// Allocated once per shard and reused across that shard's frames. Both maps
// hold frame-local node indices and are cleared back to INVALID per frame.
struct FrameScratch {
  std::vector<uint32_t> state_local;  // -> states
  std::vector<uint32_t> seg_local;    // -> SplitGraph::segments
  Partition part;                     // -> nodes, the frame's components
  std::vector<uint32_t> dense;        // -> nodes; a component root's ordinal
  std::vector<uint32_t> lanes;        // boundaries x components, row-major
  std::vector<uint32_t> spanning;     // -> edges, labelled across several boundaries
  std::vector<OrderEdge> flat;        // edges into a cross-border port, held out
  std::vector<uint8_t> fixed;         // -> edges; turned by a side pin
  std::vector<uint8_t> extreme;       // -> nodes; 1 first in its rank, 2 last

  // The helpers' working buffers; each sizes what it reads before reading it.
  struct Visit {
    uint32_t node;
    uint32_t next;  // -> walk.edge, the edge to try when this frame resumes
  };
  struct SouthPair {
    uint32_t north, south;
  };
  Adjacency walk;
  std::vector<uint32_t> fill, index, low, comp, held, pending, topo, renumber;
  std::vector<uint8_t> marks;
  std::vector<Visit> visits;
  std::vector<OrderEdge> chained;
  std::vector<SouthPair> pairs, merge;
  std::vector<uint32_t> south, tree, med, medians;
  std::vector<std::vector<uint32_t>> best;
};

// A shard's frame scratch. Per-thread; both maps return to all INVALID after every frame,
// and a shard never waits on the pool.
FrameScratch &frame_scratch(Chart const &c, SplitGraph const &g) {
  thread_local FrameScratch sc;
  if (sc.state_local.size() != c.states.size()) {
    vec_assign(sc.state_local, c.states.size(), INVALID);
  }
  if (sc.seg_local.size() != g.segments.size()) {
    vec_assign(sc.seg_local, g.segments.size(), INVALID);
  }
  return sc;
}

// What one call builds and discards, per thread and nesting depth: a call waiting on its
// shards may run another call's on the same thread.
struct CallScratch {
  std::vector<uint32_t> seg_count, seg_off, frame_segs, fill, global;
  std::vector<int32_t> seg_label;
  std::vector<uint8_t> cut, pre_reversed, sided;
  std::vector<FrameOrder> frames;
};

// One scratch per depth, each at a fixed address, deleted when the thread exits.
struct CallStack {
  std::vector<CallScratch *> at;
  CallStack() = default;
  CallStack(CallStack const &) = delete;
  CallStack(CallStack &&) = delete;
  CallStack &operator=(CallStack const &) = delete;
  CallStack &operator=(CallStack &&) = delete;
  ~CallStack() {
    for (CallScratch *const sc : at) { delete sc; }
  }
};

class CallScope {
 public:
  CallScope() {
    std::vector<CallScratch *> &s{ stack().at };
    if (depth() == s.size()) { vec_push_back(s, new CallScratch); }
    at = s[depth()];
    ++depth();
  }
  ~CallScope() { --depth(); }
  CallScope(CallScope const &) = delete;
  CallScope &operator=(CallScope const &) = delete;
  [[nodiscard]] CallScratch &scratch() const { return *at; }

 private:
  static CallStack &stack() {
    thread_local CallStack s;
    return s;
  }
  static size_t &depth() {
    thread_local size_t d{ 0 };
    return d;
  }
  CallScratch *at{ nullptr };
};

// The endpoint state of a segment's src or dst end when that end carries no
// port: the transition's own src or dst.
StateId endpoint_state(Chart const &c, SplitSegment const &seg, bool is_src) {
  Transition const &tr{ c.transitions[seg.trans.v] };
  return is_src ? tr.src : tr.dst;
}

// Inversions in `v` by a Fenwick tree over its own values, which are already
// positions and so already bounded by the rank size.
uint64_t inversions(std::vector<uint32_t> const &v, std::vector<uint32_t> &tree) {
  if (v.size() < 2) { return 0; }
  uint32_t hi{ 0 };
  for (uint32_t const x : v) { hi = imax(hi, x); }
  vec_assign(tree, static_cast<size_t>(hi) + 2, 0);
  uint64_t total{ 0 };
  uint64_t seen{ 0 };
  for (uint32_t const x : v) {
    // Everything inserted so far that is <= x does not cross x; the rest does.
    uint64_t le{ 0 };
    for (uint32_t i = x + 1; i > 0; i -= i & (~i + 1U)) { le += tree[i]; }
    total += seen - le;
    ++seen;
    for (uint32_t i = x + 1; i < tree.size(); i += i & (~i + 1U)) { ++tree[i]; }
  }
  return total;
}

// The segments on a cycle of the frame's graph as drawn -- an edge whose two
// ends share a strongly connected component. Tarjan's, iteratively, because a
// flat frame of a few thousand states is deep enough to overflow recursion.
void cyclic_segments(Frame const &f, std::vector<uint32_t> &segs, FrameScratch &sc) {
  uint32_t const n{ static_cast<uint32_t>(f.nodes.size()) };
  Adjacency &out{ sc.walk };
  adjacency_of(out, f.edges, n, true, sc.fill);
  std::vector<uint32_t> &index{ sc.index };
  vec_assign(index, n, INVALID);
  std::vector<uint32_t> &low{ sc.low };
  vec_assign(low, n, 0);
  std::vector<uint32_t> &comp{ sc.comp };
  vec_assign(comp, n, INVALID);
  std::vector<uint8_t> &on_stack{ sc.marks };
  vec_assign(on_stack, n, 0);
  std::vector<uint32_t> &held{ sc.held };
  held.clear();
  std::vector<FrameScratch::Visit> &call{ sc.visits };
  call.clear();
  uint32_t counter{ 0 };
  uint32_t comps{ 0 };
  for (uint32_t root = 0; root < n; ++root) {
    if (index[root] != INVALID) { continue; }
    index[root] = counter;
    low[root] = counter;
    ++counter;
    vec_push_back(held, root);
    on_stack[root] = 1;
    vec_push_back(call, { .node = root, .next = out.off[root] });
    while (!call.empty()) {
      uint32_t const v{ call.back().node };
      if (call.back().next < out.off[v + 1]) {
        uint32_t const w{ f.edges[out.edge[call.back().next++]].dst };
        if (index[w] == INVALID) {
          index[w] = counter;
          low[w] = counter;
          ++counter;
          vec_push_back(held, w);
          on_stack[w] = 1;
          vec_push_back(call, { .node = w, .next = out.off[w] });
        } else if (on_stack[w] != 0) {
          low[v] = imin(low[v], index[w]);
        }
        continue;
      }
      if (low[v] == index[v]) {
        uint32_t w{ INVALID };
        do {
          w = held.back();
          held.pop_back();
          on_stack[w] = 0;
          comp[w] = comps;
        } while (w != v);
        ++comps;
      }
      call.pop_back();
      if (!call.empty()) {
        uint32_t const u{ call.back().node };
        low[u] = imin(low[u], low[v]);
      }
    }
  }
  segs.clear();
  for (OrderEdge const &e : f.edges) {
    if ((e.src != e.dst) && (comp[e.src] == comp[e.dst])) {
      vec_push_back(segs, e.segment);
    }
  }
}

// Cycle breaking by iterative depth-first search in node order: an edge that
// closes back onto the current path is the one reversed, so the frame becomes
// a DAG without any node moving.
void orient_acyclic(Frame &f,
                    std::vector<uint8_t> const &pre,
                    std::vector<uint8_t> const &fixed,
                    FrameScratch &sc) {
  // Turned before the walk, which then finds those cycles already broken. An edge `fixed`
  // marks already points the way a side pin wants.
  for (uint32_t k = 0; k < f.edges.size(); ++k) {
    OrderEdge &e{ f.edges[k] };
    if ((e.segment >= pre.size()) || (pre[e.segment] == 0)) { continue; }
    if ((k < fixed.size()) && (fixed[k] != 0)) { continue; }
    uint32_t const swap{ e.src };
    e.src = e.dst;
    e.dst = swap;
    e.reversed = 1;
    trace_emit({ .kind = TraceKind::EdgeReversed, .seg = { .seg = e.segment } });
  }

  uint32_t const n{ static_cast<uint32_t>(f.nodes.size()) };
  Adjacency &out{ sc.walk };
  adjacency_of(out, f.edges, n, true, sc.fill);

  enum : uint8_t { White, Gray, Black };
  std::vector<uint8_t> &color{ sc.marks };
  vec_assign(color, n, White);
  std::vector<FrameScratch::Visit> &stack{ sc.visits };
  stack.clear();
  for (uint32_t root = 0; root < n; ++root) {
    if (color[root] != White) { continue; }
    color[root] = Gray;
    vec_push_back(stack, { .node = root, .next = out.off[root] });
    while (!stack.empty()) {
      uint32_t const node{ stack.back().node };
      if (stack.back().next == out.off[node + 1]) {
        color[node] = Black;
        stack.pop_back();
        continue;
      }
      OrderEdge &e{ f.edges[out.edge[stack.back().next++]] };
      if (color[e.dst] == Gray) {
        e.reversed ^= 1U;  // an edge a pin turned and the walk turns back runs as authored
        trace_emit({ .kind = TraceKind::EdgeReversed, .seg = { .seg = e.segment } });
        uint32_t const swap{ e.src };
        e.src = e.dst;
        e.dst = swap;
        continue;
      }
      if (color[e.dst] == White) {
        color[e.dst] = Gray;
        vec_push_back(stack, { .node = e.dst, .next = out.off[e.dst] });
      }
    }
  }
}

// Longest path from the sources, then one pull-right pass for nodes with more
// successors than predecessors, then empty ranks squeezed out. Boundary nodes
// are excluded from the pull because their rank is what puts them on the
// frame's left or right border, and a sink boundary is pinned to the last rank.
void assign_ranks(Frame &f, FrameScratch &sc) {
  uint32_t const n{ static_cast<uint32_t>(f.nodes.size()) };
  if (n == 0) { return; }
  adjacency_of(f.out, f.edges, n, true, sc.fill);
  adjacency_of(f.in, f.edges, n, false, sc.fill);
  auto const out_deg = [&](uint32_t v) { return f.out.off[v + 1] - f.out.off[v]; };
  auto const in_deg = [&](uint32_t v) { return f.in.off[v + 1] - f.in.off[v]; };

  std::vector<uint32_t> &pending{ sc.pending };
  vec_assign(pending, n, 0);
  for (uint32_t v = 0; v < n; ++v) { pending[v] = in_deg(v); }
  std::vector<uint32_t> &topo{ sc.topo };
  topo.clear();
  vec_reserve(topo, n);
  for (uint32_t v = 0; v < n; ++v) {
    if (pending[v] == 0) { vec_push_back(topo, v); }
  }
  for (uint32_t at = 0; at < topo.size(); ++at) {
    uint32_t const v{ topo[at] };
    for (uint32_t k = f.out.off[v]; k < f.out.off[v + 1]; ++k) {
      uint32_t const w{ f.edges[f.out.edge[k]].dst };
      if (--pending[w] == 0) { vec_push_back(topo, w); }
    }
  }

  for (uint32_t const v : topo) {
    for (uint32_t k = f.out.off[v]; k < f.out.off[v + 1]; ++k) {
      OrderNode &d{ f.nodes[f.edges[f.out.edge[k]].dst] };
      d.rank = imax(d.rank, f.nodes[v].rank + 1U);
    }
  }

  for (auto at = static_cast<uint32_t>(topo.size()); at-- > 0;) {
    uint32_t const v{ topo[at] };
    if ((f.nodes[v].kind == OrderKind::Boundary) || (out_deg(v) == 0) ||
        (in_deg(v) >= out_deg(v))) {
      continue;
    }
    uint32_t nearest{ INVALID };
    for (uint32_t k = f.out.off[v]; k < f.out.off[v + 1]; ++k) {
      nearest = imin(nearest, f.nodes[f.edges[f.out.edge[k]].dst].rank);
    }
    f.nodes[v].rank = nearest - 1;
  }

  uint32_t top{ 0 };
  for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
  for (uint32_t v = 0; v < n; ++v) {
    if ((f.nodes[v].kind == OrderKind::Boundary) && (out_deg(v) == 0)) {
      f.nodes[v].rank = top;
    }
  }

  std::vector<uint32_t> &used{ sc.held };
  vec_assign(used, size_t{ top } + 1, 0);
  for (OrderNode const &nd : f.nodes) { used[nd.rank] = 1; }
  std::vector<uint32_t> &renumber{ sc.renumber };
  vec_assign(renumber, used.size(), 0);
  uint32_t next{ 0 };
  for (uint32_t r = 0; r < used.size(); ++r) {
    renumber[r] = next;
    next += used[r];
  }
  for (OrderNode &nd : f.nodes) { nd.rank = renumber[nd.rank]; }

  for (OrderNode const &nd : f.nodes) {
    if (nd.kind != OrderKind::State) { continue; }
    trace_emit({ .kind = TraceKind::RankAssigned,
                 .rank = { .state = nd.subject, .rank = nd.rank } });
  }
}

// A bend per intervening rank, so every emitted edge spans exactly one. The
// chain keeps the original edge's segment and reversal, which is what lets
// phase 3 walk it back into one polyline. A segment `cut` names keeps its long
// edge instead, and reaches the router with no waypoints (11.10b).
void chain_long_edges(Frame &f, std::vector<uint8_t> const &cut, FrameScratch &sc) {
  std::vector<OrderEdge> &out{ sc.chained };
  out.clear();
  vec_reserve(out, f.edges.size());
  for (OrderEdge const &e : f.edges) {
    uint32_t const from{ f.nodes[e.src].rank };
    uint32_t const to{ f.nodes[e.dst].rank };
    if (((to - from) <= 1) || ((e.segment < cut.size()) && (cut[e.segment] != 0))) {
      vec_push_back(out, e);
      continue;
    }
    uint32_t prev{ e.src };
    for (uint32_t r = from + 1; r < to; ++r) {
      trace_emit({ .kind = TraceKind::EdgeChained,
                   .chain = { .seg = e.segment,
                              .rank = r,
                              .index = r - from,
                              .count = to - from - 1 } });
      uint32_t const bend{ static_cast<uint32_t>(f.nodes.size()) };
      vec_push_back(
          f.nodes,
          { .kind = OrderKind::Bend, .subject = e.segment, .rank = r, .pos = 0 });
      vec_push_back(
          out,
          { .src = prev, .dst = bend, .segment = e.segment, .reversed = e.reversed });
      prev = bend;
    }
    vec_push_back(
        out,
        { .src = prev, .dst = e.dst, .segment = e.segment, .reversed = e.reversed });
  }
  f.edges.swap(out);
  adjacency_of(f.out, f.edges, static_cast<uint32_t>(f.nodes.size()), true, sc.fill);
  adjacency_of(f.in, f.edges, static_cast<uint32_t>(f.nodes.size()), false, sc.fill);
}

// Rank buckets in node order; an `extreme` node goes first (1) or last (2) and no sweep
// moves it.
void bucket_ranks(Frame &f, std::vector<uint8_t> const &extreme) {
  if (f.nodes.empty()) {
    f.ranks.clear();
    return;
  }
  uint32_t top{ 0 };
  for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
  vec_assign(f.ranks, static_cast<size_t>(top) + 1, {});
  for (uint8_t const want : { uint8_t{ 1 }, uint8_t{ 0 }, uint8_t{ 2 } }) {
    for (uint32_t v = 0; v < f.nodes.size(); ++v) {
      uint8_t const at{ (v < extreme.size()) ? extreme[v] : uint8_t{ 0 } };
      if (at == want) { vec_push_back(f.ranks[f.nodes[v].rank], v); }
    }
  }
  for (std::vector<uint32_t> const &bucket : f.ranks) {
    for (uint32_t i = 0; i < bucket.size(); ++i) { f.nodes[bucket[i]].pos = i; }
  }
}

// The edges leaving rank r, as south positions ordered by their north one,
// which is the form inversion counting wants.
std::vector<uint32_t> const &south_of(Frame const &f,
                                      uint32_t north_rank,
                                      FrameScratch &sc) {
  using Pair = FrameScratch::SouthPair;
  std::vector<Pair> &pairs{ sc.pairs };
  pairs.clear();
  for (uint32_t const v : f.ranks[north_rank]) {
    for (uint32_t k = f.out.off[v]; k < f.out.off[v + 1]; ++k) {
      OrderEdge const &e{ f.edges[f.out.edge[k]] };
      vec_push_back(pairs, { .north = f.nodes[e.src].pos, .south = f.nodes[e.dst].pos });
    }
  }
  // The south tiebreak is what keeps two edges leaving one node from scoring
  // as a crossing: they share an endpoint, so they cannot cross, and without
  // it their south values arrive in edge order and can read as an inversion.
  // The mirrored case needs nothing -- `inversions` compares strictly, so two
  // edges sharing a south node already score zero.
  scav_stable_sort(pairs, sc.merge, [](Pair const &a, Pair const &b) {
    return (a.north != b.north) ? (a.north < b.north) : (a.south < b.south);
  });
  std::vector<uint32_t> &south{ sc.south };
  south.clear();
  vec_reserve(south, pairs.size());
  for (Pair const &pr : pairs) { vec_push_back(south, pr.south); }
  return south;
}

uint64_t total_crossings(Frame const &f, FrameScratch &sc) {
  uint64_t total{ 0 };
  for (uint32_t r = 0; (r + 1) < f.ranks.size(); ++r) {
    total += inversions(south_of(f, r, sc), sc.tree);
  }
  return total;
}

// Median of the adjacent positions in the fixed rank; INVALID when the node
// has no neighbour there, which pins it where it is. `scratch` is the
// caller's, because this runs once per node per rank per sweep.
uint32_t median_of(Frame const &f,
                   uint32_t node,
                   bool from_predecessors,
                   std::vector<uint32_t> &scratch) {
  Adjacency const &a{ from_predecessors ? f.in : f.out };
  scratch.clear();
  for (uint32_t k = a.off[node]; k < a.off[node + 1]; ++k) {
    OrderEdge const &e{ f.edges[a.edge[k]] };
    vec_push_back(scratch, f.nodes[from_predecessors ? e.src : e.dst].pos);
  }
  if (scratch.empty()) { return INVALID; }
  scav_insertion_sort(scratch.data(),
                      scratch.data() + scratch.size(),
                      [](uint32_t a2, uint32_t b2) { return a2 < b2; });
  return scratch[scratch.size() / 2];
}

// Adjacent exchanges only, and only between two nodes that both have a
// median: a node without one is a barrier that keeps its slot, which is what
// stops an unconstrained node from being swept to an end.
void reorder_rank(Frame &f, uint32_t rank, bool from_predecessors, FrameScratch &sc) {
  std::vector<uint32_t> &bucket{ f.ranks[rank] };
  std::vector<uint32_t> &med{ sc.med };
  vec_assign(med, bucket.size(), INVALID);
  std::vector<uint32_t> &scratch{ sc.medians };
  for (uint32_t i = 0; i < bucket.size(); ++i) {
    med[i] = median_of(f, bucket[i], from_predecessors, scratch);
  }
  for (uint32_t pass = 0; pass < bucket.size(); ++pass) {
    bool moved{ false };
    for (uint32_t i = 0; (i + 1) < bucket.size(); ++i) {
      if ((med[i] == INVALID) || (med[i + 1] == INVALID) || (med[i] <= med[i + 1])) {
        continue;
      }
      uint32_t const node{ bucket[i] };
      bucket[i] = bucket[i + 1];
      bucket[i + 1] = node;
      uint32_t const key{ med[i] };
      med[i] = med[i + 1];
      med[i + 1] = key;
      moved = true;
    }
    if (!moved) { break; }
  }
  for (uint32_t i = 0; i < bucket.size(); ++i) { f.nodes[bucket[i]].pos = i; }
}

// Alternating sweeps against the running best, so a sweep that makes things
// worse is discarded rather than carried forward.
void minimize_crossings(Frame &f, uint32_t sweeps, FrameScratch &sc) {
  if (f.ranks.size() < 2) { return; }
  std::vector<std::vector<uint32_t>> &best{ sc.best };
  auto const copy_ranks = [](std::vector<std::vector<uint32_t>> &to,
                             std::vector<std::vector<uint32_t>> const &from) {
    vec_resize(to, from.size());
    for (size_t r = 0; r < from.size(); ++r) {
      vec_assign(to[r], from[r].begin(), from[r].end());
    }
  };
  copy_ranks(best, f.ranks);
  uint64_t best_cost{ total_crossings(f, sc) };
  for (uint32_t sweep = 0; (sweep < sweeps) && (best_cost > 0); ++sweep) {
    if ((sweep % 2) == 0) {
      for (uint32_t r = 1; r < f.ranks.size(); ++r) { reorder_rank(f, r, true, sc); }
    } else {
      for (uint32_t r = static_cast<uint32_t>(f.ranks.size()) - 1; r-- > 0;) {
        reorder_rank(f, r, false, sc);
      }
    }
    uint64_t const cost{ total_crossings(f, sc) };
    if (cost < best_cost) {
      best_cost = cost;
      copy_ranks(best, f.ranks);
    }
  }
  copy_ranks(f.ranks, best);
  for (std::vector<uint32_t> const &bucket : f.ranks) {
    for (uint32_t i = 0; i < bucket.size(); ++i) { f.nodes[bucket[i]].pos = i; }
  }
}

}  // namespace

uint64_t rank_crossings(std::vector<uint32_t> const &south_positions) {
  std::vector<uint32_t> tree;
  return inversions(south_positions, tree);
}

namespace {

// Every initial pseudostate one rank before the nearest state it enters, which
// `assign_ranks` gives it and a pin on that state can take away. Where the
// state is at rank 0, everything else moves up one to make the room.
void seat_initials(Chart const &c, Frame &f) {
  for (uint32_t v = 0; v < f.nodes.size(); ++v) {
    OrderNode const &nd{ f.nodes[v] };
    if ((nd.kind != OrderKind::State) ||
        (c.states[nd.subject].kind != StateKind::Initial)) {
      continue;
    }
    uint32_t nearest{ INVALID };
    for (OrderEdge const &e : f.edges) {
      if ((e.src == v) && (e.dst != v)) { nearest = imin(nearest, f.nodes[e.dst].rank); }
      if ((e.dst == v) && (e.src != v)) { nearest = imin(nearest, f.nodes[e.src].rank); }
    }
    if (nearest == INVALID) { continue; }
    if (nearest == 0) {
      for (uint32_t u = 0; u < f.nodes.size(); ++u) {
        if (u != v) { ++f.nodes[u].rank; }
      }
      nearest = 1;
    }
    f.nodes[v].rank = nearest - 1;
  }
}

// Ranks renumbered onto the ones that still hold a node, keeping their order. A
// move can empty the rank it left, and phase 2 sizes a rank gap whether or not
// anything is in it (11.10).
void squeeze_ranks(Frame &f, FrameScratch &sc) {
  uint32_t top{ 0 };
  for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
  std::vector<uint32_t> &onto{ sc.renumber };
  vec_assign(onto, static_cast<size_t>(top) + 1, INVALID);
  for (OrderNode const &nd : f.nodes) { onto[nd.rank] = 0; }
  uint32_t next{ 0 };
  for (uint32_t r = 0; r <= top; ++r) {
    if (onto[r] == 0) { onto[r] = next++; }
  }
  for (OrderNode &nd : f.nodes) { nd.rank = onto[nd.rank]; }
}

// Everything a frame's ordering derives from the ranks it was assigned: the
// boundary charges, the chaining of multi-rank edges into single-rank ones, the
// rank buckets and the crossing sweeps. **Pure in `f.nodes[].rank` and
// `f.edges`**, which is what lets a bounded move re-derive a whole frame by
// changing one node's rank and calling this again (11.10a). Not idempotent:
// `chain_long_edges` adds nodes, so a second call wants the frame as it was
// before the first.
void rank_derived(Frame &f,
                  std::vector<int32_t> &gaps,
                  std::vector<int32_t> &labels,
                  std::vector<int32_t> const &seg_label,
                  std::vector<uint8_t> const &cut,
                  std::vector<uint8_t> const &extreme,
                  scav_profile const &p,
                  FrameScratch &sc) {
  // Charged before chaining, while each edge still spans its whole rank range. An edge
  // across one boundary is charged there first.
  uint32_t top{ 0 };
  for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
  vec_assign(gaps, top, 0);
  vec_assign(labels, top, 0);
  std::vector<uint32_t> &spanning{ sc.spanning };
  spanning.clear();
  for (uint32_t i = 0; i < f.edges.size(); ++i) {
    OrderEdge const &e{ f.edges[i] };
    int32_t const label{ seg_label[e.segment] };
    if ((label == 0) || (f.nodes[e.src].rank == f.nodes[e.dst].rank)) { continue; }
    uint32_t const from{ imin(f.nodes[e.src].rank, f.nodes[e.dst].rank) };
    uint32_t const to{ imax(f.nodes[e.src].rank, f.nodes[e.dst].rank) };
    if ((to - from) > 1) {
      vec_push_back(spanning, i);
      continue;
    }
    gaps[from] = imax(gaps[from], label);
    labels[from] = imax(labels[from], label);
    trace_emit({ .kind = TraceKind::GapCharged,
                 .pass = static_cast<uint16_t>(GapCause::Label),
                 .gap = { .boundary = from, .seg = e.segment, .width = label } });
  }

  // Lanes per component: an edge can turn in its first and last boundary, and two or more
  // lanes in one boundary sit a line of type apart. This is the most the lanes can need.
  Partition &part{ sc.part };
  part.reset(f.nodes.size());
  for (OrderEdge const &e : f.edges) { part.join(e.src, e.dst); }
  // Dense, so the table is boundaries x components, not x nodes.
  std::vector<uint32_t> &dense{ sc.dense };
  vec_assign(dense, f.nodes.size(), INVALID);
  uint32_t parts{ 0 };
  for (uint32_t i = 0; i < f.nodes.size(); ++i) {
    uint32_t const root{ part.root(i) };
    if (dense[root] == INVALID) { dense[root] = parts++; }
  }
  std::vector<uint32_t> &lanes{ sc.lanes };
  vec_assign(lanes, gaps.size() * parts, 0);
  int32_t const pitch{ label_line_height(p) };
  auto const turn = [&](uint32_t b, uint32_t of) {
    if (b >= gaps.size()) { return; }
    uint32_t &here{ lanes[(static_cast<size_t>(b) * parts) + of] };
    ++here;
    if (here < 2) { return; }
    gaps[b] =
        imax(gaps[b], static_cast<int32_t>(imin(Wide{ here } * pitch, Wide{ SPACE_MAX })));
  };
  for (OrderEdge const &e : f.edges) {
    uint32_t const from{ imin(f.nodes[e.src].rank, f.nodes[e.dst].rank) };
    uint32_t const to{ imax(f.nodes[e.src].rank, f.nodes[e.dst].rank) };
    if (from == to) { continue; }  // turns in no boundary, as above
    uint32_t const of{ dense[part.root(e.src)] };
    turn(from, of);
    if (to > (from + 1)) { turn(to - 1, of); }
  }

  // A spanning edge's label takes the widest boundary it crosses, nearest the middle among
  // equals, widest label first; where that boundary already holds it, it costs nothing.
  scav_stable_sort(spanning, [&](uint32_t a, uint32_t b) {
    return seg_label[f.edges[a].segment] > seg_label[f.edges[b].segment];
  });
  for (uint32_t const i : spanning) {
    OrderEdge const &e{ f.edges[i] };
    int32_t const label{ seg_label[e.segment] };
    uint32_t const from{ imin(f.nodes[e.src].rank, f.nodes[e.dst].rank) };
    uint32_t const to{ imax(f.nodes[e.src].rank, f.nodes[e.dst].rank) };
    uint32_t const middle{ from + ((to - from) / 2) };
    uint32_t at{ middle };
    for (uint32_t b = from; b < to; ++b) {
      uint32_t const off{ (b > middle) ? (b - middle) : (middle - b) };
      uint32_t const best{ (at > middle) ? (at - middle) : (middle - at) };
      if ((gaps[b] > gaps[at]) || ((gaps[b] == gaps[at]) && (off < best))) { at = b; }
    }
    bool const held{ gaps[at] >= label };
    gaps[at] = imax(gaps[at], label);
    labels[at] = imax(labels[at], label);
    trace_emit({ .kind = TraceKind::GapCharged,
                 .pass = static_cast<uint16_t>(held ? GapCause::Held : GapCause::Label),
                 .gap = { .boundary = at, .seg = e.segment, .width = label } });
  }

  chain_long_edges(f, cut, sc);
  bucket_ranks(f, extreme);
  minimize_crossings(f, static_cast<uint32_t>(p.sweep_count), sc);
}

}  // namespace

uint32_t label_segment(Chart const &c, SplitGraph const &g, uint32_t t) {
  if (t < g.trans_label.size()) { return g.trans_label[t]; }
  if ((t >= g.trans_segments.size()) || (t >= c.transitions.size())) { return INVALID; }
  Span const segs{ g.trans_segments[t] };
  if ((segs.len == 0) || inner_loop(c, t)) {
    return INVALID;
  }  // labelled in its loop room
  SubmachineId const frame{ g.trans_common[t].frame };
  for (uint32_t k = 0; (frame.v != INVALID) && (k < segs.len); ++k) {
    if (g.segments[segs.off + k].frame == frame) { return segs.off + k; }
  }
  return segs.off + (segs.len / 2);
}

SubmachineOrders order_submachines(Chart const &c,
                                   SplitGraph const &g,
                                   scav_spaces const &s,
                                   scav_profile const &p,
                                   uint32_t threads,
                                   SearchPins const &pins) {
  SubmachineOrders o;
  order_submachines(o, c, g, s, p, threads, pins);
  return o;
}

void order_submachines(SubmachineOrders &o,
                       Chart const &c,
                       SplitGraph const &g,
                       scav_spaces const &s,
                       scav_profile const &p,
                       uint32_t threads,
                       SearchPins const &pins) {
  o.nodes.clear();
  o.edges.clear();
  o.gaps.clear();
  o.labels.clear();
  vec_assign(o.sub_nodes, c.submachines.size(), Span{});
  vec_assign(o.sub_edges, c.submachines.size(), Span{});
  vec_assign(o.sub_ranks, c.submachines.size(), 0);
  vec_assign(o.sub_down, c.submachines.size(), 0);
  for (OrientPin const &pin : pins.orients) {
    if (pin.frame.v < o.sub_down.size()) { o.sub_down[pin.frame.v] = 1; }
  }
  vec_assign(o.sub_fold, c.submachines.size(), 0);
  vec_assign(o.sub_fold_cut, c.submachines.size(), 0);
  for (FoldPin const &pin : pins.folds) {
    if ((pin.frame.v < o.sub_fold.size()) && (pin.mode <= FOLD_NEVER)) {
      o.sub_fold[pin.frame.v] = static_cast<uint8_t>(pin.mode + 1);
      o.sub_fold_cut[pin.frame.v] = pin.layer;
    }
  }
  vec_assign(o.sub_gaps, c.submachines.size(), Span{});
  vec_assign(o.state_node, c.states.size(), INVALID);
  vec_assign(o.seg_node, g.segments.size(), INVALID);
  vec_assign(o.seg_port, g.segments.size(), INVALID);
  vec_assign(o.seg_cross, g.segments.size(), 0);
  vec_assign(o.seg_sided, g.segments.size(), 0);
  vec_assign(o.seg_cyclic, g.segments.size(), 0);

  CallScope const scope;
  CallScratch &cs{ scope.scratch() };

  // Which segments each frame routes, gathered once: a segment names its frame
  // but a frame does not name its segments.
  std::vector<uint32_t> &seg_count{ cs.seg_count };
  vec_assign(seg_count, c.submachines.size(), 0);
  for (SplitSegment const &seg : g.segments) {
    if (seg.frame.v != INVALID) { ++seg_count[seg.frame.v]; }
  }
  std::vector<uint32_t> &seg_off{ cs.seg_off };
  vec_assign(seg_off, seg_count.size() + 1, 0);
  for (uint32_t i = 0; i < seg_count.size(); ++i) {
    seg_off[i + 1] = seg_off[i] + seg_count[i];
  }
  std::vector<uint32_t> &frame_segs{ cs.frame_segs };
  vec_assign(frame_segs, g.segments.size(), 0);
  vec_assign(cs.fill, seg_off.begin(), seg_off.end() - 1);
  for (uint32_t i = 0; i < g.segments.size(); ++i) {
    SubmachineId const frame{ g.segments[i].frame };
    if (frame.v != INVALID) { frame_segs[cs.fill[frame.v]++] = i; }
  }

  // A label is charged to one rank boundary in its `label_segment`'s frame, by its extent
  // along that frame's ranks: its width across the page, its height down it.
  std::vector<int32_t> &seg_label{ cs.seg_label };
  vec_assign(seg_label, g.segments.size(), 0);
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    scav_path_box const &box{ s.path_box[i] };
    uint32_t const at{ label_segment(c, g, box.subject) };
    if (at == INVALID) { continue; }
    uint32_t const frame{ g.segments[at].frame.v };
    bool const down{ (frame < o.sub_down.size()) && (o.sub_down[frame] != 0) };
    seg_label[at] += down ? box.h : box.w;
  }

  // `{trans, leg}` resolved to segment ordinals once, as a flat table.
  auto const resolve_pins = [&g](auto const &rows, std::vector<uint8_t> &table) {
    table.clear();
    if (rows.empty()) { return; }
    vec_assign(table, g.segments.size(), 0);
    for (auto const &row : rows) {
      if ((row.trans.v == INVALID) || (row.trans.v >= g.trans_segments.size())) {
        continue;
      }
      Span const segs{ g.trans_segments[row.trans.v] };
      if (row.leg >= segs.len) { continue; }  // a leg this transition does not have
      table[segs.off + row.leg] = 1;
    }
  };
  std::vector<uint8_t> &cut{ cs.cut };
  std::vector<uint8_t> &pre_reversed{ cs.pre_reversed };
  resolve_pins(pins.cuts, cut);
  resolve_pins(pins.reverses, pre_reversed);
  // A side pin resolved to the leg inside the border its port is on, whose
  // boundary node stands for it: one plus the side, 0 for none.
  std::vector<uint8_t> &sided{ cs.sided };
  sided.clear();
  for (SidePin const &pin : pins.sides) {
    if ((pin.trans.v == INVALID) || (pin.trans.v >= g.trans_segments.size()) ||
        (pin.end > 1) || (pin.side > 3)) {
      continue;
    }
    Span const segs{ g.trans_segments[pin.trans.v] };
    if (pin.leg >= segs.len) { continue; }
    uint32_t const at{ segs.off + pin.leg };
    uint32_t const port{ (pin.end == 0) ? g.segments[at].src_port
                                        : g.segments[at].dst_port };
    if ((port >= g.ports.size()) || (g.ports[port].state.v == INVALID)) { continue; }
    // On a child's border the port is the next leg's, inside that child.
    bool const own{ c.states[g.ports[port].state.v].parent != g.segments[at].frame };
    if (!own && (((pin.end == 0) && (pin.leg == 0)) ||
                 ((pin.end == 1) && ((pin.leg + 1) == segs.len)))) {
      continue;
    }
    uint32_t seg{ at };
    if (!own) { seg = (pin.end == 0) ? (at - 1) : (at + 1); }
    if (sided.empty()) { vec_assign(sided, g.segments.size(), 0); }
    sided[seg] = static_cast<uint8_t>(pin.side + 1);
  }

  // A pinned side against the frame: 1 or 2 for the leading or trailing cross border, and
  // likewise for a rank border in `lead_side`.
  auto const cross_side = [&](uint32_t seg, uint32_t m) -> uint8_t {
    if (sided.empty() || (sided[seg] == 0)) { return 0; }
    uint32_t const first{ (o.sub_down[m] != 0) ? 0U : 2U };
    uint32_t const side{ sided[seg] - 1U };
    if (side == first) { return 1; }
    return (side == (first + 1)) ? 2 : 0;
  };
  auto const lead_side = [&](uint32_t seg, uint32_t m) -> uint8_t {
    if (sided.empty() || (sided[seg] == 0)) { return 0; }
    uint32_t const first{ (o.sub_down[m] != 0) ? 2U : 0U };
    uint32_t const side{ sided[seg] - 1U };
    if (side == first) { return 1; }
    return (side == (first + 1)) ? 2 : 0;
  };

  std::vector<FrameOrder> &frames{ cs.frames };
  vec_resize(frames, c.submachines.size());
  uint32_t const profile_word{ memo_profile(p) };

  // Reads the model, the split and the label charges; writes `frames[m]` and
  // the caller's own scratch, so two frames share nothing.
  auto const order_frame = [&](uint32_t m, FrameScratch &sc) {
    TraceFrame const traced{ SubmachineId{ m } };
    Frame &f{ frames[m].f };
    std::vector<SegPort> &seg_ports{ frames[m].seg_ports };
    f.nodes.clear();
    f.edges.clear();
    seg_ports.clear();

    Span const kids{ c.submachines[m].children };
    for (uint32_t k = 0; k < kids.len; ++k) {
      uint32_t const child{ c.state_ids[kids.off + k].v };
      if (c.states[child].live == 0) { continue; }
      sc.state_local[child] = static_cast<uint32_t>(f.nodes.size());
      vec_push_back(f.nodes,
                    { .kind = OrderKind::State, .subject = child, .rank = 0, .pos = 0 });
    }

    // A port on a child's border is that child; one on the frame's own border is a node,
    // at most one per segment.
    auto const boundary_node = [&](uint32_t seg, uint32_t port) {
      if (sc.seg_local[seg] == INVALID) {
        sc.seg_local[seg] = static_cast<uint32_t>(f.nodes.size());
        vec_push_back(seg_ports, { .seg = seg, .port = port });
        vec_push_back(
            f.nodes,
            { .kind = OrderKind::Boundary, .subject = seg, .rank = 0, .pos = 0 });
      }
      return sc.seg_local[seg];
    };
    auto const resolve = [&](uint32_t seg, bool is_src) -> uint32_t {
      SplitSegment const &sg{ g.segments[seg] };
      uint32_t const port{ is_src ? sg.src_port : sg.dst_port };
      if (port == INVALID) {
        StateId const st{ endpoint_state(c, sg, is_src) };
        if ((st.v == INVALID) || (c.states[st.v].live == 0)) { return INVALID; }
        // The endpoint encloses this frame rather than sitting in it: no
        // border is crossed, so the boundary node carries no port.
        if ((is_src ? sg.src_inner : sg.dst_inner) != 0) {
          return boundary_node(seg, INVALID);
        }
        return sc.state_local[st.v];
      }
      SplitPort const &pt{ g.ports[port] };
      if (pt.state.v != INVALID) {
        return (c.states[pt.state.v].parent.v == m) ? sc.state_local[pt.state.v]
                                                    : boundary_node(seg, port);
      }
      if (pt.sub.v == m) { return boundary_node(seg, port); }
      StateId const owner{ c.submachines[pt.sub.v].owner };
      if ((owner.v != INVALID) && (c.states[owner.v].parent.v == m)) {
        return sc.state_local[owner.v];
      }
      return INVALID;
    };

    for (uint32_t k = seg_off[m]; k < seg_off[m + 1]; ++k) {
      uint32_t const seg{ frame_segs[k] };
      uint32_t const src{ resolve(seg, true) };
      uint32_t const dst{ resolve(seg, false) };
      if ((src == INVALID) || (dst == INVALID) || (src == dst)) { continue; }
      vec_push_back(f.edges, { .src = src, .dst = dst, .segment = seg, .reversed = 0 });
    }
    // Whether a side pin holds an edge's port: only a boundary node standing for
    // a port on a state's border takes one.
    auto const pinned = [&](OrderEdge const &e) {
      if (sided.empty() || (sided[e.segment] == 0)) { return false; }
      for (SegPort const &sp : seg_ports) {
        if (sp.seg == e.segment) {
          return (sp.port != INVALID) && (g.ports[sp.port].state.v != INVALID);
        }
      }
      return false;
    };
    auto const cross_of = [&](OrderEdge const &e) -> uint8_t {
      return pinned(e) ? cross_side(e.segment, m) : uint8_t{ 0 };
    };
    auto const lead_of = [&](OrderEdge const &e) -> uint8_t {
      return pinned(e) ? lead_side(e.segment, m) : uint8_t{ 0 };
    };

    // Everything below reads the frame as built, the reversal and cut tables
    // at its segments, the rank pins that land in it, which of its states are
    // initials, its label charges and the profile -- and those are the key, so
    // a hit is the ordering the steps below would derive. A traced run derives
    // every frame, since a hit would emit nothing.
    bool const tracing{ trace_sink() != nullptr };
    thread_local std::vector<uint32_t> key;
    thread_local std::vector<int32_t> value;
    key.clear();
    if (!tracing) {
      vec_push_back(key, g.serial);
      vec_push_back(key, profile_word);
      if (g.serial != 0) {
        vec_push_back(key, m);  // the frame as built is a function of the graph and `m`
      } else {
        vec_push_back(key, static_cast<uint32_t>(f.nodes.size()));
        for (OrderNode const &nd : f.nodes) {
          vec_push_back(key, static_cast<uint32_t>(nd.kind));
          vec_push_back(key, nd.subject);
          vec_push_back(key,
                        (nd.kind == OrderKind::State)
                            ? static_cast<uint32_t>(c.states[nd.subject].kind)
                            : 0U);
        }
        vec_push_back(key, static_cast<uint32_t>(f.edges.size()));
      }
      for (OrderEdge const &e : f.edges) {
        if (g.serial == 0) {
          vec_push_back(key, e.src);
          vec_push_back(key, e.dst);
          vec_push_back(key, e.segment);
        }
        uint32_t const reversed{ pre_reversed.empty() ? 0U : pre_reversed[e.segment] };
        uint32_t const cuts{ cut.empty() ? 0U : cut[e.segment] };
        // Each a byte, so the packed word is exact.
        vec_push_back(key,
                      reversed | (cuts << 8U) | (uint32_t{ cross_of(e) } << 16U) |
                          (uint32_t{ lead_of(e) } << 24U));
        vec_push_back(key, static_cast<uint32_t>(seg_label[e.segment]));
      }
      for (RankPin const &pin : pins.ranks) {
        if ((pin.state.v == INVALID) || (pin.state.v >= sc.state_local.size())) {
          continue;
        }
        uint32_t const at{ sc.state_local[pin.state.v] };
        if (at >= f.nodes.size()) { continue; }
        vec_push_back(key, at);
        vec_push_back(key, pin.rank);
      }
    }
    Memo &memo{ frame_memo() };
    int32_t const *hit{ nullptr };
    uint32_t len{ 0 };
    if (!tracing && memo.find(key, hit, len)) {
      auto word = [&, at = uint32_t{ 0 }]() mutable {
        return static_cast<uint32_t>(hit[at++]);
      };
      vec_resize(f.nodes, word());
      for (OrderNode &nd : f.nodes) {
        nd = { .kind = static_cast<OrderKind>(word()),
               .subject = word(),
               .rank = word(),
               .pos = word() };
      }
      vec_resize(f.edges, word());
      for (OrderEdge &e : f.edges) {
        e = { .src = word(), .dst = word(), .segment = word(), .reversed = word() };
      }
      vec_resize(f.ranks, word());
      for (std::vector<uint32_t> &bucket : f.ranks) {
        vec_resize(bucket, word());
        for (uint32_t &v : bucket) { v = word(); }
      }
      vec_resize(frames[m].gaps, word());
      for (int32_t &gap : frames[m].gaps) { gap = static_cast<int32_t>(word()); }
      vec_resize(frames[m].labels, frames[m].gaps.size());
      for (int32_t &label : frames[m].labels) { label = static_cast<int32_t>(word()); }
      vec_resize(frames[m].cyclic, word());
      for (uint32_t &seg : frames[m].cyclic) { seg = word(); }
    } else {
      cyclic_segments(f, frames[m].cyclic, sc);
      // An edge into a cross-border port is held out of ranking and takes its mate's rank;
      // one into a port pinned to a rank border is turned and `fixed`.
      std::vector<OrderEdge> &flat{ sc.flat };
      flat.clear();
      std::vector<uint8_t> &fixed{ sc.fixed };
      fixed.clear();
      if (!sided.empty()) {
        uint32_t kept{ 0 };
        for (OrderEdge const &e : f.edges) {
          if (cross_of(e) != 0) {
            vec_push_back(flat, e);
          } else {
            f.edges[kept++] = e;
          }
        }
        vec_resize(f.edges, kept);
        vec_assign(fixed, f.edges.size(), 0);
        for (uint32_t k = 0; k < f.edges.size(); ++k) {
          OrderEdge &e{ f.edges[k] };
          uint8_t const lead{ lead_of(e) };
          if (lead == 0) { continue; }
          fixed[k] = 1;
          if ((f.nodes[e.src].kind == OrderKind::Boundary) != (lead == 1)) {
            uint32_t const swap{ e.src };
            e.src = e.dst;
            e.dst = swap;
            e.reversed = 1;
          }
        }
      }
      orient_acyclic(f, pre_reversed, fixed, sc);
      assign_ranks(f, sc);

      // Pins apply between ranking and everything derived from it, naming nodes by their
      // merged index from `first`.
      if (!pins.ranks.empty()) {
        bool moved{ false };
        for (RankPin const &pin : pins.ranks) {
          if ((pin.state.v == INVALID) || (pin.state.v >= sc.state_local.size())) {
            continue;
          }
          uint32_t const at{ sc.state_local[pin.state.v] };
          if (at >= f.nodes.size()) { continue; }  // not a state this frame holds
          if (c.states[pin.state.v].kind == StateKind::Initial) { continue; }
          f.nodes[at].rank = pin.rank;
          moved = true;
          trace_emit({ .kind = TraceKind::RankPinned,
                       .rank = { .state = pin.state.v, .rank = pin.rank } });
        }
        if (moved) {
          seat_initials(c, f);
          // Ranks renumbered onto those that still hold a node.
          squeeze_ranks(f, sc);
        }
      }
      std::vector<uint8_t> &extreme{ sc.extreme };
      extreme.clear();
      if (!flat.empty()) {
        vec_assign(extreme, f.nodes.size(), 0);
        for (OrderEdge const &e : flat) {
          bool const at_src{ f.nodes[e.src].kind == OrderKind::Boundary };
          uint32_t const port{ at_src ? e.src : e.dst };
          f.nodes[port].rank = f.nodes[at_src ? e.dst : e.src].rank;
          extreme[port] = cross_of(e);
        }
        squeeze_ranks(f, sc);
      }
      rank_derived(f, frames[m].gaps, frames[m].labels, seg_label, cut, extreme, p, sc);
      vec_insert(f.edges, f.edges.end(), flat.begin(), flat.end());
      if (!tracing) {
        value.clear();
        auto const put = [](uint32_t w) { vec_push_back(value, static_cast<int32_t>(w)); };
        put(static_cast<uint32_t>(f.nodes.size()));
        for (OrderNode const &nd : f.nodes) {
          put(static_cast<uint32_t>(nd.kind));
          put(nd.subject);
          put(nd.rank);
          put(nd.pos);
        }
        put(static_cast<uint32_t>(f.edges.size()));
        for (OrderEdge const &e : f.edges) {
          put(e.src);
          put(e.dst);
          put(e.segment);
          put(e.reversed);
        }
        put(static_cast<uint32_t>(f.ranks.size()));
        for (std::vector<uint32_t> const &bucket : f.ranks) {
          put(static_cast<uint32_t>(bucket.size()));
          for (uint32_t const v : bucket) { put(v); }
        }
        put(static_cast<uint32_t>(frames[m].gaps.size()));
        for (int32_t const gap : frames[m].gaps) { vec_push_back(value, gap); }
        for (int32_t const label : frames[m].labels) { vec_push_back(value, label); }
        put(static_cast<uint32_t>(frames[m].cyclic.size()));
        for (uint32_t const seg : frames[m].cyclic) { put(seg); }
        memo.insert(key, value);
      }
    }

    for (uint32_t k = 0; k < kids.len; ++k) {
      sc.state_local[c.state_ids[kids.off + k].v] = INVALID;
    }
    for (SegPort const &sp : seg_ports) { sc.seg_local[sp.seg] = INVALID; }
  };

  uint32_t const shards{ layout_shard_count(c) };
  auto body = [&](uint32_t shard) {
    scav_span const mine{
      shard_range(shard, shards, static_cast<uint32_t>(c.submachines.size()))
    };
    if (mine.len == 0) { return; }
    FrameScratch &sc{ frame_scratch(c, g) };
    for (uint32_t k = 0; k < mine.len; ++k) {
      uint32_t const m{ mine.off + k };
      if (c.submachines[m].live != 0) { order_frame(m, sc); }
    }
  };
  parallel_for(shards, threads, body);

  size_t node_rows{ 0 };
  size_t edge_rows{ 0 };
  size_t gap_rows{ 0 };
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    node_rows += frames[m].f.nodes.size();
    edge_rows += frames[m].f.edges.size();
    gap_rows += frames[m].gaps.size();
  }
  vec_reserve(o.nodes, node_rows);
  vec_reserve(o.edges, edge_rows);
  vec_reserve(o.gaps, gap_rows);
  vec_reserve(o.labels, gap_rows);

  // Emitted in submachine order, so the global numbering and every span into it
  // are what one worker would have produced (6).
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    uint32_t const node_base{ static_cast<uint32_t>(o.nodes.size()) };
    uint32_t const edge_base{ static_cast<uint32_t>(o.edges.size()) };
    uint32_t const gap_base{ static_cast<uint32_t>(o.gaps.size()) };
    Frame const &f{ frames[m].f };

    // Emitted in (rank, pos) order, so a consumer walking one frame's nodes
    // walks its diagram left to right and top to bottom.
    std::vector<uint32_t> &global{ cs.global };
    vec_assign(global, f.nodes.size(), INVALID);
    for (std::vector<uint32_t> const &bucket : f.ranks) {
      for (uint32_t const v : bucket) {
        global[v] = static_cast<uint32_t>(o.nodes.size());
        vec_push_back(o.nodes, f.nodes[v]);
      }
    }
    for (OrderEdge const &e : f.edges) {
      vec_push_back(o.edges,
                    { .src = global[e.src],
                      .dst = global[e.dst],
                      .segment = e.segment,
                      .reversed = e.reversed });
    }
    for (int32_t const gap : frames[m].gaps) { vec_push_back(o.gaps, gap); }
    for (int32_t const label : frames[m].labels) { vec_push_back(o.labels, label); }
    for (uint32_t v = 0; v < f.nodes.size(); ++v) {
      OrderNode const &nd{ f.nodes[v] };
      if (nd.kind == OrderKind::State) {
        o.state_node[nd.subject] = global[v];
      } else if (nd.kind == OrderKind::Boundary) {
        o.seg_node[nd.subject] = global[v];
      }
    }
    for (SegPort const &sp : frames[m].seg_ports) {
      o.seg_port[sp.seg] = sp.port;
      if ((sp.port != INVALID) && (g.ports[sp.port].state.v != INVALID) &&
          !sided.empty() && (sided[sp.seg] != 0)) {
        o.seg_cross[sp.seg] = cross_side(sp.seg, m);
        o.seg_sided[sp.seg] = 1;
      }
    }
    for (uint32_t const seg : frames[m].cyclic) { o.seg_cyclic[seg] = 1; }

    o.sub_nodes[m] =
        make_span(node_base, static_cast<uint32_t>(o.nodes.size()) - node_base);
    o.sub_edges[m] =
        make_span(edge_base, static_cast<uint32_t>(o.edges.size()) - edge_base);
    o.sub_ranks[m] = static_cast<uint32_t>(f.ranks.size());
    o.sub_gaps[m] = make_span(gap_base, static_cast<uint32_t>(o.gaps.size()) - gap_base);
  }
}

}  // namespace scav
