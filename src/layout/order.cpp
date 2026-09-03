// One frame at a time: nodes from the frame's own children and from the ports
// on its enclosing border, edges from the segments routed there, ranks by
// longest path, bends for whatever spans more than one rank, then median
// sweeps against an inversion count.

#include "layout/order.h"

#include "layout/decompose.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"
#include "scav_stable_sort.h"

#include <cstdint>
#include <vector>

namespace scav {

namespace {

// Prefix sums over `count`, so a per-key bucket is a slice of one array rather
// than a vector of vectors. `out[i]..out[i+1]` is key i's run.
std::vector<uint32_t> offsets_of(std::vector<uint32_t> const &count) {
  std::vector<uint32_t> off(count.size() + 1, 0);
  for (uint32_t i = 0; i < count.size(); ++i) { off[i + 1] = off[i] + count[i]; }
  return off;
}

// Edge indices grouped by one endpoint. Built once per frame and rebuilt after
// chaining, because every traversal below would otherwise rescan every edge
// and turn a sweep quadratic.
struct Adjacency {
  std::vector<uint32_t> off, edge;
};

Adjacency adjacency_of(std::vector<OrderEdge> const &edges, uint32_t nodes, bool by_src) {
  std::vector<uint32_t> count(nodes, 0);
  for (OrderEdge const &e : edges) { ++count[by_src ? e.src : e.dst]; }
  Adjacency a;
  a.off = offsets_of(count);
  a.edge.assign(edges.size(), 0);
  std::vector<uint32_t> fill(a.off.begin(), a.off.end() - 1);
  for (uint32_t i = 0; i < edges.size(); ++i) {
    a.edge[fill[by_src ? edges[i].src : edges[i].dst]++] = i;
  }
  return a;
}

// One frame's graph in frame-local indices. Ranks and positions live beside
// the nodes until the frame is flushed into `SubmachineOrders`.
struct Frame {
  std::vector<OrderNode> nodes;
  std::vector<OrderEdge> edges;
  std::vector<std::vector<uint32_t>> ranks;  // rank -> node indices, in order
  Adjacency in, out;
};

// The endpoint state of a segment's src or dst end when that end carries no
// port: the transition's own src or dst.
StateId endpoint_state(Chart const &c, SplitSegment const &seg, bool is_src) {
  Transition const &tr{ c.transitions[seg.trans.v] };
  return is_src ? tr.src : tr.dst;
}

// Inversions in `v` by a Fenwick tree over its own values, which are already
// positions and so already bounded by the rank size.
uint64_t inversions(std::vector<uint32_t> const &v) {
  if (v.size() < 2) { return 0; }
  uint32_t hi{ 0 };
  for (uint32_t const x : v) { hi = imax(hi, x); }
  std::vector<uint32_t> tree(static_cast<size_t>(hi) + 2, 0);
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

// Cycle breaking by iterative depth-first search in node order: an edge that
// closes back onto the current path is the one reversed, so the frame becomes
// a DAG without any node moving.
void orient_acyclic(Frame &f) {
  uint32_t const n{ static_cast<uint32_t>(f.nodes.size()) };
  Adjacency const out{ adjacency_of(f.edges, n, true) };

  enum : uint8_t { White, Gray, Black };
  std::vector<uint8_t> color(n, White);
  struct Visit {
    uint32_t node;
    uint32_t next;  // -> out.edge, the edge to try when this frame resumes
  };
  std::vector<Visit> stack;
  for (uint32_t root = 0; root < n; ++root) {
    if (color[root] != White) { continue; }
    color[root] = Gray;
    stack.push_back({ .node = root, .next = out.off[root] });
    while (!stack.empty()) {
      uint32_t const node{ stack.back().node };
      if (stack.back().next == out.off[node + 1]) {
        color[node] = Black;
        stack.pop_back();
        continue;
      }
      OrderEdge &e{ f.edges[out.edge[stack.back().next++]] };
      if (color[e.dst] == Gray) {
        e.reversed = 1;
        uint32_t const swap{ e.src };
        e.src = e.dst;
        e.dst = swap;
        continue;
      }
      if (color[e.dst] == White) {
        color[e.dst] = Gray;
        stack.push_back({ .node = e.dst, .next = out.off[e.dst] });
      }
    }
  }
}

// Longest path from the sources, then one pull-right pass for nodes with more
// successors than predecessors, then empty ranks squeezed out. Boundary nodes
// are excluded from the pull because their rank is what puts them on the
// frame's left or right border, and a sink boundary is pinned to the last rank.
void assign_ranks(Frame &f) {
  uint32_t const n{ static_cast<uint32_t>(f.nodes.size()) };
  if (n == 0) { return; }
  f.out = adjacency_of(f.edges, n, true);
  f.in = adjacency_of(f.edges, n, false);
  auto const out_deg = [&](uint32_t v) { return f.out.off[v + 1] - f.out.off[v]; };
  auto const in_deg = [&](uint32_t v) { return f.in.off[v + 1] - f.in.off[v]; };

  std::vector<uint32_t> pending(n, 0);
  for (uint32_t v = 0; v < n; ++v) { pending[v] = in_deg(v); }
  std::vector<uint32_t> topo;
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

  for (uint32_t at = static_cast<uint32_t>(topo.size()); at-- > 0;) {
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

  std::vector<uint32_t> used(size_t{ top } + 1, 0);
  for (OrderNode const &nd : f.nodes) { used[nd.rank] = 1; }
  std::vector<uint32_t> renumber(used.size(), 0);
  uint32_t next{ 0 };
  for (uint32_t r = 0; r < used.size(); ++r) {
    renumber[r] = next;
    next += used[r];
  }
  for (OrderNode &nd : f.nodes) { nd.rank = renumber[nd.rank]; }
}

// A bend per intervening rank, so every emitted edge spans exactly one. The
// chain keeps the original edge's segment and reversal, which is what lets
// phase 3 walk it back into one polyline.
void chain_long_edges(Frame &f) {
  std::vector<OrderEdge> out;
  out.reserve(f.edges.size());
  for (OrderEdge const &e : f.edges) {
    uint32_t const from{ f.nodes[e.src].rank };
    uint32_t const to{ f.nodes[e.dst].rank };
    if ((to - from) <= 1) {
      out.push_back(e);
      continue;
    }
    uint32_t prev{ e.src };
    for (uint32_t r = from + 1; r < to; ++r) {
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
  f.edges = out;
  f.out = adjacency_of(f.edges, static_cast<uint32_t>(f.nodes.size()), true);
  f.in = adjacency_of(f.edges, static_cast<uint32_t>(f.nodes.size()), false);
}

// Rank buckets in node order, which is document order for the frame's states
// and route order for everything the split contributed.
void bucket_ranks(Frame &f) {
  if (f.nodes.empty()) {
    f.ranks.clear();
    return;
  }
  uint32_t top{ 0 };
  for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
  f.ranks.assign(static_cast<size_t>(top) + 1, {});
  for (uint32_t v = 0; v < f.nodes.size(); ++v) { f.ranks[f.nodes[v].rank].push_back(v); }
  for (std::vector<uint32_t> const &bucket : f.ranks) {
    for (uint32_t i = 0; i < bucket.size(); ++i) { f.nodes[bucket[i]].pos = i; }
  }
}

// The edges leaving rank r, as south positions ordered by their north one,
// which is the form inversion counting wants.
std::vector<uint32_t> south_of(Frame const &f, uint32_t north_rank) {
  struct Pair {
    uint32_t north, south;
  };
  std::vector<Pair> pairs;
  for (uint32_t const v : f.ranks[north_rank]) {
    for (uint32_t k = f.out.off[v]; k < f.out.off[v + 1]; ++k) {
      OrderEdge const &e{ f.edges[f.out.edge[k]] };
      pairs.push_back({ .north = f.nodes[e.src].pos, .south = f.nodes[e.dst].pos });
    }
  }
  scav_stable_sort(pairs, [](Pair const &a, Pair const &b) { return a.north < b.north; });
  std::vector<uint32_t> south;
  south.reserve(pairs.size());
  for (Pair const &pr : pairs) { south.push_back(pr.south); }
  return south;
}

uint64_t total_crossings(Frame const &f) {
  uint64_t total{ 0 };
  for (uint32_t r = 0; (r + 1) < f.ranks.size(); ++r) {
    total += rank_crossings(south_of(f, r));
  }
  return total;
}

// Median of the adjacent positions in the fixed rank; INVALID when the node
// has no neighbour there, which pins it where it is.
uint32_t median_of(Frame const &f, uint32_t node, bool from_predecessors) {
  Adjacency const &a{ from_predecessors ? f.in : f.out };
  std::vector<uint32_t> adj;
  adj.reserve(a.off[node + 1] - a.off[node]);
  for (uint32_t k = a.off[node]; k < a.off[node + 1]; ++k) {
    OrderEdge const &e{ f.edges[a.edge[k]] };
    adj.push_back(f.nodes[from_predecessors ? e.src : e.dst].pos);
  }
  if (adj.empty()) { return INVALID; }
  scav_stable_sort(adj, [](uint32_t a2, uint32_t b2) { return a2 < b2; });
  return adj[adj.size() / 2];
}

// Adjacent exchanges only, and only between two nodes that both have a
// median: a node without one is a barrier that keeps its slot, which is what
// stops an unconstrained node from being swept to an end.
void reorder_rank(Frame &f, uint32_t rank, bool from_predecessors) {
  std::vector<uint32_t> &bucket{ f.ranks[rank] };
  std::vector<uint32_t> med(bucket.size(), INVALID);
  for (uint32_t i = 0; i < bucket.size(); ++i) {
    med[i] = median_of(f, bucket[i], from_predecessors);
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
void minimize_crossings(Frame &f, uint32_t sweeps) {
  if (f.ranks.size() < 2) { return; }
  std::vector<std::vector<uint32_t>> best{ f.ranks };
  uint64_t best_cost{ total_crossings(f) };
  for (uint32_t sweep = 0; (sweep < sweeps) && (best_cost > 0); ++sweep) {
    if ((sweep % 2) == 0) {
      for (uint32_t r = 1; r < f.ranks.size(); ++r) { reorder_rank(f, r, true); }
    } else {
      for (uint32_t r = static_cast<uint32_t>(f.ranks.size()) - 1; r-- > 0;) {
        reorder_rank(f, r, false);
      }
    }
    uint64_t const cost{ total_crossings(f) };
    if (cost < best_cost) {
      best_cost = cost;
      best = f.ranks;
    }
  }
  f.ranks = best;
  for (std::vector<uint32_t> const &bucket : f.ranks) {
    for (uint32_t i = 0; i < bucket.size(); ++i) { f.nodes[bucket[i]].pos = i; }
  }
}

}  // namespace

uint64_t rank_crossings(std::vector<uint32_t> const &south_positions) {
  return inversions(south_positions);
}

SubmachineOrders phase1_order(Chart const &c,
                              SplitGraph const &g,
                              scav_spaces const &s,
                              scav_profile const &p) {
  SubmachineOrders o;
  o.sub_nodes.assign(c.submachines.size(), Span{});
  o.sub_edges.assign(c.submachines.size(), Span{});
  o.sub_ranks.assign(c.submachines.size(), 0);
  o.sub_gaps.assign(c.submachines.size(), Span{});
  o.state_node.assign(c.states.size(), INVALID);
  o.seg_node.assign(g.segments.size(), INVALID);

  // Which segments each frame routes, gathered once: a segment names its frame
  // but a frame does not name its segments.
  std::vector<uint32_t> seg_count(c.submachines.size(), 0);
  for (SplitSegment const &seg : g.segments) {
    if (seg.frame.v != INVALID) { ++seg_count[seg.frame.v]; }
  }
  std::vector<uint32_t> const seg_off{ offsets_of(seg_count) };
  std::vector<uint32_t> frame_segs(g.segments.size(), 0);
  {
    std::vector<uint32_t> fill(seg_off.begin(), seg_off.end() - 1);
    for (uint32_t i = 0; i < g.segments.size(); ++i) {
      SubmachineId const frame{ g.segments[i].frame };
      if (frame.v != INVALID) { frame_segs[fill[frame.v]++] = i; }
    }
  }

  // A label is charged to one rank boundary in one frame -- the middle of the
  // route, which is where a builder draws it -- so a hierarchy-crossing
  // transition does not widen every frame it passes through.
  std::vector<int32_t> seg_label(g.segments.size(), 0);
  for (uint32_t i = 0; i < s.n_path_box; ++i) {
    scav_path_box const &box{ s.path_box[i] };
    if (box.subject >= g.trans_segments.size()) { continue; }
    Span const segs{ g.trans_segments[box.subject] };
    if (segs.len == 0) { continue; }
    seg_label[segs.off + (segs.len / 2)] += box.w;
  }

  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    Frame f;

    Span const kids{ c.submachines[m].children };
    for (uint32_t k = 0; k < kids.len; ++k) {
      uint32_t const child{ c.state_ids[kids.off + k].v };
      if (c.states[child].live == 0) { continue; }
      o.state_node[child] = static_cast<uint32_t>(f.nodes.size());
      f.nodes.push_back(
          { .kind = OrderKind::State, .subject = child, .rank = 0, .pos = 0 });
    }

    // A port on a child's border is that child; a port on the frame's own
    // border is a node of its own, and there is at most one such end per
    // segment because consecutive crossings always change frame.
    auto const boundary_node = [&](uint32_t seg) {
      if (o.seg_node[seg] == INVALID) {
        o.seg_node[seg] = static_cast<uint32_t>(f.nodes.size());
        f.nodes.push_back(
            { .kind = OrderKind::Boundary, .subject = seg, .rank = 0, .pos = 0 });
      }
      return o.seg_node[seg];
    };
    auto const resolve = [&](uint32_t seg, bool is_src) -> uint32_t {
      SplitSegment const &sg{ g.segments[seg] };
      uint32_t const port{ is_src ? sg.src_port : sg.dst_port };
      if (port == INVALID) {
        StateId const st{ endpoint_state(c, sg, is_src) };
        if ((st.v == INVALID) || (c.states[st.v].live == 0)) { return INVALID; }
        return (c.states[st.v].parent.v == m) ? o.state_node[st.v] : boundary_node(seg);
      }
      SplitPort const &pt{ g.ports[port] };
      if (pt.state.v != INVALID) {
        return (c.states[pt.state.v].parent.v == m) ? o.state_node[pt.state.v]
                                                    : boundary_node(seg);
      }
      if (pt.sub.v == m) { return boundary_node(seg); }
      StateId const owner{ c.submachines[pt.sub.v].owner };
      if ((owner.v != INVALID) && (c.states[owner.v].parent.v == m)) {
        return o.state_node[owner.v];
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

    orient_acyclic(f);
    assign_ranks(f);

    // Charged before chaining, while an edge still knows the whole span its
    // label sits in the middle of.
    uint32_t top{ 0 };
    for (OrderNode const &nd : f.nodes) { top = imax(top, nd.rank); }
    std::vector<int32_t> gaps(top, 0);
    for (OrderEdge const &e : f.edges) {
      int32_t const label{ seg_label[e.segment] };
      if (label == 0) { continue; }
      uint32_t const from{ f.nodes[e.src].rank };
      uint32_t const at{ from + ((f.nodes[e.dst].rank - from) / 2) };
      if (at < gaps.size()) { gaps[at] = imax(gaps[at], label); }
    }

    chain_long_edges(f);
    bucket_ranks(f);
    minimize_crossings(f, static_cast<uint32_t>(p.sweep_count));

    uint32_t const node_base{ static_cast<uint32_t>(o.nodes.size()) };
    uint32_t const edge_base{ static_cast<uint32_t>(o.edges.size()) };
    uint32_t const gap_base{ static_cast<uint32_t>(o.gaps.size()) };
    // Emitted in (rank, pos) order, so a consumer walking one frame's nodes
    // walks its diagram left to right and top to bottom.
    std::vector<uint32_t> global(f.nodes.size(), INVALID);
    for (std::vector<uint32_t> const &bucket : f.ranks) {
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
    for (int32_t const gap : gaps) { o.gaps.push_back(gap); }
    for (uint32_t v = 0; v < f.nodes.size(); ++v) {
      OrderNode const &nd{ f.nodes[v] };
      if (nd.kind == OrderKind::State) {
        o.state_node[nd.subject] = global[v];
      } else if (nd.kind == OrderKind::Boundary) {
        o.seg_node[nd.subject] = global[v];
      }
    }

    o.sub_nodes[m] =
        make_span(node_base, static_cast<uint32_t>(o.nodes.size()) - node_base);
    o.sub_edges[m] =
        make_span(edge_base, static_cast<uint32_t>(o.edges.size()) - edge_base);
    o.sub_ranks[m] = static_cast<uint32_t>(f.ranks.size());
    o.sub_gaps[m] = make_span(gap_base, static_cast<uint32_t>(o.gaps.size()) - gap_base);
  }
  return o;
}

}  // namespace scav
