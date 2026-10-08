// Builds one layered graph per frame from its children and border ports, ranks it by
// longest path, chains long edges through bends, then median-sweeps against inversions.

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
#include "scav_pod_vector.h"
#include "scav_shard.h"
#include "scav_stable_sort.h"
#include "scav_thread.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace scav {

namespace {

// Per-thread memo of ordered frames, keyed by every input the ordering reads.
Memo &frame_memo() {
  thread_local Memo m{ size_t{ 1 } << 20 };
  return m;
}

// Edge indices grouped by one endpoint.
struct Adjacency {
  PodVector<uint32_t> off, edge;
};

// `a.off[i]..a.off[i+1]` is node i's run of `a.edge`; `fill` is the caller's scratch.
void adjacency_of(Adjacency &a,
                  PodVector<OrderEdge> const &edges,
                  uint32_t nodes,
                  bool by_src,
                  PodVector<uint32_t> &fill) {
  a.off.assign(size_t{ nodes } + 1, 0);
  for (OrderEdge const &e : edges) { ++a.off[size_t{ by_src ? e.src : e.dst } + 1]; }
  for (uint32_t i = 0; i < nodes; ++i) { a.off[i + 1] += a.off[i]; }
  a.edge.assign(edges.size(), 0);
  fill.assign(a.off.begin(), a.off.end() - 1);
  for (uint32_t i = 0; i < edges.size(); ++i) {
    a.edge[fill[by_src ? edges[i].src : edges[i].dst]++] = i;
  }
}

// One frame's graph in frame-local node indices.
struct Frame {
  PodVector<OrderNode> nodes;
  PodVector<OrderEdge> edges;
  std::vector<PodVector<uint32_t>> ranks;  // rank -> node indices, in order
  Adjacency in, out;
};

// A boundary node's segment and its port, recorded as the frame builds the node.
struct SegPort {
  uint32_t seg;
  uint32_t port;
};

// One submachine's result, written by the shard that owns it.
struct FrameOrder {
  Frame f;
  PodVector<int32_t> gaps, labels;
  PodVector<SegPort> seg_ports;
  PodVector<uint32_t> cyclic;  // -> segments, those on a cycle of this frame
};

// Per-thread scratch reused across frames. Both maps hold frame-local node indices and
// return to INVALID after each frame.
struct FrameScratch {
  PodVector<uint32_t> state_local;  // parallel to states
  PodVector<uint32_t> seg_local;    // parallel to SplitGraph::segments
  Partition part;                   // -> nodes, the frame's components
  PodVector<uint32_t> dense;        // parallel to nodes; a component root's ordinal
  PodVector<uint32_t> lanes;        // boundaries x components, row-major
  PodVector<uint32_t> spanning;     // -> edges, labelled across several boundaries
  PodVector<OrderEdge> flat;        // edges into a cross-border port, held out
  PodVector<uint8_t> fixed;         // parallel to edges; 1 = oriented by an end pin
  PodVector<uint32_t> flips;        // -> edges, an initial's reversed by a pin
  PodVector<uint8_t> extreme;       // parallel to nodes; 1 first in its rank, 2 last

  // The helpers' working buffers; each sizes what it reads before reading it.
  struct Visit {
    uint32_t node;
    uint32_t next;  // -> walk.edge, the edge to try when this visit resumes
  };
  struct SouthPair {
    uint32_t north, south;
  };
  Adjacency walk;
  PodVector<uint32_t> fill, index, low, comp, held, pending, topo, renumber;
  PodVector<uint8_t> marks;
  PodVector<Visit> visits;
  PodVector<OrderEdge> chained;
  PodVector<SouthPair> pairs, merge;
  PodVector<uint32_t> south, tree, med, medians;
  std::vector<PodVector<uint32_t>> best;
};

// This thread's `FrameScratch`, both maps sized to `c` and `g`. A shard never waits on the
// pool.
FrameScratch &frame_scratch(Chart const &c, SplitGraph const &g) {
  thread_local FrameScratch sc;
  if (sc.state_local.size() != c.states.size()) {
    sc.state_local.assign(c.states.size(), INVALID);
  }
  if (sc.seg_local.size() != g.segments.size()) {
    sc.seg_local.assign(g.segments.size(), INVALID);
  }
  return sc;
}

// One `order_submachines` call's buffers, per thread and nesting depth; a call waiting on
// its shards may run another call's shards on this thread.
struct CallScratch {
  PodVector<uint32_t> seg_count, seg_off, frame_segs, fill, global;
  PodVector<int32_t> seg_label;
  PodVector<uint8_t> cut, pre_reversed, sided;
  std::vector<FrameOrder> frames;
};

// One scratch per depth, each at a fixed address, deleted when the thread exits.
struct CallStack {
  PodVector<CallScratch *> at;
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
    PodVector<CallScratch *> &s{ stack().at };
    if (depth() == s.size()) { s.push_back(new CallScratch); }
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

// The transition's src or dst state: a segment's end when it carries no port.
StateId endpoint_state(Chart const &c, SplitSegment const &seg, bool is_src) {
  Transition const &tr{ c.transitions[seg.trans.v] };
  return is_src ? tr.src : tr.dst;
}

// Strict inversions in `v` by a Fenwick tree indexed by value; `tree` is caller scratch.
uint64_t inversions(PodVector<uint32_t> const &v, PodVector<uint32_t> &tree) {
  if (v.size() < 2) { return 0; }
  uint32_t hi{ 0 };
  for (uint32_t const x : v) { hi = imax(hi, x); }
  tree.assign(static_cast<size_t>(hi) + 2, 0);
  uint64_t total{ 0 };
  uint64_t seen{ 0 };
  for (uint32_t const x : v) {
    // Earlier values above x cross it.
    uint64_t le{ 0 };
    for (uint32_t i = x + 1; i > 0; i -= i & (~i + 1U)) { le += tree[i]; }
    total += seen - le;
    ++seen;
    for (uint32_t i = x + 1; i < tree.size(); i += i & (~i + 1U)) { ++tree[i]; }
  }
  return total;
}

// Segments whose edge lies on a cycle of the frame's graph: both ends in one strongly
// connected component, found by iterative Tarjan's.
void cyclic_segments(Frame const &f, PodVector<uint32_t> &segs, FrameScratch &sc) {
  uint32_t const n{ static_cast<uint32_t>(f.nodes.size()) };
  Adjacency &out{ sc.walk };
  adjacency_of(out, f.edges, n, true, sc.fill);
  PodVector<uint32_t> &index{ sc.index };
  index.assign(n, INVALID);
  PodVector<uint32_t> &low{ sc.low };
  low.assign(n, 0);
  PodVector<uint32_t> &comp{ sc.comp };
  comp.assign(n, INVALID);
  PodVector<uint8_t> &on_stack{ sc.marks };
  on_stack.assign(n, 0);
  PodVector<uint32_t> &held{ sc.held };
  held.clear();
  PodVector<FrameScratch::Visit> &call{ sc.visits };
  call.clear();
  uint32_t counter{ 0 };
  uint32_t comps{ 0 };
  for (uint32_t root = 0; root < n; ++root) {
    if (index[root] != INVALID) { continue; }
    index[root] = counter;
    low[root] = counter;
    ++counter;
    held.push_back(root);
    on_stack[root] = 1;
    call.push_back({ .node = root, .next = out.off[root] });
    while (!call.empty()) {
      uint32_t const v{ call.back().node };
      if (call.back().next < out.off[v + 1]) {
        uint32_t const w{ f.edges[out.edge[call.back().next++]].dst };
        if (index[w] == INVALID) {
          index[w] = counter;
          low[w] = counter;
          ++counter;
          held.push_back(w);
          on_stack[w] = 1;
          call.push_back({ .node = w, .next = out.off[w] });
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
    if ((e.src != e.dst) && (comp[e.src] == comp[e.dst])) { segs.push_back(e.segment); }
  }
}

// Makes the frame a DAG by DFS walks that reverse each edge closing onto the walk's path:
// against the edges from each trailing port, then from each leading port, then node order.
void orient_acyclic(Frame &f,
                    PodVector<uint8_t> const &pre,
                    PodVector<uint8_t> const &fixed,
                    FrameScratch &sc) {
  // Before the walk, reverses each edge `pre` names; skips `fixed` edges, which an end pin
  // has oriented.
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
  Adjacency &in{ f.in };  // scratch here; `assign_ranks` rebuilds it
  adjacency_of(in, f.edges, n, false, sc.fill);

  enum : uint8_t { White, Gray, Black };
  PodVector<uint8_t> &color{ sc.marks };
  color.assign(n, White);
  PodVector<FrameScratch::Visit> &stack{ sc.visits };
  stack.clear();
  // A DFS from `root` along out-edges (`forward`) or in-edges.
  auto const walk = [&](uint32_t root, bool forward) {
    Adjacency const &a{ forward ? out : in };
    if (color[root] != White) { return; }
    color[root] = Gray;
    stack.push_back({ .node = root, .next = a.off[root] });
    while (!stack.empty()) {
      uint32_t const node{ stack.back().node };
      if (stack.back().next == a.off[node + 1]) {
        color[node] = Black;
        stack.pop_back();
        continue;
      }
      OrderEdge &e{ f.edges[a.edge[stack.back().next++]] };
      uint32_t const next{ forward ? e.dst : e.src };
      if (color[next] == Gray) {
        e.reversed ^= 1U;  // an edge a pin turned and the walk turns back runs as authored
        trace_emit({ .kind = TraceKind::EdgeReversed, .seg = { .seg = e.segment } });
        uint32_t const swap{ e.src };
        e.src = e.dst;
        e.dst = swap;
        continue;
      }
      if (color[next] == White) {
        color[next] = Gray;
        stack.push_back({ .node = next, .next = a.off[next] });
      }
    }
  };
  auto const port = [&](uint32_t v, Adjacency const &a) {
    return (f.nodes[v].kind == OrderKind::Boundary) && (a.off[v + 1] > a.off[v]);
  };
  for (uint32_t root = 0; root < n; ++root) {
    if (port(root, in)) { walk(root, false); }
  }
  for (uint32_t root = 0; root < n; ++root) {
    if (port(root, out)) { walk(root, true); }
  }
  for (uint32_t root = 0; root < n; ++root) { walk(root, true); }
}

// Longest-path ranks, sink boundaries on the last rank, a pull-right pass for non-boundary
// nodes with more successors than predecessors, then empty ranks squeezed out.
void assign_ranks(Frame &f, FrameScratch &sc) {
  uint32_t const n{ static_cast<uint32_t>(f.nodes.size()) };
  if (n == 0) { return; }
  adjacency_of(f.out, f.edges, n, true, sc.fill);
  adjacency_of(f.in, f.edges, n, false, sc.fill);
  auto const out_deg = [&](uint32_t v) { return f.out.off[v + 1] - f.out.off[v]; };
  auto const in_deg = [&](uint32_t v) { return f.in.off[v + 1] - f.in.off[v]; };

  PodVector<uint32_t> &pending{ sc.pending };
  pending.assign(n, 0);
  for (uint32_t v = 0; v < n; ++v) { pending[v] = in_deg(v); }
  PodVector<uint32_t> &topo{ sc.topo };
  topo.clear();
  topo.reserve(n);
  for (uint32_t v = 0; v < n; ++v) {
    if (pending[v] == 0) { topo.push_back(v); }
  }
  for (uint32_t at = 0; at < topo.size(); ++at) {
    uint32_t const v{ topo[at] };
    for (uint32_t k = f.out.off[v]; k < f.out.off[v + 1]; ++k) {
      uint32_t const w{ f.edges[f.out.edge[k]].dst };
      if (--pending[w] == 0) { topo.push_back(w); }
    }
  }

  for (uint32_t const v : topo) {
    for (uint32_t k = f.out.off[v]; k < f.out.off[v + 1]; ++k) {
      OrderNode &d{ f.nodes[f.edges[f.out.edge[k]].dst] };
      d.rank = imax(d.rank, f.nodes[v].rank + 1U);
    }
  }

  uint32_t top{ 0 };
  for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
  for (uint32_t v = 0; v < n; ++v) {
    if ((f.nodes[v].kind == OrderKind::Boundary) && (out_deg(v) == 0)) {
      f.nodes[v].rank = top;
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

  PodVector<uint32_t> &used{ sc.held };
  used.assign(size_t{ top } + 1, 0);
  for (OrderNode const &nd : f.nodes) { used[nd.rank] = 1; }
  PodVector<uint32_t> &renumber{ sc.renumber };
  renumber.assign(used.size(), 0);
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

// Chains each forward multi-rank edge through one bend per intervening rank, keeping its
// segment and reversal; a segment `cut` names keeps its long edge.
void chain_long_edges(Frame &f, PodVector<uint8_t> const &cut, FrameScratch &sc) {
  PodVector<OrderEdge> &out{ sc.chained };
  out.clear();
  out.reserve(f.edges.size());
  for (OrderEdge const &e : f.edges) {
    uint32_t const from{ f.nodes[e.src].rank };
    uint32_t const to{ f.nodes[e.dst].rank };
    if (((to - from) <= 1) || ((e.segment < cut.size()) && (cut[e.segment] != 0))) {
      out.push_back(e);
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
      f.nodes.push_back(
          { .kind = OrderKind::Bend, .subject = e.segment, .rank = r, .pos = 0 });
      out.push_back(
          { .src = prev, .dst = bend, .segment = e.segment, .reversed = e.reversed });
      prev = bend;
    }
    out.push_back(
        { .src = prev, .dst = e.dst, .segment = e.segment, .reversed = e.reversed });
  }
  f.edges.swap(out);
  adjacency_of(f.out, f.edges, static_cast<uint32_t>(f.nodes.size()), true, sc.fill);
  adjacency_of(f.in, f.edges, static_cast<uint32_t>(f.nodes.size()), false, sc.fill);
}

// Rank buckets in node order; an `extreme` node goes first (1) or last (2) and no sweep
// moves it.
void bucket_ranks(Frame &f, PodVector<uint8_t> const &extreme) {
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
      if (at == want) { f.ranks[f.nodes[v].rank].push_back(v); }
    }
  }
  for (PodVector<uint32_t> const &bucket : f.ranks) {
    for (uint32_t i = 0; i < bucket.size(); ++i) { f.nodes[bucket[i]].pos = i; }
  }
}

// South positions of the edges leaving `north_rank`, sorted by (north, south).
PodVector<uint32_t> const &south_of(Frame const &f,
                                    uint32_t north_rank,
                                    FrameScratch &sc) {
  using Pair = FrameScratch::SouthPair;
  PodVector<Pair> &pairs{ sc.pairs };
  pairs.clear();
  for (uint32_t const v : f.ranks[north_rank]) {
    for (uint32_t k = f.out.off[v]; k < f.out.off[v + 1]; ++k) {
      OrderEdge const &e{ f.edges[f.out.edge[k]] };
      pairs.push_back({ .north = f.nodes[e.src].pos, .south = f.nodes[e.dst].pos });
    }
  }
  // Edges sharing a north or a south node score no crossing.
  scav_stable_sort(pairs, sc.merge, [](Pair const &a, Pair const &b) {
    return (a.north != b.north) ? (a.north < b.north) : (a.south < b.south);
  });
  PodVector<uint32_t> &south{ sc.south };
  south.clear();
  south.reserve(pairs.size());
  for (Pair const &pr : pairs) { south.push_back(pr.south); }
  return south;
}

uint64_t total_crossings(Frame const &f, FrameScratch &sc) {
  uint64_t total{ 0 };
  for (uint32_t r = 0; (r + 1) < f.ranks.size(); ++r) {
    total += inversions(south_of(f, r, sc), sc.tree);
  }
  return total;
}

// Median position of the node's neighbours in the fixed rank, INVALID when it has none
// there; `scratch` is caller-owned.
uint32_t median_of(Frame const &f,
                   uint32_t node,
                   bool from_predecessors,
                   PodVector<uint32_t> &scratch) {
  Adjacency const &a{ from_predecessors ? f.in : f.out };
  scratch.clear();
  for (uint32_t k = a.off[node]; k < a.off[node + 1]; ++k) {
    OrderEdge const &e{ f.edges[a.edge[k]] };
    scratch.push_back(f.nodes[from_predecessors ? e.src : e.dst].pos);
  }
  if (scratch.empty()) { return INVALID; }
  scav_insertion_sort(scratch.data(),
                      scratch.data() + scratch.size(),
                      [](uint32_t a2, uint32_t b2) { return a2 < b2; });
  return scratch[scratch.size() / 2];
}

// Bubble-sorts the rank by median with adjacent swaps between two nodes that both have
// one; a node with no median keeps its slot.
void reorder_rank(Frame &f, uint32_t rank, bool from_predecessors, FrameScratch &sc) {
  PodVector<uint32_t> &bucket{ f.ranks[rank] };
  PodVector<uint32_t> &med{ sc.med };
  med.assign(bucket.size(), INVALID);
  PodVector<uint32_t> &scratch{ sc.medians };
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

// Up to `sweeps` alternating median sweeps; keeps the ordering with the fewest crossings.
void minimize_crossings(Frame &f, uint32_t sweeps, FrameScratch &sc) {
  if (f.ranks.size() < 2) { return; }
  std::vector<PodVector<uint32_t>> &best{ sc.best };
  auto const copy_ranks = [](std::vector<PodVector<uint32_t>> &to,
                             std::vector<PodVector<uint32_t>> const &from) {
    vec_resize(to, from.size());
    for (size_t r = 0; r < from.size(); ++r) {
      to[r].assign(from[r].begin(), from[r].end());
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
  for (PodVector<uint32_t> const &bucket : f.ranks) {
    for (uint32_t i = 0; i < bucket.size(); ++i) { f.nodes[bucket[i]].pos = i; }
  }
}

// Moves each cross-border port's mate toward the port's end of their rank, one swap at a
// time while no crossing is added; an `extreme` node or an earlier mate stops it.
void slide_to_ports(Frame &f,
                    PodVector<OrderEdge> const &flat,
                    PodVector<uint8_t> const &extreme,
                    FrameScratch &sc) {
  if (flat.empty()) { return; }
  PodVector<uint8_t> &held{ sc.marks };
  held.assign(f.nodes.size(), 0);
  auto const stops = [&](uint32_t v) {
    return (held[v] != 0) || ((v < extreme.size()) && (extreme[v] != 0));
  };
  uint64_t cost{ total_crossings(f, sc) };
  for (OrderEdge const &e : flat) {
    bool const at_src{ f.nodes[e.src].kind == OrderKind::Boundary };
    uint32_t const port{ at_src ? e.src : e.dst };
    uint32_t const mate{ at_src ? e.dst : e.src };
    if (stops(mate) || (f.nodes[mate].rank != f.nodes[port].rank)) { continue; }
    held[mate] = 1;
    bool const last{ extreme[port] == 2 };
    PodVector<uint32_t> &bucket{ f.ranks[f.nodes[mate].rank] };
    for (;;) {
      uint32_t const at{ f.nodes[mate].pos };
      if (last ? ((at + 1) >= bucket.size()) : (at == 0)) { break; }
      uint32_t const to{ last ? (at + 1) : (at - 1) };
      uint32_t const other{ bucket[to] };
      if (stops(other)) { break; }
      std::swap(bucket[at], bucket[to]);
      f.nodes[mate].pos = to;
      f.nodes[other].pos = at;
      uint64_t const next{ total_crossings(f, sc) };
      if (next > cost) {
        std::swap(bucket[at], bucket[to]);
        f.nodes[mate].pos = at;
        f.nodes[other].pos = to;
        break;
      }
      cost = next;
    }
  }
}

}  // namespace

uint64_t rank_crossings(PodVector<uint32_t> const &south_positions) {
  PodVector<uint32_t> tree;
  return inversions(south_positions, tree);
}

namespace {

// Puts each initial pseudostate one rank before its lowest-ranked neighbour, or one after
// its highest where its edge was reversed; when before is rank 0, every other node moves
// up.
void seat_initials(Chart const &c, Frame &f) {
  for (uint32_t v = 0; v < f.nodes.size(); ++v) {
    OrderNode const &nd{ f.nodes[v] };
    if ((nd.kind != OrderKind::State) ||
        (c.states[nd.subject].kind != StateKind::Initial)) {
      continue;
    }
    uint32_t nearest{ INVALID };
    uint32_t farthest{ 0 };
    bool after{ false };
    for (OrderEdge const &e : f.edges) {
      if ((e.src == v) && (e.dst != v)) { nearest = imin(nearest, f.nodes[e.dst].rank); }
      if ((e.dst == v) && (e.src != v)) {
        nearest = imin(nearest, f.nodes[e.src].rank);
        farthest = imax(farthest, f.nodes[e.src].rank);
        after = true;
      }
    }
    if (nearest == INVALID) { continue; }
    if (after) {
      f.nodes[v].rank = farthest + 1;
      continue;
    }
    if (nearest == 0) {
      for (uint32_t u = 0; u < f.nodes.size(); ++u) {
        if (u != v) { ++f.nodes[u].rank; }
      }
      nearest = 1;
    }
    f.nodes[v].rank = nearest - 1;
  }
}

// Renumbers ranks onto those that still hold a node, keeping their order.
void squeeze_ranks(Frame &f, FrameScratch &sc) {
  uint32_t top{ 0 };
  for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
  PodVector<uint32_t> &onto{ sc.renumber };
  onto.assign(static_cast<size_t>(top) + 1, INVALID);
  for (OrderNode const &nd : f.nodes) { onto[nd.rank] = 0; }
  uint32_t next{ 0 };
  for (uint32_t r = 0; r <= top; ++r) {
    if (onto[r] == 0) { onto[r] = next++; }
  }
  for (OrderNode &nd : f.nodes) { nd.rank = onto[nd.rank]; }
}

// Charges rank boundaries, chains long edges, buckets, sweeps and slides mates to `flat`'s
// ports; of `f` it reads only `nodes[].rank` and `edges`. Not idempotent: chaining appends
// bend nodes.
void rank_derived(Frame &f,
                  PodVector<int32_t> &gaps,
                  PodVector<int32_t> &labels,
                  PodVector<int32_t> const &seg_label,
                  PodVector<uint8_t> const &cut,
                  PodVector<uint8_t> const &extreme,
                  PodVector<OrderEdge> const &flat,
                  scav_profile const &p,
                  FrameScratch &sc) {
  // Before chaining: an edge across one boundary charges its label there; longer edges
  // wait in `spanning`.
  uint32_t top{ 0 };
  for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
  gaps.assign(top, 0);
  labels.assign(top, 0);
  PodVector<uint32_t> &spanning{ sc.spanning };
  spanning.clear();
  for (uint32_t i = 0; i < f.edges.size(); ++i) {
    OrderEdge const &e{ f.edges[i] };
    int32_t const label{ seg_label[e.segment] };
    if ((label == 0) || (f.nodes[e.src].rank == f.nodes[e.dst].rank)) { continue; }
    uint32_t const from{ imin(f.nodes[e.src].rank, f.nodes[e.dst].rank) };
    uint32_t const to{ imax(f.nodes[e.src].rank, f.nodes[e.dst].rank) };
    if ((to - from) > 1) {
      spanning.push_back(i);
      continue;
    }
    gaps[from] = imax(gaps[from], label);
    labels[from] = imax(labels[from], label);
    trace_emit({ .kind = TraceKind::GapCharged,
                 .pass = static_cast<uint16_t>(GapCause::Label),
                 .gap = { .boundary = from, .seg = e.segment, .width = label } });
  }

  // Per component, each edge turns in its first and last boundary; two or more turns in
  // one boundary widen it to a line of type each, capped at SPACE_MAX.
  Partition &part{ sc.part };
  part.reset(f.nodes.size());
  for (OrderEdge const &e : f.edges) { part.join(e.src, e.dst); }
  // Component ordinals; the column index of the `lanes` table.
  PodVector<uint32_t> &dense{ sc.dense };
  dense.assign(f.nodes.size(), INVALID);
  uint32_t parts{ 0 };
  for (uint32_t i = 0; i < f.nodes.size(); ++i) {
    uint32_t const root{ part.root(i) };
    if (dense[root] == INVALID) { dense[root] = parts++; }
  }
  PodVector<uint32_t> &lanes{ sc.lanes };
  lanes.assign(gaps.size() * parts, 0);
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
    if (from == to) { continue; }  // same rank: no boundary
    uint32_t const of{ dense[part.root(e.src)] };
    turn(from, of);
    if (to > (from + 1)) { turn(to - 1, of); }
  }

  // Widest label first, a spanning edge charges the widest boundary it crosses, nearest
  // the middle among equals; traced `Held` when that gap already covers it.
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
  slide_to_ports(f, flat, extreme, sc);
}

}  // namespace

uint32_t label_segment(Chart const &c, SplitGraph const &g, uint32_t t) {
  if (t < g.trans_label.size()) { return g.trans_label[t]; }
  if ((t >= g.trans_segments.size()) || (t >= c.transitions.size())) { return INVALID; }
  Span const segs{ g.trans_segments[t] };
  // An inner loop is labelled in its loop room.
  if ((segs.len == 0) || inner_loop(c, t)) { return INVALID; }
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
  o.sub_nodes.assign(c.submachines.size(), Span{});
  o.sub_edges.assign(c.submachines.size(), Span{});
  o.sub_ranks.assign(c.submachines.size(), 0);
  o.sub_down.assign(c.submachines.size(), 0);
  for (OrientPin const &pin : pins.orients) {
    if (pin.frame.v < o.sub_down.size()) { o.sub_down[pin.frame.v] = 1; }
  }
  o.sub_fold.assign(c.submachines.size(), 0);
  o.sub_fold_cut.assign(c.submachines.size(), 0);
  for (FoldPin const &pin : pins.folds) {
    if ((pin.frame.v < o.sub_fold.size()) && (pin.mode <= FOLD_NEVER)) {
      o.sub_fold[pin.frame.v] = static_cast<uint8_t>(pin.mode + 1);
      o.sub_fold_cut[pin.frame.v] = pin.layer;
    }
  }
  o.state_loop.assign(c.states.size(), 0);
  for (LoopPin const &pin : pins.loops) {
    if ((pin.state.v < o.state_loop.size()) && (pin.face < 4) && (pin.end < 2)) {
      o.state_loop[pin.state.v] = static_cast<uint8_t>(1 + (pin.face * 2) + pin.end);
    }
  }
  o.sub_gaps.assign(c.submachines.size(), Span{});
  o.state_node.assign(c.states.size(), INVALID);
  o.seg_node.assign(g.segments.size(), INVALID);
  o.seg_port.assign(g.segments.size(), INVALID);
  o.seg_cross.assign(g.segments.size(), 0);
  o.seg_sided.assign(g.segments.size(), 0);
  o.seg_side.assign(g.segments.size(), 0);
  o.seg_cyclic.assign(g.segments.size(), 0);

  CallScope const scope;
  CallScratch &cs{ scope.scratch() };

  // Segments grouped by frame: `frame_segs[seg_off[m]..seg_off[m+1]]` are frame `m`'s.
  PodVector<uint32_t> &seg_count{ cs.seg_count };
  seg_count.assign(c.submachines.size(), 0);
  for (SplitSegment const &seg : g.segments) {
    if (seg.frame.v != INVALID) { ++seg_count[seg.frame.v]; }
  }
  PodVector<uint32_t> &seg_off{ cs.seg_off };
  seg_off.assign(seg_count.size() + 1, 0);
  for (uint32_t i = 0; i < seg_count.size(); ++i) {
    seg_off[i + 1] = seg_off[i] + seg_count[i];
  }
  PodVector<uint32_t> &frame_segs{ cs.frame_segs };
  frame_segs.assign(g.segments.size(), 0);
  cs.fill.assign(seg_off.begin(), seg_off.end() - 1);
  for (uint32_t i = 0; i < g.segments.size(); ++i) {
    SubmachineId const frame{ g.segments[i].frame };
    if (frame.v != INVALID) { frame_segs[cs.fill[frame.v]++] = i; }
  }

  // A label is charged to one rank boundary in its `label_segment`'s frame, by its extent
  // along that frame's ranks: its width across the page, its height down it.
  PodVector<int32_t> &seg_label{ cs.seg_label };
  seg_label.assign(g.segments.size(), 0);
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    scav_path_box const &box{ s.path_box[i] };
    uint32_t const at{ label_segment(c, g, box.subject) };
    if (at == INVALID) { continue; }
    uint32_t const frame{ g.segments[at].frame.v };
    bool const down{ (frame < o.sub_down.size()) && (o.sub_down[frame] != 0) };
    seg_label[at] += down ? box.h : box.w;
  }

  // Marks each pinned `{trans, leg}` in a table parallel to segments; empty for no pins.
  auto const resolve_pins = [&g](auto const &rows, PodVector<uint8_t> &table) {
    table.clear();
    if (rows.empty()) { return; }
    table.assign(g.segments.size(), 0);
    for (auto const &row : rows) {
      if ((row.trans.v == INVALID) || (row.trans.v >= g.trans_segments.size())) {
        continue;
      }
      Span const segs{ g.trans_segments[row.trans.v] };
      if (row.leg >= segs.len) { continue; }
      table[segs.off + row.leg] = 1;
    }
  };
  PodVector<uint8_t> &cut{ cs.cut };
  PodVector<uint8_t> &pre_reversed{ cs.pre_reversed };
  resolve_pins(pins.cuts, cut);
  resolve_pins(pins.reverses, pre_reversed);
  // Per segment, 1 + an end pin's side at a state-border port, 0 for none; set on the leg
  // inside the pinned port's border, whose boundary node stands for the port.
  PodVector<uint8_t> &sided{ cs.sided };
  sided.clear();
  for (EndPin const &pin : pins.ends) {
    if ((pin.trans.v == INVALID) || (pin.trans.v >= g.trans_segments.size()) ||
        (pin.end > 1) || (pin.face > 3)) {
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
    if (sided.empty()) { sided.assign(g.segments.size(), 0); }
    sided[seg] = static_cast<uint8_t>(pin.face + 1);
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
  frames.resize(c.submachines.size());
  uint32_t const profile_word{ memo_profile(p) };

  // Writes `frames[m]`, `sc` and this thread's memo; `sc`'s maps end all INVALID.
  auto const order_frame = [&](uint32_t m, FrameScratch &sc) {
    TraceFrame const traced{ SubmachineId{ m } };
    Frame &f{ frames[m].f };
    PodVector<SegPort> &seg_ports{ frames[m].seg_ports };
    f.nodes.clear();
    f.edges.clear();
    seg_ports.clear();

    Span const kids{ c.submachines[m].children };
    for (uint32_t k = 0; k < kids.len; ++k) {
      uint32_t const child{ c.state_ids[kids.off + k].v };
      if (c.states[child].live == 0) { continue; }
      sc.state_local[child] = static_cast<uint32_t>(f.nodes.size());
      f.nodes.push_back(
          { .kind = OrderKind::State, .subject = child, .rank = 0, .pos = 0 });
    }

    // A port on a child's border is that child; one on the frame's own border is a node,
    // at most one per segment.
    auto const boundary_node = [&](uint32_t seg, uint32_t port) {
      if (sc.seg_local[seg] == INVALID) {
        sc.seg_local[seg] = static_cast<uint32_t>(f.nodes.size());
        seg_ports.push_back({ .seg = seg, .port = port });
        f.nodes.push_back(
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
        // The endpoint encloses this frame: a boundary node with no port.
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
      if ((pt.sub.v == m) || (pt.into.v == m)) { return boundary_node(seg, port); }
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
      f.edges.push_back({ .src = src, .dst = dst, .segment = seg, .reversed = 0 });
    }
    // Whether an end pin holds the edge's port: a boundary node for a port on a state's
    // border.
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

    // Memo key: serial, profile, the frame as built, per edge its pins and label charge,
    // and its rank pins. A traced run skips the memo.
    bool const tracing{ trace_sink() != nullptr };
    thread_local PodVector<uint32_t> key;
    thread_local PodVector<int32_t> value;
    key.clear();
    if (!tracing) {
      key.push_back(g.serial);
      key.push_back(profile_word);
      if (g.serial != 0) {
        key.push_back(m);  // the frame as built is a function of the graph and `m`
      } else {
        key.push_back(static_cast<uint32_t>(f.nodes.size()));
        for (OrderNode const &nd : f.nodes) {
          key.push_back(static_cast<uint32_t>(nd.kind));
          key.push_back(nd.subject);
          key.push_back((nd.kind == OrderKind::State)
                            ? static_cast<uint32_t>(c.states[nd.subject].kind)
                            : 0U);
        }
        key.push_back(static_cast<uint32_t>(f.edges.size()));
      }
      for (OrderEdge const &e : f.edges) {
        if (g.serial == 0) {
          key.push_back(e.src);
          key.push_back(e.dst);
          key.push_back(e.segment);
        }
        uint32_t const reversed{ pre_reversed.empty() ? 0U : pre_reversed[e.segment] };
        uint32_t const cuts{ cut.empty() ? 0U : cut[e.segment] };
        // Each field fits in a byte.
        key.push_back(reversed | (cuts << 8U) | (uint32_t{ cross_of(e) } << 16U) |
                      (uint32_t{ lead_of(e) } << 24U));
        key.push_back(static_cast<uint32_t>(seg_label[e.segment]));
      }
      for (RankPin const &pin : pins.ranks) {
        if ((pin.state.v == INVALID) || (pin.state.v >= sc.state_local.size())) {
          continue;
        }
        uint32_t const at{ sc.state_local[pin.state.v] };
        if (at >= f.nodes.size()) { continue; }
        key.push_back(at);
        key.push_back(pin.rank);
      }
    }
    Memo &memo{ frame_memo() };
    int32_t const *hit{ nullptr };
    uint32_t len{ 0 };
    if (!tracing && memo.find(key, hit, len)) {
      auto word = [&, at = uint32_t{ 0 }]() mutable {
        return static_cast<uint32_t>(hit[at++]);
      };
      f.nodes.resize(word());
      for (OrderNode &nd : f.nodes) {
        nd = { .kind = static_cast<OrderKind>(word()),
               .subject = word(),
               .rank = word(),
               .pos = word() };
      }
      f.edges.resize(word());
      for (OrderEdge &e : f.edges) {
        e = { .src = word(), .dst = word(), .segment = word(), .reversed = word() };
      }
      vec_resize(f.ranks, word());
      for (PodVector<uint32_t> &bucket : f.ranks) {
        bucket.resize(word());
        for (uint32_t &v : bucket) { v = word(); }
      }
      frames[m].gaps.resize(word());
      for (int32_t &gap : frames[m].gaps) { gap = static_cast<int32_t>(word()); }
      frames[m].labels.resize(frames[m].gaps.size());
      for (int32_t &label : frames[m].labels) { label = static_cast<int32_t>(word()); }
      frames[m].cyclic.resize(word());
      for (uint32_t &seg : frames[m].cyclic) { seg = word(); }
    } else {
      cyclic_segments(f, frames[m].cyclic, sc);
      // An edge into a cross-border port is held out of ranking and takes its mate's rank;
      // one into a port pinned to a rank border is oriented and `fixed`.
      PodVector<OrderEdge> &flat{ sc.flat };
      flat.clear();
      PodVector<uint8_t> &fixed{ sc.fixed };
      fixed.clear();
      if (!sided.empty()) {
        uint32_t kept{ 0 };
        for (OrderEdge const &e : f.edges) {
          if (cross_of(e) != 0) {
            flat.push_back(e);
          } else {
            f.edges[kept++] = e;
          }
        }
        f.edges.resize(kept);
        fixed.assign(f.edges.size(), 0);
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
      // A reversal pin on an initial's edge takes no part in ranking; the dot moves after
      // its target once ranks settle.
      PodVector<uint32_t> &flips{ sc.flips };
      flips.clear();
      for (uint32_t k = 0; !pre_reversed.empty() && (k < f.edges.size()); ++k) {
        OrderEdge const &e{ f.edges[k] };
        OrderNode const &from{ f.nodes[e.src] };
        if ((pre_reversed[e.segment] != 0) && (from.kind == OrderKind::State) &&
            (c.states[from.subject].kind == StateKind::Initial)) {
          flips.push_back(k);
        }
      }
      if (!flips.empty() && (fixed.size() != f.edges.size())) {
        fixed.assign(f.edges.size(), 0);
      }
      for (uint32_t const k : flips) { fixed[k] = 1; }
      orient_acyclic(f, pre_reversed, fixed, sc);
      assign_ranks(f, sc);

      // Rank pins apply after ranking, before `rank_derived`; an initial's pin is ignored.
      bool moved{ false };
      if (!pins.ranks.empty()) {
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
      }
      for (uint32_t const k : flips) {
        OrderEdge &e{ f.edges[k] };
        std::swap(e.src, e.dst);
        e.reversed = 1;
        trace_emit({ .kind = TraceKind::EdgeReversed, .seg = { .seg = e.segment } });
      }
      if (moved || !flips.empty()) {
        seat_initials(c, f);
        squeeze_ranks(f, sc);
      }
      PodVector<uint8_t> &extreme{ sc.extreme };
      extreme.clear();
      if (!flat.empty()) {
        extreme.assign(f.nodes.size(), 0);
        for (OrderEdge const &e : flat) {
          bool const at_src{ f.nodes[e.src].kind == OrderKind::Boundary };
          uint32_t const port{ at_src ? e.src : e.dst };
          f.nodes[port].rank = f.nodes[at_src ? e.dst : e.src].rank;
          extreme[port] = cross_of(e);
        }
        squeeze_ranks(f, sc);
      }
      rank_derived(f,
                   frames[m].gaps,
                   frames[m].labels,
                   seg_label,
                   cut,
                   extreme,
                   flat,
                   p,
                   sc);
      f.edges.insert(f.edges.end(), flat.begin(), flat.end());
      if (!tracing) {
        value.clear();
        auto const put = [](uint32_t w) { value.push_back(static_cast<int32_t>(w)); };
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
        for (PodVector<uint32_t> const &bucket : f.ranks) {
          put(static_cast<uint32_t>(bucket.size()));
          for (uint32_t const v : bucket) { put(v); }
        }
        put(static_cast<uint32_t>(frames[m].gaps.size()));
        for (int32_t const gap : frames[m].gaps) { value.push_back(gap); }
        for (int32_t const label : frames[m].labels) { value.push_back(label); }
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
  o.nodes.reserve(node_rows);
  o.edges.reserve(edge_rows);
  o.gaps.reserve(gap_rows);
  o.labels.reserve(gap_rows);

  // Emitted in submachine order, so numbering and spans match a single-threaded run.
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    uint32_t const node_base{ static_cast<uint32_t>(o.nodes.size()) };
    uint32_t const edge_base{ static_cast<uint32_t>(o.edges.size()) };
    uint32_t const gap_base{ static_cast<uint32_t>(o.gaps.size()) };
    Frame const &f{ frames[m].f };
    bool const down{ o.sub_down[m] != 0 };

    // Nodes in (rank, pos) order.
    PodVector<uint32_t> &global{ cs.global };
    global.assign(f.nodes.size(), INVALID);
    for (PodVector<uint32_t> const &bucket : f.ranks) {
      for (uint32_t const v : bucket) {
        global[v] = static_cast<uint32_t>(o.nodes.size());
        o.nodes.push_back(f.nodes[v]);
      }
    }
    for (OrderEdge const &e : f.edges) {
      o.edges.push_back({ .src = global[e.src],
                          .dst = global[e.dst],
                          .segment = e.segment,
                          .reversed = e.reversed });
    }
    for (int32_t const gap : frames[m].gaps) { o.gaps.push_back(gap); }
    for (int32_t const label : frames[m].labels) { o.labels.push_back(label); }
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
      uint8_t const cross{ o.seg_cross[sp.seg] };
      o.seg_side[sp.seg] = static_cast<uint8_t>(
          (cross != 0) ? ((down ? 0U : 2U) + (cross - 1U)) : ((down ? 2U : 0U) + 1U));
    }
    for (OrderEdge const &e : f.edges) {
      OrderNode const &from{ f.nodes[e.src] };
      if ((from.kind == OrderKind::Boundary) && (o.seg_cross[from.subject] == 0)) {
        o.seg_side[from.subject] = down ? 2U : 0U;  // the leading rank border
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
