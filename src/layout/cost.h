#ifndef SCAV_LAYOUT_COST_H_INCLUDED
#define SCAV_LAYOUT_COST_H_INCLUDED

// Scores the cost vector from the phase outputs alone. The terms, reducers and
// `layout_cost` are public, in scav_layout.h.

#include "layout/decompose.h"
#include "layout/route.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_vector.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

// Segment `k` of transition `trans`'s route, from `a` to `b`.
struct Piece {
  scav_point a, b;
  uint32_t trans;
  uint32_t k;
};

// Entry and exit times from one DFS of the containment forest; a state's descendants are
// the states whose interval nests inside its own.
struct Ancestry {
  std::vector<uint32_t> tin, tout;  // 0 = the walk never reached the state
  // Live states the Tier 0 descent cannot reach: those under a dead state and those no
  // document root encloses.
  std::vector<uint32_t> detached;
};

// One uniform bucket grid per submachine over its live children.
struct ChildGrid {
  struct Frame {
    int32_t x0{ 0 }, y0{ 0 };  // the grid's origin
    Wide cell_w{ 1 }, cell_h{ 1 };
    uint32_t side{ 0 };    // cells per axis; 0 for a frame scanned without cells
    uint32_t bucket{ 0 };  // -> bucket_off, this frame's first cell
    Span children{};       // -> child
  };
  std::vector<Frame> frame;          // parallel to Chart::submachines
  std::vector<uint32_t> child;       // live child state ordinals, in span order
  std::vector<uint32_t> bucket_off;  // one entry per cell over all frames, plus a tail
  std::vector<uint32_t> bucket_at;   // -> child
};

// Scratch for grid queries. `hit` lists each child once; `stamp` holds the `epoch` of the
// query that last yielded it.
struct GridQuery {
  std::vector<uint64_t> stamp;  // parallel to ChildGrid::child
  uint64_t epoch{ 0 };
  std::vector<uint32_t> hit;  // -> ChildGrid::child
};

// What scoring reads of the chart alone; built once per chart and read concurrently by
// every candidate.
struct CostContext {
  Ancestry an;
  ChildGrid grid;  // frames and children only; each candidate fills a per-thread copy
  // Per transition, src end then dst: the lowest common ancestor's child on that end's
  // chain where it lies strictly above the end's enclosing state, else INVALID.
  std::vector<std::array<uint32_t, 2>> transit_top;
};

CostContext cost_context(Chart const &c, SplitGraph const &g);

// Where `cost_terms` may stop: once its Tier 2 terms less the labels', with aspect only
// where `aspect`, reach `t2`, it sets `stopped` and counts no more.
struct CostStop {
  int64_t t2{ 0 };
  bool aspect{ true };
  bool stopped{ false };
};

// `ctx` is `cost_context(c, g)`. Per transition, `party` gets 1 where a term charges it,
// all 1 on a Tier 0 violation, and `charge` the em-scaled Tier 2 it is charged.
CostTerms cost_terms(CostContext const &ctx,
                     Chart const &c,
                     SplitGraph const &g,
                     SizedLayout const &z,
                     Routes const &r,
                     scav_spaces const &s,
                     scav_profile const &p,
                     std::vector<uint8_t> *party = nullptr,
                     std::vector<Wide> *charge = nullptr,
                     CostStop *stop = nullptr);

// The same with a context built for this one call.
CostTerms cost_terms(Chart const &c,
                     SplitGraph const &g,
                     SizedLayout const &z,
                     Routes const &r,
                     scav_spaces const &s,
                     scav_profile const &p);

// Per term, the least any routing of `z` by `router` with Tier 0 zero and no degraded net
// scores, box ends on their seats' faces where `seated`; `faces` per `box_faces`.
CostTerms cost_bound(Chart const &c,
                     SplitGraph const &g,
                     std::vector<std::vector<uint32_t>> const &bends,  // `segment_bends`
                     SizedLayout const &z,
                     Vector<uint32_t> const &faces,
                     scav_profile const &p,
                     int32_t clear,
                     Router const &router,
                     bool seated = true,
                     std::vector<int32_t> *per_trans = nullptr);  // bends per transition

// The same scoring read from the geometry columns; `placed` is the run's placed-box
// out-param.
CostTerms cost_columns(Chart const &c,
                       SplitGraph const &g,
                       scav_profile const &p,
                       scav_spaces const &s = {},
                       std::vector<scav_rect> const &placed = {});

}  // namespace scav

#endif  // SCAV_LAYOUT_COST_H_INCLUDED
