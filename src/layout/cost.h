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
