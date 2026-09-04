// One pass per depth level, deepest first: a submachine needs its children sized
// and a state needs its submachines. Ranks give one axis, `cross_coordinates`
// the other; the box formula then sizes the state.

#include "layout/size.h"

#include "layout/coords.h"
#include "layout/pack.h"
#include "scav/scav_core.h"
#include "scav_int.h"

#include <cstdint>
#include <vector>

namespace scav {

namespace {

scav_box_space box_of(scav_box_space const *rows, uint32_t count, uint32_t i) {
  return ((rows != nullptr) && (i < count)) ? rows[i] : scav_box_space{};
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

}  // namespace

bool phase2_size(Chart const &c,
                 SplitGraph const &g,
                 SubmachineOrders const &o,
                 scav_spaces const &s,
                 scav_profile const &p,
                 SizedLayout &out,
                 std::vector<Diagnostic> &diags) {
  out.state.assign(c.states.size(), {});
  out.before.assign(c.states.size(), {});
  out.after.assign(c.states.size(), {});
  out.sub.assign(c.submachines.size(), {});
  out.node.assign(o.nodes.size(), {});
  out.chart = {};

  // Where each submachine sits inside its owner's packed area, which the
  // descent at the end turns into an absolute origin.
  std::vector<scav_point> sub_local(c.submachines.size(), scav_point{});

  uint32_t max_depth{ 0 };
  for (uint32_t const d : g.state_depth) { max_depth = imax(max_depth, d); }
  std::vector<std::vector<uint32_t>> states_at(max_depth + 1);
  std::vector<std::vector<uint32_t>> subs_at(max_depth + 2);
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    if (c.states[i].live != 0) { states_at[g.state_depth[i]].push_back(i); }
  }
  for (uint32_t m = 0; m < c.submachines.size(); ++m) {
    if (c.submachines[m].live == 0) { continue; }
    StateId const owner{ c.submachines[m].owner };
    subs_at[(owner.v == INVALID) ? 0 : g.state_depth[owner.v] + 1].push_back(m);
  }

  bool ok{ true };

  // A frame's graph need not be connected, and unconnected states all rank 0, so
  // one graph would stack them in a column. Components are laid out and packed.
  auto const size_sub = [&](uint32_t m) {
    Span const span{ o.sub_nodes[m] };
    uint32_t const ranks{ o.sub_ranks[m] };
    if ((span.len == 0) || (ranks == 0)) { return; }
    Span const espan{ o.sub_edges[m] };
    Span const gspan{ o.sub_gaps[m] };

    std::vector<uint32_t> adj_count(span.len, 0);
    for (uint32_t k = 0; k < espan.len; ++k) {
      OrderEdge const &e{ o.edges[espan.off + k] };
      ++adj_count[e.src - span.off];
      ++adj_count[e.dst - span.off];
    }
    std::vector<uint32_t> adj_off(size_t{ span.len } + 1, 0);
    for (uint32_t k = 0; k < span.len; ++k) { adj_off[k + 1] = adj_off[k] + adj_count[k]; }
    std::vector<uint32_t> adj(adj_off[span.len], 0);
    {
      std::vector<uint32_t> fill(adj_off.begin(), adj_off.end() - 1);
      for (uint32_t k = 0; k < espan.len; ++k) {
        OrderEdge const &e{ o.edges[espan.off + k] };
        adj[fill[e.src - span.off]++] = e.dst - span.off;
        adj[fill[e.dst - span.off]++] = e.src - span.off;
      }
    }

    // Components in first-node order, and nodes within a component in
    // (rank, pos) order, so both are the reading order the frame was emitted in.
    std::vector<uint32_t> component(span.len, INVALID);
    std::vector<std::vector<uint32_t>> members;
    std::vector<uint32_t> queue;
    for (uint32_t seed = 0; seed < span.len; ++seed) {
      if (component[seed] != INVALID) { continue; }
      uint32_t const id{ static_cast<uint32_t>(members.size()) };
      members.emplace_back();
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
      for (uint32_t k = 0; k < span.len; ++k) {
        if (component[k] == id) { members[id].push_back(k); }
      }
    }

    std::vector<scav_point> local(span.len, scav_point{});
    std::vector<scav_rect> boxes(members.size(), scav_rect{});
    for (uint32_t id = 0; id < members.size(); ++id) {
      std::vector<uint32_t> const &nodes{ members[id] };

      // Ranks renumbered from zero, with the frame's rank kept alongside so a label's
      // gap still lands where it was charged.
      std::vector<uint32_t> global_rank;
      std::vector<uint32_t> local_rank(span.len, INVALID);
      for (uint32_t const k : nodes) {
        uint32_t const rank{ o.nodes[span.off + k].rank };
        if (global_rank.empty() || (global_rank.back() != rank)) {
          global_rank.push_back(rank);
        }
        local_rank[k] = static_cast<uint32_t>(global_rank.size()) - 1;
      }
      uint32_t const layers{ static_cast<uint32_t>(global_rank.size()) };
      std::vector<uint32_t> index(span.len, INVALID);
      std::vector<int32_t> extent(nodes.size(), 0);
      std::vector<int32_t> layer_w(layers, 0);
      std::vector<Wide> layer_h(layers, 0);
      for (uint32_t i = 0; i < nodes.size(); ++i) {
        index[nodes[i]] = i;
        OrderNode const &nd{ o.nodes[span.off + nodes[i]] };
        uint32_t const r{ local_rank[nodes[i]] };
        if (nd.kind == OrderKind::State) {
          extent[i] = out.state[nd.subject].h;
          layer_w[r] = imax(layer_w[r], out.state[nd.subject].w);
        }
        layer_h[r] += extent[i] + p.node_sep;
      }
      auto const boundary_gap = [&](uint32_t r) {
        return (global_rank[r] < gspan.len) ? Wide{ o.gaps[gspan.off + global_rank[r]] }
                                            : Wide{ 0 };
      };

      // A rank run grows unbounded along one axis and nesting multiplies it by depth.
      // Cutting helps only sometimes -- at two ranks it worsens the aspect -- so both
      // shapes are laid out and the scale measure picks, as `trybox` picks a packer.
      struct Shape {
        std::vector<scav_point> at;
        Wide w{ 0 }, h{ 0 };
        bool ok{ true };
      };
      auto const lay_out = [&](Wide wrap_at) {
        Shape shape;
        shape.at.assign(nodes.size(), scav_point{});
        std::vector<uint32_t> chunk_of(layers, 0);
        std::vector<uint32_t> chunks{ 0 };
        Wide run{ 0 };
        for (uint32_t r = 0; r < layers; ++r) {
          Wide const step{ (r == 0)
                               ? Wide{ layer_w[r] }
                               : (Wide{ layer_w[r] } + p.rank_sep + boundary_gap(r - 1)) };
          if ((r > 0) && ((run + step) > wrap_at)) {
            chunks.push_back(r);
            run = layer_w[r];
          } else {
            run += step;
          }
          chunk_of[r] = static_cast<uint32_t>(chunks.size()) - 1;
        }

        std::vector<scav_rect> pieces(chunks.size(), scav_rect{});
        bool fits{ true };
        for (uint32_t chunk = 0; chunk < chunks.size(); ++chunk) {
          uint32_t const first{ chunks[chunk] };
          uint32_t const last{ ((chunk + 1) < chunks.size()) ? chunks[chunk + 1]
                                                             : layers };

          // Only this chunk's nodes and the edges wholly inside it: an edge the cut
          // crosses has its ends in two pieces, as a wrapped line's does.
          CoordGraph cg;
          cg.layers.assign(last - first, {});
          std::vector<uint32_t> chunk_index(nodes.size(), INVALID);
          std::vector<uint32_t> chunk_nodes;
          for (uint32_t i = 0; i < nodes.size(); ++i) {
            uint32_t const r{ local_rank[nodes[i]] };
            if (chunk_of[r] != chunk) { continue; }
            chunk_index[i] = static_cast<uint32_t>(chunk_nodes.size());
            chunk_nodes.push_back(i);
            cg.extent.push_back(extent[i]);
            cg.layers[r - first].push_back(chunk_index[i]);
          }
          for (uint32_t k = 0; k < espan.len; ++k) {
            OrderEdge const &e{ o.edges[espan.off + k] };
            uint32_t const from{ index[e.src - span.off] };
            uint32_t const to{ index[e.dst - span.off] };
            if ((from == INVALID) || (to == INVALID)) { continue; }
            if ((chunk_index[from] == INVALID) || (chunk_index[to] == INVALID)) {
              continue;
            }
            cg.edges.push_back({ .from = chunk_index[from],
                                 .to = chunk_index[to],
                                 .inner = ((o.nodes[e.src].kind == OrderKind::Bend) &&
                                           (o.nodes[e.dst].kind == OrderKind::Bend))
                                              ? 1U
                                              : 0U });
          }
          cg.sep = p.node_sep;
          std::vector<int32_t> const centre{ cross_coordinates(cg) };

          std::vector<Wide> layer_x(last - first, 0);
          for (uint32_t r = first + 1; r < last; ++r) {
            layer_x[r - first] =
                layer_x[r - first - 1] + layer_w[r - 1] + p.rank_sep + boundary_gap(r - 1);
          }
          Wide const chunk_w{ layer_x[last - first - 1] + layer_w[last - 1] };
          Wide chunk_h{ 0 };
          for (uint32_t i = 0; i < chunk_nodes.size(); ++i) {
            // A node's trailing edge, not its centre plus half its extent: an odd extent
            // halves down, so the box would not contain its own rects.
            chunk_h =
                imax(chunk_h, (Wide{ centre[i] } - (cg.extent[i] / 2)) + cg.extent[i]);
          }
          for (uint32_t i = 0; i < chunk_nodes.size(); ++i) {
            uint32_t const at{ chunk_nodes[i] };
            // Local to the piece; the packing below decides where the piece
            // itself goes.
            shape.at[at] = { .x = static_cast<int32_t>(
                                 layer_x[local_rank[nodes[at]] - first]),
                             .y = static_cast<int32_t>(centre[i]) };
          }
          fits = fits && (chunk_w <= COORD_MAX) && (chunk_h <= COORD_MAX);
          if (!fits) { break; }
          pieces[chunk] = { .x = 0,
                            .y = 0,
                            .w = static_cast<int32_t>(chunk_w),
                            .h = static_cast<int32_t>(chunk_h) };
        }
        if (!fits) {
          shape.ok = false;
          return shape;
        }

        // Packed, not stacked: stacking left-aligned gives every piece the width of the
        // widest. They are rectangles sharing an area, which is `pack_lr`'s job (11.4).
        Packing packed{ pack_lr(pieces, p.node_sep, p.dar_num, p.dar_den) };
        if (p.trybox != 0) {
          Packing const row{ pack_box(pieces, p.node_sep) };
          if (pack_better(row, packed, p.dar_num, p.dar_den, p.sm_tiebreak != 0)) {
            packed = row;
          }
        }
        shape.w = packed.w;
        shape.h = packed.h;
        shape.ok = (shape.w <= COORD_MAX) && (shape.h <= COORD_MAX);
        // A saturated position would leave int32 when the offset below added
        // to it, and no caller reads a shape this phase goes on to diagnose.
        if (!shape.ok) { return shape; }
        for (uint32_t i = 0; i < nodes.size(); ++i) {
          scav_rect const &at{ packed.at[chunk_of[local_rank[nodes[i]]]] };
          shape.at[i].x += at.x;
          shape.at[i].y += at.y;
        }
        return shape;
      };

      Wide area{ 0 };
      int32_t widest{ 0 };
      for (uint32_t r = 0; r < layers; ++r) {
        area += (Wide{ layer_w[r] } + p.rank_sep + boundary_gap(r)) * layer_h[r];
        widest = imax(widest, layer_w[r]);
      }
      Wide const target{ imax(Wide{ widest },
                              static_cast<Wide>(isqrt(static_cast<uint64_t>(
                                  floor_div(area * p.dar_num, Wide{ p.dar_den }))))) };

      Shape best{ lay_out(Wide{ COORD_MAX } * 2) };
      Shape const folded{ lay_out(target) };
      bool const swap{ folded.ok &&
                       (!best.ok || pack_better({ .at = {},
                                                  .w = static_cast<int32_t>(folded.w),
                                                  .h = static_cast<int32_t>(folded.h) },
                                                { .at = {},
                                                  .w = static_cast<int32_t>(best.w),
                                                  .h = static_cast<int32_t>(best.h) },
                                                p.dar_num,
                                                p.dar_den,
                                                p.sm_tiebreak != 0)) };
      if (swap) { best = folded; }
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

    Packing packed{ pack_lr(boxes, p.node_sep, p.dar_num, p.dar_den) };
    if (p.trybox != 0) {
      Packing const row{ pack_box(boxes, p.node_sep) };
      if (pack_better(row, packed, p.dar_num, p.dar_den, p.sm_tiebreak != 0)) {
        packed = row;
      }
    }
    if ((packed.w > COORD_MAX) || (packed.h > COORD_MAX)) {
      overflow(diags, ElemKind::Submachine, m);
      ok = false;
      return;
    }
    out.sub[m].w = packed.w;
    out.sub[m].h = packed.h;

    // A boundary node's rank puts it on the frame's border, and the folding and
    // the two packings above leave a piece's own edges mid-frame, so its x comes
    // from the frame: sources where a route arrives, sinks where one leaves.
    std::vector<uint32_t> out_deg(span.len, 0);
    for (uint32_t k = 0; k < espan.len; ++k) {
      ++out_deg[o.edges[espan.off + k].src - span.off];
    }

    for (uint32_t k = 0; k < span.len; ++k) {
      scav_rect const &at{ packed.at[component[k]] };
      OrderNode const &nd{ o.nodes[span.off + k] };
      int32_t const x{ (nd.kind == OrderKind::Boundary)
                           ? ((out_deg[k] != 0) ? 0 : out.sub[m].w)
                           : (local[k].x + at.x) };
      int32_t const y{ local[k].y + at.y };
      out.node[span.off + k] = { .x = x, .y = y };
      if (nd.kind == OrderKind::State) {
        out.state[nd.subject].x = x;
        out.state[nd.subject].y = y - (out.state[nd.subject].h / 2);
      }
    }
  };

  auto const size_state = [&](uint32_t i) {
    std::vector<scav_rect> kids;
    std::vector<uint32_t> ids;
    Span const subs{ c.states[i].submachines };
    for (uint32_t k = 0; k < subs.len; ++k) {
      uint32_t const m{ c.submachine_ids[subs.off + k].v };
      if (c.submachines[m].live == 0) { continue; }
      kids.push_back(out.sub[m]);
      ids.push_back(m);
    }
    Packing packed;
    if (!kids.empty()) {
      packed = pack_lr(kids, p.sub_sep, p.dar_num, p.dar_den);
      if (p.trybox != 0) {
        Packing const row{ pack_box(kids, p.sub_sep) };
        if (pack_better(row, packed, p.dar_num, p.dar_den, p.sm_tiebreak != 0)) {
          packed = row;
        }
      }
      for (uint32_t k = 0; k < ids.size(); ++k) {
        sub_local[ids[k]] = { .x = packed.at[k].x, .y = packed.at[k].y };
      }
    }

    scav_box_space const b{ box_of(s.box_state, s.n_box_state, i) };
    uint32_t const kind{ static_cast<uint32_t>(c.states[i].kind) };
    Wide const ring{ bare_pseudostate(c, out.sub, b, i) ? Wide{ 0 }
                                                        : (2 * static_cast<Wide>(p.pad)) };
    Wide const w{
      imax(imax(Wide{ b.min_w }, Wide{ packed.w }), Wide{ p.kind_min_w[kind] }) + ring
    };
    Wide const h{
      imax(Wide{ b.h_before } + packed.h + b.h_after, Wide{ p.kind_min_h[kind] }) + ring
    };
    if ((w > COORD_MAX) || (h > COORD_MAX)) {
      overflow(diags, ElemKind::State, i);
      ok = false;
      return;
    }
    out.state[i].w = static_cast<int32_t>(w);
    out.state[i].h = static_cast<int32_t>(h);
  };

  // Levels interleave: the submachines whose children sit at this depth, then
  // the states one level up that wrap them; level 0 sizes the document roots.
  for (uint32_t level = max_depth + 2; level-- > 0;) {
    for (uint32_t const m : subs_at[level]) { size_sub(m); }
    if (level > 0) {
      for (uint32_t const i : states_at[level - 1]) { size_state(i); }
    }
  }
  if (!ok) { return false; }

  // One descent from the root, adding each frame's origin. Everything stays inside
  // the sized extents, so int32 cannot leave the domain here.
  struct Frame {
    uint32_t sub;
    int32_t x, y;
  };
  std::vector<Frame> work;
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

}  // namespace scav
