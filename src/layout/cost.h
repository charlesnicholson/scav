#ifndef SCAV_LAYOUT_COST_H_INCLUDED
#define SCAV_LAYOUT_COST_H_INCLUDED

// The cost vector of 11.6, scored from the phase outputs alone: no layout is
// re-run to obtain one, and a test can hand it two rects and one route.

#include "layout/decompose.h"
#include "layout/route.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

#include <cstdint>
#include <vector>

namespace scav {

// One route segment, with the transition it belongs to and its index in that
// transition's own polyline, which is what the trunk exemption reads.
struct Piece {
  scav_point a, b;
  uint32_t trans;
  uint32_t k;
};

// Entry and exit times from one depth-first walk of the containment forest, so
// a state's descendants are exactly the states whose interval nests inside its
// own and 11.14's carve-out is two comparisons.
struct Ancestry {
  std::vector<uint32_t> tin, tout;  // 0 = the walk never reached the state
  // Live states Tier 0's descent cannot arrive at: one a tombstone stands
  // above, whose zero rect prunes nothing, and one no document root encloses.
  std::vector<uint32_t> detached;
};

// One uniform bucket grid per submachine over its live children. Siblings are
// disjoint by Tier 0, so a cell holds a bounded number of them.
struct ChildGrid {
  struct Frame {
    int32_t x0{ 0 }, y0{ 0 };  // the grid's origin
    Wide cell_w{ 1 }, cell_h{ 1 };
    uint32_t side{ 0 };    // cells per axis
    uint32_t bucket{ 0 };  // -> bucket_off, this frame's first cell
    Span children{};       // -> child
  };
  std::vector<Frame> frame;          // parallel to Chart::submachines
  std::vector<uint32_t> child;       // live child state ordinals, in span order
  std::vector<uint32_t> bucket_off;  // one entry per cell over all frames, plus a tail
  std::vector<uint32_t> bucket_at;   // -> child
};

// Scratch for one grid query. `stamp` marks a child the running query already
// yielded, so a rect covering several cells comes back once.
struct GridQuery {
  std::vector<uint64_t> stamp;  // parallel to ChildGrid::child
  uint64_t epoch{ 0 };
  std::vector<uint32_t> hit;  // -> ChildGrid::child
};

// The nine Tier-2 quantities before weighting, so a test reads one of them
// rather than a sum.
struct CostTerms {
  int64_t bends{ 0 };       // direction changes at a route's interior vertices
  int64_t corridor{ 0 };    // length two routes' segments run collinear over
  int64_t crossings{ 0 };   // properly crossing route segment pairs
  int64_t excess_len{ 0 };  // over min_len, charged per crossing on the edge
  int64_t adjacency{ 0 };   // sibling submachine pairs joined but not adjacent
  // Per placed box: another box, another transition's route, and per state its
  // `before`/`after` bands if it encloses an endpoint, else its whole rect.
  int64_t label{ 0 };
  // Per placed box: how far short of its own height the box falls of being
  // nearer its own route than every other transition's.
  int64_t label_near{ 0 };
  int64_t aspect{ 0 };  // |w * dar_den - h * dar_num|
  int64_t area{ 0 };    // the root bounding box

  // Tier 0, forbidden rather than priced: the obstacle set makes these
  // unrepresentable, and the count survives as a net (11.6).
  int32_t through_box{ 0 };
  int32_t box_overlap{ 0 };
};

// Compared lexicographically, in this order.
struct Cost {
  int32_t t0_violations{ 0 };
  int64_t t1_hints{ 0 };
  int64_t t2{ 0 };
};

CostTerms cost_terms(Chart const &c,
                     SplitGraph const &g,
                     SizedLayout const &z,
                     Routes const &r,
                     scav_spaces const &s,
                     scav_profile const &p);

// The same scoring from the geometry columns, so one build scores another's
// output. The placed boxes are an out-param of the run, so they come back in.
CostTerms cost_columns(Chart const &c,
                       SplitGraph const &g,
                       scav_profile const &p,
                       scav_spaces const &s = {},
                       std::vector<scav_rect> const &placed = {});

// Every weight is capped at 2^10 and area at 2^40, so the sum stays inside
// int64 by construction rather than by measurement (11.6).
Cost cost_of(CostTerms const &t, scav_profile const &p);

bool cost_less(Cost const &a, Cost const &b);

}  // namespace scav

#endif  // SCAV_LAYOUT_COST_H_INCLUDED
