// Brandes & Kopf, with the 2020 erratum's two corrections to horizontal
// compaction applied: a block is placed whole rather than only at its root, so
// no offset is added twice, and class offsets are a separate pass over
// recorded class adjacencies, so a shift accumulates along a critical path
// instead of assuming the current class never moves.

#include "layout/coords.h"

#include "layout/memo.h"
#include "scav/scav_core.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_stable_sort.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace scav {

// Bracketed so the two steps a defect would hide in are reachable from a test
// with a hand-written graph, instead of only through the balanced average of
// four passes. The prototypes a test uses are its own; see scav_internal.h.
SCAV_INTERNAL_BEGIN
std::vector<uint8_t> coords_mark_type1(CoordGraph const &g);
std::vector<int64_t> coords_one_pass(CoordGraph const &g,
                                     std::vector<uint8_t> const &mark,
                                     bool upward,
                                     bool rightward);
SCAV_INTERNAL_END

namespace {

constexpr int64_t SHIFT_INF{ INT64_MAX };

// The graph as one of the four orientations sees it: layers walked in run
// order, nodes walked in run order within a layer, and "upper" meaning the
// neighbour the alignment pass may align onto.
struct View {
  std::vector<std::vector<uint32_t>> layers;
  std::vector<uint32_t> pos, layer;
  std::vector<uint32_t> up_off, up_edge;  // CSR over nodes -> edge indices
};

// Every buffer the five views and four passes of one call use, kept per thread
// and reassigned in place, so a call after the first allocates only its result.
struct Scratch {
  View v;
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

// Every graph this thread has placed, by value. A search scores thousands of
// candidates that differ in one frame, so nearly every graph one of them builds
// another built first -- on bottler, 69k distinct of 19.8M. The key is the
// whole graph, so a hit is the answer the passes would compute. Per thread, so
// no lookup waits on another, and bounded, since a pool thread and what it
// keeps here last as long as the process.
Memo &memo() {
  thread_local Memo m{ size_t{ 1 } << 20 };
  return m;
}

// Every field `cross_coordinates` reads, counts before contents, so two graphs
// with one key are one graph.
void key_of(CoordGraph const &g, std::vector<uint32_t> &key) {
  key.clear();
  key.push_back(static_cast<uint32_t>(g.sep));
  key.push_back(static_cast<uint32_t>(g.extent.size()));
  for (int32_t const e : g.extent) { key.push_back(static_cast<uint32_t>(e)); }
  key.push_back(static_cast<uint32_t>(g.layers.size()));
  for (std::vector<uint32_t> const &lay : g.layers) {
    key.push_back(static_cast<uint32_t>(lay.size()));
    key.insert(key.end(), lay.begin(), lay.end());
  }
  key.push_back(static_cast<uint32_t>(g.edges.size()));
  for (CoordGraph::Edge const &e : g.edges) {
    key.push_back(e.from);
    key.push_back(e.to);
    key.push_back(e.inner);
    key.push_back(static_cast<uint32_t>(e.from_at));
    key.push_back(static_cast<uint32_t>(e.to_at));
    key.push_back(e.weak);
  }
}

// `edge.from` is in the earlier layer, so a downward run aligns `to` onto
// `from` and an upward run does the reverse.
uint32_t upper_of(CoordGraph::Edge const &e, bool upward) {
  return upward ? e.to : e.from;
}
uint32_t lower_of(CoordGraph::Edge const &e, bool upward) {
  return upward ? e.from : e.to;
}

void view_of(CoordGraph const &g, bool upward, bool rightward, Scratch &sc) {
  uint32_t const n{ static_cast<uint32_t>(g.extent.size()) };
  View &v{ sc.v };
  v.layers = g.layers;
  if (upward) {
    for (uint32_t i = 0; i < (v.layers.size() / 2); ++i) {
      v.layers[i].swap(v.layers[v.layers.size() - 1 - i]);
    }
  }
  if (rightward) {
    for (std::vector<uint32_t> &lay : v.layers) {
      for (uint32_t i = 0; i < (lay.size() / 2); ++i) {
        uint32_t const other{ lay[i] };
        lay[i] = lay[lay.size() - 1 - i];
        lay[lay.size() - 1 - i] = other;
      }
    }
  }
  v.pos.assign(n, 0);
  v.layer.assign(n, INVALID);
  for (uint32_t i = 0; i < v.layers.size(); ++i) {
    for (uint32_t k = 0; k < v.layers[i].size(); ++k) {
      v.pos[v.layers[i][k]] = k;
      v.layer[v.layers[i][k]] = i;
    }
  }

  sc.count.assign(n, 0);
  for (CoordGraph::Edge const &e : g.edges) { ++sc.count[lower_of(e, upward)]; }
  v.up_off.assign(size_t{ n } + 1, 0);
  for (uint32_t i = 0; i < n; ++i) { v.up_off[i + 1] = v.up_off[i] + sc.count[i]; }
  v.up_edge.assign(g.edges.size(), 0);
  sc.fill.assign(v.up_off.begin(), v.up_off.end() - 1);
  for (uint32_t i = 0; i < g.edges.size(); ++i) {
    v.up_edge[sc.fill[lower_of(g.edges[i], upward)]++] = i;
  }
  // The medians the alignment step picks are positions in the upper layer, so
  // a node's upper edges have to be in that order and not in edge order. In
  // place over each CSR slice: a copy per node is one allocation per node per
  // pass, and there are four passes over every chunk of every component.
  for (uint32_t i = 0; i < n; ++i) {
    scav_insertion_sort(v.up_edge.data() + v.up_off[i],
                        v.up_edge.data() + v.up_off[i + 1],
                        [&](uint32_t a, uint32_t b) {
                          return v.pos[upper_of(g.edges[a], upward)] <
                                 v.pos[upper_of(g.edges[b], upward)];
                        });
  }
}

// Each node tries its median upper neighbour and then the other median,
// taking the first that is neither a marked segment nor a crossing of what
// this layer has already aligned. `offset` is each node's centre from its
// block's root, which puts the aligned segment's two ends at one coordinate;
// a mirrored run reads the ends' offsets mirrored too.
void align_vertical(CoordGraph const &g,
                    View const &v,
                    std::vector<uint8_t> const &mark,
                    bool upward,
                    bool rightward,
                    std::vector<uint32_t> &root,
                    std::vector<uint32_t> &align,
                    std::vector<int64_t> &offset) {
  uint32_t const n{ static_cast<uint32_t>(g.extent.size()) };
  root.assign(n, 0);
  align.assign(n, 0);
  offset.assign(n, 0);
  for (uint32_t i = 0; i < n; ++i) {
    root[i] = i;
    align[i] = i;
  }
  int64_t const sign{ rightward ? -1 : 1 };
  for (std::vector<uint32_t> const &lay : v.layers) {
    // The rightmost upper neighbour this layer has already aligned onto, which
    // is what keeps two alignments in one layer from crossing. INVALID rather
    // than -1 so the comparison below stays within one signedness.
    uint32_t reached{ INVALID };
    for (uint32_t const node : lay) {
      uint32_t const d{ v.up_off[node + 1] - v.up_off[node] };
      if (d == 0) { continue; }
      std::array<uint32_t, 2> const medians{ (d - 1) / 2, d / 2 };
      // The strong medians first, then the weak ones, so a weak edge anchors
      // its lower end only where nothing else can.
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

// Blocks form a DAG under the alignment and predecessor relations, so placing
// them in topological order is what the erratum's recursion was ensuring.
std::vector<uint32_t> const &block_order(Scratch &sc, uint32_t n) {
  View const &v{ sc.v };
  std::vector<uint32_t> const &root{ sc.root };
  // Predecessor block -> successor block: a block is released once every
  // block it must sit after has been placed.
  std::vector<uint32_t> &pending{ sc.pending };
  std::vector<uint32_t> &succ_off{ sc.succ_off };
  std::vector<uint32_t> &succ{ sc.succ };
  pending.assign(n, 0);
  sc.succ_count.assign(n, 0);
  for (std::vector<uint32_t> const &lay : v.layers) {
    for (uint32_t k = 1; k < lay.size(); ++k) { ++sc.succ_count[root[lay[k - 1]]]; }
  }
  succ_off.assign(size_t{ n } + 1, 0);
  for (uint32_t i = 0; i < n; ++i) { succ_off[i + 1] = succ_off[i] + sc.succ_count[i]; }
  succ.assign(succ_off[n], 0);
  sc.fill.assign(succ_off.begin(), succ_off.end() - 1);
  for (std::vector<uint32_t> const &lay : v.layers) {
    for (uint32_t k = 1; k < lay.size(); ++k) {
      succ[sc.fill[root[lay[k - 1]]]++] = root[lay[k]];
      ++pending[root[lay[k]]];
    }
  }

  std::vector<uint32_t> &order{ sc.order };
  order.clear();
  for (uint32_t i = 0; i < n; ++i) {
    if ((root[i] == i) && (pending[i] == 0)) { order.push_back(i); }
  }
  for (uint32_t at = 0; at < order.size(); ++at) {
    uint32_t const block{ order[at] };
    for (uint32_t k = succ_off[block]; k < succ_off[block + 1]; ++k) {
      if (--pending[succ[k]] == 0) { order.push_back(succ[k]); }
    }
  }
  // A leftover would mean the block relation is not the DAG it is proven to
  // be; place it anyway rather than dropping nodes out of the drawing.
  for (uint32_t i = 0; i < n; ++i) {
    if ((root[i] == i) && (pending[i] != 0)) { order.push_back(i); }
  }
  return order;
}

// The gap two nodes adjacent in one layer need between their centres. `ceil`
// so a halved odd extent never under-separates.
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
  x.assign(n, 0);
  sink.assign(n, 0);
  shift.assign(n, SHIFT_INF);
  for (uint32_t i = 0; i < n; ++i) { sink[i] = i; }

  auto const pred_of = [&](uint32_t node) {
    return (v.pos[node] == 0) ? INVALID : v.layers[v.layer[node]][v.pos[node] - 1];
  };

  // `x[block]` is the root's centre and a member sits `offset` from it, so a
  // left neighbour's own centre is what a member is separated from.
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
    // The first correction: the whole block takes the root's coordinate and
    // class here, so the last pass below adds an offset exactly once.
    while (align[w] != block) {
      w = align[w];
      x[w] = x[block] + offset[w];
      sink[w] = sink[block];
    }
  }

  // The second correction: class adjacencies recorded first, then offsets
  // propagated from the highest class downward, so a shift accumulates.
  // Classes stack diagonally, so the class a pair's right member belongs to is
  // always finished before that pair is read.
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> &neighborings{ sc.neighborings };
  neighborings.resize(v.layers.size());
  for (std::vector<std::pair<uint32_t, uint32_t>> &at : neighborings) { at.clear(); }
  for (std::vector<uint32_t> const &lay : v.layers) {
    for (auto k = static_cast<uint32_t>(lay.size()); k-- > 1;) {
      if (sink[lay[k - 1]] != sink[lay[k]]) {
        neighborings[v.layer[sink[lay[k]]]].emplace_back(lay[k - 1], lay[k]);
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
  mark.assign(g.edges.size(), 0);
  view_of(g, false, false, sc);
  View const &v{ sc.v };
  uint32_t const h{ static_cast<uint32_t>(g.layers.size()) };
  // Layer 0 holds no dummy, so no inner segment starts there and the pair
  // (0, 1) can mark nothing; the walk starts one layer in, as published.
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
  // A rightward run was computed mirrored, so its coordinates come back into
  // the graph's own frame here rather than at every reader.
  if (rightward) {
    for (int64_t &c : x) { c = -c; }
  }
}

}  // namespace

// Only a test calls these, so a build without one has no caller of either.
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

SCAV_INTERNAL_END

namespace {

std::vector<int32_t> place(CoordGraph const &g) {
  uint32_t const n{ static_cast<uint32_t>(g.extent.size()) };
  std::vector<int32_t> out(n, 0);
  if (n == 0) { return out; }

  Scratch &sc{ scratch() };
  mark_type1(g, sc);
  std::array<std::vector<int64_t>, 4> &pass{ sc.pass };
  for (uint32_t k = 0; k < 4; ++k) {
    one_pass(g, sc.mark, (k & 2U) != 0, (k & 1U) != 0, sc, pass[k]);
  }

  std::vector<uint8_t> &placed{ sc.placed };
  placed.assign(n, 0);
  for (std::vector<uint32_t> const &lay : g.layers) {
    for (uint32_t const node : lay) { placed[node] = 1; }
  }

  // Aligned to the narrowest of the four, leftward runs by their leading edge
  // and rightward runs by their trailing one, so the average below cannot
  // drift toward whichever pass happened to come out widest.
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

  // Translated so the leading edge of whatever comes first is zero, which is
  // what makes the result a frame-local extent rather than an offset.
  int32_t least{ COORD_MAX };
  for (uint32_t i = 0; i < n; ++i) {
    if (placed[i] != 0) { least = imin(least, out[i] - (g.extent[i] / 2)); }
  }
  if (least == COORD_MAX) { return out; }
  for (uint32_t i = 0; i < n; ++i) {
    if (placed[i] != 0) { out[i] -= least; }
  }
  return out;
}

}  // namespace

std::vector<int32_t> cross_coordinates(CoordGraph const &g) {
  thread_local std::vector<uint32_t> key;
  key_of(g, key);
  Memo &m{ memo() };
  uint32_t len{ 0 };
  if (int32_t const *const hit{ m.find(key, len) }) { return { hit, hit + len }; }
  std::vector<int32_t> out{ place(g) };
  m.insert(key, out);
  return out;
}

}  // namespace scav
