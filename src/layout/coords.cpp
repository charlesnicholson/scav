// Brandes & Kopf with the 2020 erratum's two horizontal-compaction corrections: blocks
// are placed whole, and class shifts propagate in a separate pass.

#include "layout/coords.h"

#include "layout/memo.h"
#include "scav/scav_core.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_stable_sort.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace scav {

// Test entry points: type-1 marking and one of the four passes.
SCAV_INTERNAL_BEGIN
std::vector<uint8_t> coords_mark_type1(CoordGraph const &g);
std::vector<int64_t> coords_one_pass(CoordGraph const &g,
                                     std::vector<uint8_t> const &mark,
                                     bool upward,
                                     bool rightward);
// `cross_coordinates` without the memo.
std::vector<int32_t> coords_place(CoordGraph const &g);
SCAV_INTERNAL_END

namespace {

constexpr int64_t SHIFT_INF{ INT64_MAX };

// The graph in one orientation: layers and nodes in run order; "upper" is the neighbour
// the alignment pass may align onto.
struct View {
  std::vector<std::vector<uint32_t>> layers;
  std::vector<uint32_t> pos, layer;
  std::vector<uint32_t> up_off, up_edge;  // CSR over nodes -> edge indices
};

// Per-thread buffers for one call's five views and four passes, reassigned in place.
struct Scratch {
  View v;
  std::vector<std::vector<uint32_t>> spare;  // layers a smaller graph's view dropped
  std::vector<uint32_t> count, fill, root, align, sink;
  std::vector<uint32_t> pending, succ_count, succ_off, succ, order;
  std::vector<int64_t> offset, shift;
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> neighborings;
  std::vector<uint8_t> mark, placed;
  std::array<std::vector<int64_t>, 4> pass;
};

Scratch &scratch() {
  thread_local Scratch s;
  return s;
}

// Per-thread memo of placed graphs, keyed by the whole graph; holds up to 2^20 words.
Memo &memo() {
  thread_local Memo m{ size_t{ 1 } << 20 };
  return m;
}

// An injective key: every field `cross_coordinates` reads, lengths before contents.
void key_of(CoordGraph const &g, std::vector<uint32_t> &key) {
  key.clear();
  vec_push_back(key, static_cast<uint32_t>(g.sep));
  vec_push_back(key, static_cast<uint32_t>(g.extent.size()));
  for (int32_t const e : g.extent) { vec_push_back(key, static_cast<uint32_t>(e)); }
  vec_push_back(key, static_cast<uint32_t>(g.layers.size()));
  for (std::vector<uint32_t> const &lay : g.layers) {
    vec_push_back(key, static_cast<uint32_t>(lay.size()));
    vec_insert(key, key.end(), lay.begin(), lay.end());
  }
  vec_push_back(key, static_cast<uint32_t>(g.edges.size()));
  for (CoordGraph::Edge const &e : g.edges) {
    vec_push_back(key, e.from);
    vec_push_back(key, e.to);
    vec_push_back(key, e.inner);
    vec_push_back(key, static_cast<uint32_t>(e.from_at));
    vec_push_back(key, static_cast<uint32_t>(e.to_at));
    vec_push_back(key, e.weak);
  }
}

// The end a run aligns onto: `from` running downward, `to` running upward.
uint32_t upper_of(CoordGraph::Edge const &e, bool upward) {
  return upward ? e.to : e.from;
}
uint32_t lower_of(CoordGraph::Edge const &e, bool upward) {
  return upward ? e.from : e.to;
}

void view_of(CoordGraph const &g, bool upward, bool rightward, Scratch &sc) {
  uint32_t const n{ static_cast<uint32_t>(g.extent.size()) };
  View &v{ sc.v };
  size_t const count{ g.layers.size() };
  while (v.layers.size() > count) {  // parks the storage in `spare`
    vec_push_back(sc.spare, std::move(v.layers.back()));
    v.layers.pop_back();
  }
  while ((v.layers.size() < count) && !sc.spare.empty()) {
    vec_push_back(v.layers, std::move(sc.spare.back()));
    sc.spare.pop_back();
  }
  vec_resize(v.layers, count);
  for (size_t i = 0; i < count; ++i) {
    std::vector<uint32_t> const &from{ g.layers[upward ? (count - 1 - i) : i] };
    if (rightward) {
      vec_assign(v.layers[i], from.rbegin(), from.rend());
    } else {
      vec_assign(v.layers[i], from.begin(), from.end());
    }
  }
  vec_assign(v.pos, n, 0);
  vec_assign(v.layer, n, INVALID);
  for (uint32_t i = 0; i < v.layers.size(); ++i) {
    for (uint32_t k = 0; k < v.layers[i].size(); ++k) {
      v.pos[v.layers[i][k]] = k;
      v.layer[v.layers[i][k]] = i;
    }
  }

  vec_assign(sc.count, n, 0);
  for (CoordGraph::Edge const &e : g.edges) { ++sc.count[lower_of(e, upward)]; }
  vec_assign(v.up_off, size_t{ n } + 1, 0);
  for (uint32_t i = 0; i < n; ++i) { v.up_off[i + 1] = v.up_off[i] + sc.count[i]; }
  vec_assign(v.up_edge, g.edges.size(), 0);
  vec_assign(sc.fill, v.up_off.begin(), v.up_off.end() - 1);
  for (uint32_t i = 0; i < g.edges.size(); ++i) {
    v.up_edge[sc.fill[lower_of(g.edges[i], upward)]++] = i;
  }
  // Sorts each node's upper edges in place by the upper end's position.
  for (uint32_t i = 0; i < n; ++i) {
    scav_insertion_sort(v.up_edge.data() + v.up_off[i],
                        v.up_edge.data() + v.up_off[i + 1],
                        [&](uint32_t a, uint32_t b) {
                          return v.pos[upper_of(g.edges[a], upward)] <
                                 v.pos[upper_of(g.edges[b], upward)];
                        });
  }
}

// Aligns each node to its first unmarked median upper neighbour past the layer's last
// alignment. `offset`: a node's centre from its block root's, mirrored when `rightward`.
void align_vertical(CoordGraph const &g,
                    View const &v,
                    std::vector<uint8_t> const &mark,
                    bool upward,
                    bool rightward,
                    std::vector<uint32_t> &root,
                    std::vector<uint32_t> &align,
                    std::vector<int64_t> &offset) {
  uint32_t const n{ static_cast<uint32_t>(g.extent.size()) };
  vec_assign(root, n, 0);
  vec_assign(align, n, 0);
  vec_assign(offset, n, 0);
  for (uint32_t i = 0; i < n; ++i) {
    root[i] = i;
    align[i] = i;
  }
  int64_t const sign{ rightward ? -1 : 1 };
  for (std::vector<uint32_t> const &lay : v.layers) {
    // Position of the rightmost upper neighbour this layer aligned onto; INVALID if none.
    uint32_t reached{ INVALID };
    for (uint32_t const node : lay) {
      uint32_t const d{ v.up_off[node + 1] - v.up_off[node] };
      if (d == 0) { continue; }
      std::array<uint32_t, 2> const medians{ (d - 1) / 2, d / 2 };
      // Round 0 tries the strong medians, round 1 the weak ones.
      for (uint32_t const round : { 0U, 1U }) {
        for (uint32_t const m : medians) {
          if (align[node] != node) { continue; }
          uint32_t const edge{ v.up_edge[v.up_off[node] + m] };
          uint32_t const up{ upper_of(g.edges[edge], upward) };
          if ((g.edges[edge].weak != round) || (mark[edge] != 0) ||
              ((reached != INVALID) && (reached >= v.pos[up]))) {
            continue;
          }
          CoordGraph::Edge const &e{ g.edges[edge] };
          int64_t const up_at{ upward ? e.to_at : e.from_at };
          int64_t const node_at{ upward ? e.from_at : e.to_at };
          align[up] = node;
          root[node] = root[up];
          align[node] = root[node];
          offset[node] = offset[up] + (sign * (up_at - node_at));
          reached = v.pos[up];
        }
      }
    }
  }
}

// Block roots in topological order of the left-neighbour relation between blocks.
std::vector<uint32_t> const &block_order(Scratch &sc, uint32_t n) {
  View const &v{ sc.v };
  std::vector<uint32_t> const &root{ sc.root };
  // CSR of predecessor block -> successor blocks; `pending` counts unplaced predecessors.
  std::vector<uint32_t> &pending{ sc.pending };
  std::vector<uint32_t> &succ_off{ sc.succ_off };
  std::vector<uint32_t> &succ{ sc.succ };
  vec_assign(pending, n, 0);
  vec_assign(sc.succ_count, n, 0);
  for (std::vector<uint32_t> const &lay : v.layers) {
    for (uint32_t k = 1; k < lay.size(); ++k) { ++sc.succ_count[root[lay[k - 1]]]; }
  }
  vec_assign(succ_off, size_t{ n } + 1, 0);
  for (uint32_t i = 0; i < n; ++i) { succ_off[i + 1] = succ_off[i] + sc.succ_count[i]; }
  vec_assign(succ, succ_off[n], 0);
  vec_assign(sc.fill, succ_off.begin(), succ_off.end() - 1);
  for (std::vector<uint32_t> const &lay : v.layers) {
    for (uint32_t k = 1; k < lay.size(); ++k) {
      succ[sc.fill[root[lay[k - 1]]]++] = root[lay[k]];
      ++pending[root[lay[k]]];
    }
  }

  std::vector<uint32_t> &order{ sc.order };
  order.clear();
  for (uint32_t i = 0; i < n; ++i) {
    if ((root[i] == i) && (pending[i] == 0)) { vec_push_back(order, i); }
  }
  for (uint32_t at = 0; at < order.size(); ++at) {
    uint32_t const block{ order[at] };
    for (uint32_t k = succ_off[block]; k < succ_off[block + 1]; ++k) {
      if (--pending[succ[k]] == 0) { vec_push_back(order, succ[k]); }
    }
  }
  // Appends any root still pending; every block is placed even if the relation cycles.
  for (uint32_t i = 0; i < n; ++i) {
    if ((root[i] == i) && (pending[i] != 0)) { vec_push_back(order, i); }
  }
  return order;
}

// Least centre-to-centre gap between two neighbours in one layer, rounded up.
int32_t sep_between(CoordGraph const &g, uint32_t left, uint32_t right) {
  return ceil_div(g.extent[left] + g.extent[right], 2) + g.sep;
}

void compact(CoordGraph const &g, Scratch &sc, std::vector<int64_t> &x) {
  uint32_t const n{ static_cast<uint32_t>(g.extent.size()) };
  View const &v{ sc.v };
  std::vector<uint32_t> const &root{ sc.root };
  std::vector<uint32_t> const &align{ sc.align };
  std::vector<int64_t> const &offset{ sc.offset };
  std::vector<uint32_t> &sink{ sc.sink };
  std::vector<int64_t> &shift{ sc.shift };
  vec_assign(x, n, 0);
  vec_assign(sink, n, 0);
  vec_assign(shift, n, SHIFT_INF);
  for (uint32_t i = 0; i < n; ++i) { sink[i] = i; }

  auto const pred_of = [&](uint32_t node) {
    return (v.pos[node] == 0) ? INVALID : v.layers[v.layer[node]][v.pos[node] - 1];
  };

  // `x[block]` is the root's centre and member `w` sits `offset[w]` from it; each member
  // keeps `sep_between` from its left neighbour in the same class.
  for (uint32_t const block : block_order(sc, n)) {
    uint32_t w{ block };
    do {
      uint32_t const left{ pred_of(w) };
      if (left != INVALID) {
        uint32_t const u{ root[left] };
        if (sink[block] == block) { sink[block] = sink[u]; }
        if (sink[block] == sink[u]) {
          x[block] = imax(x[block], (x[left] + sep_between(g, left, w)) - offset[w]);
        }
      }
      w = align[w];
    } while (w != block);
    // First correction: each member takes `x[block] + offset` and the root's class.
    while (align[w] != block) {
      w = align[w];
      x[w] = x[block] + offset[w];
      sink[w] = sink[block];
    }
  }

  // Second correction: records class adjacencies by the sink layer of the right class,
  // then propagates shifts in layer order; a right class's shift is final when read.
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> &neighborings{ sc.neighborings };
  vec_resize(neighborings, v.layers.size());
  for (std::vector<std::pair<uint32_t, uint32_t>> &at : neighborings) { at.clear(); }
  for (std::vector<uint32_t> const &lay : v.layers) {
    for (auto k = static_cast<uint32_t>(lay.size()); k-- > 1;) {
      if (sink[lay[k - 1]] != sink[lay[k]]) {
        vec_emplace_back(neighborings[v.layer[sink[lay[k]]]], lay[k - 1], lay[k]);
      }
    }
  }
  for (uint32_t i = 0; i < v.layers.size(); ++i) {
    if (!v.layers[i].empty()) {
      uint32_t const first{ sink[v.layers[i][0]] };
      if (shift[first] == SHIFT_INF) { shift[first] = 0; }
    }
    for (std::pair<uint32_t, uint32_t> const &pair : neighborings[i]) {
      uint32_t const left{ pair.first };
      uint32_t const right{ pair.second };
      int64_t const base{ (shift[sink[right]] == SHIFT_INF) ? 0 : shift[sink[right]] };
      int64_t const want{ base + x[right] - (x[left] + sep_between(g, left, right)) };
      shift[sink[left]] = imin(shift[sink[left]], want);
    }
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (shift[sink[i]] != SHIFT_INF) { x[i] += shift[sink[i]]; }
  }
}

void mark_type1(CoordGraph const &g, Scratch &sc) {
  std::vector<uint8_t> &mark{ sc.mark };
  vec_assign(mark, g.edges.size(), 0);
  view_of(g, false, false, sc);
  View const &v{ sc.v };
  uint32_t const h{ static_cast<uint32_t>(g.layers.size()) };
  // Starts at layer 1, as published; layer 0 holds no dummy.
  for (uint32_t i = 1; (i + 1) < h; ++i) {
    std::vector<uint32_t> const &next{ g.layers[i + 1] };
    if (next.empty() || g.layers[i].empty()) { continue; }
    uint32_t k0{ 0 };
    uint32_t l{ 0 };
    for (uint32_t l1 = 0; l1 < next.size(); ++l1) {
      uint32_t const node{ next[l1] };
      uint32_t k1{ static_cast<uint32_t>(g.layers[i].size()) - 1 };
      bool inner{ false };
      for (uint32_t k = v.up_off[node]; k < v.up_off[node + 1]; ++k) {
        CoordGraph::Edge const &e{ g.edges[v.up_edge[k]] };
        if (e.inner != 0) {
          inner = true;
          k1 = v.pos[e.from];
        }
      }
      if (((l1 + 1) != next.size()) && !inner) { continue; }
      while (l <= l1) {
        uint32_t const at{ next[l] };
        for (uint32_t k = v.up_off[at]; k < v.up_off[at + 1]; ++k) {
          uint32_t const up{ v.pos[g.edges[v.up_edge[k]].from] };
          if ((up < k0) || (up > k1)) { mark[v.up_edge[k]] = 1; }
        }
        ++l;
      }
      k0 = k1;
    }
  }
}

void one_pass(CoordGraph const &g,
              std::vector<uint8_t> const &mark,
              bool upward,
              bool rightward,
              Scratch &sc,
              std::vector<int64_t> &x) {
  view_of(g, upward, rightward, sc);
  align_vertical(g, sc.v, mark, upward, rightward, sc.root, sc.align, sc.offset);
  compact(g, sc, x);
  // Negates a rightward run's mirrored coordinates back into the graph's frame.
  if (rightward) {
    for (int64_t &c : x) { c = -c; }
  }
}

}  // namespace

namespace {

// `coords_place`, written into `out`.
void place_into(CoordGraph const &g, std::vector<int32_t> &out) {
  uint32_t const n{ static_cast<uint32_t>(g.extent.size()) };
  vec_assign(out, n, 0);
  if (n == 0) { return; }

  Scratch &sc{ scratch() };
  mark_type1(g, sc);
  std::array<std::vector<int64_t>, 4> &pass{ sc.pass };
  for (uint32_t k = 0; k < 4; ++k) {
    one_pass(g, sc.mark, (k & 2U) != 0, (k & 1U) != 0, sc, pass[k]);
  }

  std::vector<uint8_t> &placed{ sc.placed };
  vec_assign(placed, n, 0);
  for (std::vector<uint32_t> const &lay : g.layers) {
    for (uint32_t const node : lay) { placed[node] = 1; }
  }

  // Shifts each pass onto the narrowest: leftward runs align leading edges, rightward
  // runs trailing edges.
  uint32_t narrowest{ 0 };
  std::array<int64_t, 4> lo{};
  std::array<int64_t, 4> hi{};
  for (uint32_t k = 0; k < 4; ++k) {
    bool first{ true };
    for (uint32_t i = 0; i < n; ++i) {
      if (placed[i] == 0) { continue; }
      int64_t const half{ g.extent[i] / 2 };
      lo[k] = first ? (pass[k][i] - half) : imin(lo[k], pass[k][i] - half);
      hi[k] = first ? (pass[k][i] + half) : imax(hi[k], pass[k][i] + half);
      first = false;
    }
    if ((hi[k] - lo[k]) < (hi[narrowest] - lo[narrowest])) { narrowest = k; }
  }
  for (uint32_t k = 0; k < 4; ++k) {
    int64_t const delta{ ((k & 1U) != 0) ? (hi[narrowest] - hi[k])
                                         : (lo[narrowest] - lo[k]) };
    for (int64_t &c : pass[k]) { c += delta; }
  }

  for (uint32_t i = 0; i < n; ++i) {
    if (placed[i] == 0) { continue; }
    std::array<int64_t, 4> four{ pass[0][i], pass[1][i], pass[2][i], pass[3][i] };
    scav_insertion_sort(four.data(), four.data() + four.size(), [](int64_t a, int64_t b) {
      return a < b;
    });
    int64_t const centre{ floor_div(four[1] + four[2], int64_t{ 2 }) };
    out[i] = static_cast<int32_t>(
        imin(imax(centre, int64_t{ COORD_MIN }), int64_t{ COORD_MAX }));
  }

  // Translates so the least leading edge of any placed node is zero.
  int32_t least{ COORD_MAX };
  for (uint32_t i = 0; i < n; ++i) {
    if (placed[i] != 0) { least = imin(least, out[i] - (g.extent[i] / 2)); }
  }
  if (least == COORD_MAX) { return; }
  for (uint32_t i = 0; i < n; ++i) {
    if (placed[i] != 0) { out[i] -= least; }
  }
}

}  // namespace

SCAV_INTERNAL_BEGIN

[[maybe_unused]] std::vector<uint8_t> coords_mark_type1(CoordGraph const &g) {
  Scratch sc;
  mark_type1(g, sc);
  return sc.mark;
}

[[maybe_unused]] std::vector<int64_t> coords_one_pass(CoordGraph const &g,
                                                      std::vector<uint8_t> const &mark,
                                                      bool upward,
                                                      bool rightward) {
  Scratch sc;
  std::vector<int64_t> x;
  one_pass(g, mark, upward, rightward, sc, x);
  return x;
}

[[maybe_unused]] std::vector<int32_t> coords_place(CoordGraph const &g) {
  std::vector<int32_t> out;
  place_into(g, out);
  return out;
}

SCAV_INTERNAL_END

void cross_coordinates(CoordGraph const &g, std::vector<int32_t> &out) {
  thread_local std::vector<uint32_t> key;
  key_of(g, key);
  Memo &m{ memo() };
  int32_t const *hit{ nullptr };
  uint32_t len{ 0 };
  if (m.find(key, hit, len)) {
    vec_assign(out, hit, hit + len);
    return;
  }
  place_into(g, out);
  m.insert(key, out);
}

}  // namespace scav
