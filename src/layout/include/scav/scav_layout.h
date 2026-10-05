#ifndef SCAV_LAYOUT_H_INCLUDED
#define SCAV_LAYOUT_H_INCLUDED

// libscavlayout's C++ API over the C ABI's PODs: space tables, profiles, layout, cost
// and the router registry.

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

// Space requests ============================================================

inline constexpr int32_t SPACE_MAX{ COORD_MAX / 4 };  // bound on every space request

// Checks every row of every table against the domain, appending findings sorted by
// (code, kind, ordinal). False when it found any.
bool spaces_validate(Chart const &c, scav_spaces const &s, std::vector<Diagnostic> &diags);

// xxh32 over each table's count and rows, field by field; strides are excluded.
uint32_t spaces_digest(scav_spaces const &s);

// Profile ===================================================================

// Fills `out` from a shipped profile, "compact" or "readable". False for an
// unknown name, writing nothing.
bool profile_named(char const *name, scav_profile &out);

// True when every field is within the bound its `scav_profile` comment gives.
bool profile_validate(scav_profile const &p);

// Drawn silhouette =========================================================

// Corner radius of a `Normal` state: an eighth of the box's shorter side, capped at
// `pad`. Zero for every other kind.
inline int32_t state_corner_radius(StateKind kind, scav_rect const &box, int32_t pad) {
  if (kind != StateKind::Normal) { return 0; }
  int32_t const proportional{ ((box.w < box.h) ? box.w : box.h) / 8 };
  return (proportional < pad) ? proportional : pad;
}

// Distance from a label's anchor on its route to the attachment point on its placed
// `scav_path_box`: half an em, at least 1.
inline int32_t label_leader(scav_profile const &p) {
  return (p.font_size_grid / 2 > 1) ? (p.font_size_grid / 2) : 1;
}

// ceil(font_size_grid * k_num / k_den), or 0 outside the profile's domain or past
// COORD_MAX. Equals `scav_draw.h`'s `line_height`.
inline int32_t label_line_height(scav_profile const &p) {
  if ((p.font_size_grid <= 0) || (p.line_height_k_num < 1) ||
      (p.line_height_k_num > 1024) || (p.line_height_k_den < 1) ||
      (p.line_height_k_den > 1024)) {
    return 0;
  }
  int64_t const h{ ((int64_t{ p.font_size_grid } * p.line_height_k_num) +
                    p.line_height_k_den - 1) /
                   p.line_height_k_den };
  return (h > COORD_MAX) ? 0 : static_cast<int32_t>(h);
}

// Derived spacing ===========================================================

// The room a route keeps from another route: lanes, seats on one face, a loop's legs.
constexpr int32_t route_clearance(scav_profile const &p) {
  return (p.node_sep / 3 > 1) ? (p.node_sep / 3) : 1;
}

// Width of the band either side of a state's border that a route running along that
// border keeps out of: one `pad`, at least 1.
constexpr int32_t border_band(scav_profile const &p) { return (p.pad > 1) ? p.pad : 1; }

// The room a route keeps from a box it passes: `border_band`, at least `route_clearance`.
constexpr int32_t box_clearance(scav_profile const &p) {
  return (border_band(p) > route_clearance(p)) ? border_band(p) : route_clearance(p);
}

// A route's cost per bend, in units of length: one rank separation.
constexpr int64_t route_bend_penalty(scav_profile const &p) {
  return (p.rank_sep > 1) ? p.rank_sep : 1;
}

int32_t loop_reach(scav_profile const &p);  // the far leg's distance in from the exit face
int32_t loop_gap(scav_profile const &p);    // label to far leg, at most `label_leader`
int32_t loop_lane(scav_profile const &p);   // the least lane between the loop's two legs

// A run `[lo, lo + len]` along one face of an obstacle (0 left, 1 right, 2 top, 3 bottom)
// whose interior no end seats in.
struct OccupiedSpan {
  uint32_t obstacle, face;
  int32_t lo, len;
};

// Per inner loop in a laid-out chart's geometry columns, its ends' run on each face of its
// state they touch, padded by `route_clearance`; `obstacle` is the StateId ordinal.
void layout_occupied_spans(Chart const &c,
                           scav_profile const &p,
                           std::vector<OccupiedSpan> &out);

// Layout ====================================================================

// Holds `state` at `rank` in phase 1, in place of the rank longest path gives it.
struct RankPin {
  StateId state{ INVALID };
  uint32_t rank{ 0 };
};

// Leaves leg `leg` of `trans` unchained in phase 1, so it routes with no waypoints.
// `leg` indexes the transition's segments; 0 for one that crosses no boundary.
struct ChainCut {
  TransId trans{ INVALID };
  uint32_t leg{ 0 };
};

// Reverses leg `leg` of `trans` before phase 1 breaks cycles.
struct ReversePin {
  TransId trans{ INVALID };
  uint32_t leg{ 0 };
};

// Puts end `end` (0 departure, 1 arrival) of leg `leg` of `trans` on face 0 left, 1 right,
// 2 top or 3 bottom: of its box at a box end, of its state's border at a port end.
struct EndPin {
  TransId trans{ INVALID };
  uint32_t leg{ 0 };
  uint32_t end{ 0 };
  uint32_t face{ 0 };
};

// Lays out `frame` with ranks running down the page: layers are rows, the cross axis
// is horizontal, and its ports sit on its top and bottom borders.
struct OrientPin {
  SubmachineId frame{ INVALID };
};

// Overrides the row's fold rule for `frame`: `mode` FOLD_SCALE lets the scale measure
// decide; a nonzero `layer` is the one rank the fold cuts before.
inline constexpr uint32_t FOLD_SCALE{ 0 };
inline constexpr uint32_t FOLD_ALWAYS{ 1 };
inline constexpr uint32_t FOLD_NEVER{ 2 };
struct FoldPin {
  SubmachineId frame{ INVALID };
  uint32_t mode{ FOLD_SCALE };
  uint32_t layer{ 0 };
};

// Places `state`'s loop room on face `face` (0 left, 1 right, 2 top, 3 bottom) of its free
// interior, at end `end` of that face: 0 leading (top or left), 1 trailing.
struct LoopPin {
  StateId state{ INVALID };
  uint32_t face{ 0 };
  uint32_t end{ 0 };
};

// Every input besides the tuple that a drawing depends on.
struct SearchPins {
  std::vector<RankPin> ranks;
  std::vector<ChainCut> cuts;
  std::vector<ReversePin> reverses;
  std::vector<EndPin> ends;
  std::vector<OrientPin> orients;
  std::vector<FoldPin> folds;  // the last pin naming a frame decides it
  std::vector<LoopPin> loops;  // the last pin naming a state decides it
};

// Rows in the Level 2 table of phase-2 tuples, one per combination of box packer,
// compaction, ratio source and fold; bounds `portfolio_m` and `layout_run`'s `row`.
inline constexpr uint32_t LAYOUT_SEARCH_ROWS{ 16 };

// Lays out and searches the first `portfolio_m` table rows; keeps the lowest `Cost`.
// Writes the geometry columns and sizes `placed` to the path boxes. False leaves the
// last successful run's columns; true may leave one RouteDegraded finding in `diags`
// per transition drawn as a straight line.
// `inflations`: spacing inflations the written geometry took.
// `tuple`: the table row that produced it; row 0 is the profile as passed.
// `row`: runs that row alone, ignoring `portfolio_m`; `INVALID` searches, any other
// value must be below `LAYOUT_SEARCH_ROWS`.
// `moves`: how many more pins the drawing rests on than the seed `pins`.
// `taken`: every pin the drawing rests on.
// `pins`: a prior run's `taken`, seeding phase 1. With that run's row in `row` and
// `portfolio_k` 0 it reproduces that run; a nonzero `portfolio_k` searches on from it.
bool layout_run(Chart &c,
                scav_spaces const &s,
                scav_layout_opts const &o,
                std::vector<scav_placed> &placed,
                std::vector<Diagnostic> &diags,
                uint32_t *inflations = nullptr,
                uint32_t *tuple = nullptr,
                uint32_t row = INVALID,
                uint32_t *moves = nullptr,
                SearchPins *taken = nullptr,
                SearchPins const *pins = nullptr);

enum class TraceScope : uint32_t {
  Shipped,  // a single-threaded re-run of the winning row and pins, searching nothing
  Search    // the whole search on one thread, every candidate included
};

// Runs `layout_run` and writes its decision trace as JSON to `out`. Debug output,
// unhashed.
bool layout_trace_json(Chart &c,
                       scav_spaces const &s,
                       scav_layout_opts const &o,
                       std::vector<scav_placed> &placed,
                       std::vector<Diagnostic> &diags,
                       std::vector<char> &out,
                       uint32_t row = INVALID,
                       TraceScope scope = TraceScope::Shipped);

// Structural: route lengths, turn directions, port sides and depths, seeded with the
// model's structural digest. Coordinate: the rest; a translation moves only this one.
uint32_t layout_structural_hash(Chart const &c);
uint32_t layout_coordinate_hash(Chart const &c);

// Hash of every non-geometry layout input: profile, router name and version, and
// space tables.
uint32_t layout_inputs_digest(Chart const &c);

// Cost ======================================================================

inline constexpr uint32_t TIER2_TERMS{ 13 };

// The thirteen Tier-2 quantities before weighting, and the Tier-0 counts.
struct CostTerms {
  int64_t bends{ 0 };      // direction changes at a route's interior vertices
  int64_t corridor{ 0 };   // length two routes' segments run collinear over
  int64_t crossings{ 0 };  // properly crossing route segment pairs
  // Per route: its length past max(end-to-end distance, its boxes' summed widths), times
  // one plus its crossings.
  int64_t excess_len{ 0 };
  // Segments joining concurrent submachines that are not adjacent; fork and join excluded.
  int64_t adjacency{ 0 };
  // Overlapping placed-box pairs, plus each placed box over a band of a state enclosing
  // both endpoints.
  int64_t label{ 0 };
  // Per placed box: its gap to its own route plus its height, less its gap to the
  // nearest other route, when positive.
  int64_t label_near{ 0 };
  int64_t aspect{ 0 };  // |w * dar_den - h * dar_num| of the root bounding box
  int64_t area{ 0 };    // the root bounding box
  // Per pair of different transitions' parallel segments that overlap along their axis
  // and lie closer than an em but not collinear: `along * (em - apart) / em`.
  int64_t crowding{ 0 };
  int64_t length{ 0 };  // every route's polyline, end to end, summed
  // Bends in a state the route only passes through: below the lowest common ancestor
  // on one end's chain, and inside neither that end nor its enclosing state.
  int64_t transit_bends{ 0 };
  // Per composite: the rect between its text bands inside its padding, less its
  // live children's rects.
  int64_t whitespace{ 0 };

  // Tier 0 counts, summed into `Cost::t0_violations`.
  int32_t through_box{ 0 };
  // Route segments entering a band of a state whose interior they may occupy.
  int32_t through_band{ 0 };
  int32_t box_overlap{ 0 };
  // Transitions with a segment to draw and a route of fewer than two points.
  int32_t vanished{ 0 };
  // Route segments running along a state's border inside its `border_band`, either side.
  int32_t flush{ 0 };
  // Route segments entering a region neither end lies in, or an external route's segments
  // entering both its ends' regions.
  int32_t through_region{ 0 };
  // Route vertices where an axis-aligned segment turns straight back along the one
  // before it.
  int32_t retrace{ 0 };
  // Pairs of one route's non-adjacent segments that cross.
  int32_t self_crossing{ 0 };
  // Pairs of different transitions' collinear segments sharing a run, except a pair both
  // in the two routes' common head or tail.
  int32_t shared_run{ 0 };
  // Per placed box, each state rect it overlaps except those enclosing both endpoints.
  int32_t label_over_box{ 0 };
  // Per placed box, each segment of another transition's route it overlaps.
  int32_t label_over_route{ 0 };
  // Placed boxes whose Chebyshev gap to their own transition's route exceeds
  // `label_leader`.
  int32_t label_far{ 0 };
  // Inner loops with an end on neither their state's border nor a ruled band's inner edge.
  int32_t loop_unanchored{ 0 };
};

inline constexpr uint32_t TIER0_TERMS{ 13 };

// Tier-0 term names, in `tier0_terms` order.
inline constexpr std::array<char const *, TIER0_TERMS> TIER0_NAMES{
  "through_box",      "through_band", "box_overlap",    "vanished",   "flush",
  "through_region",   "retrace",      "self_crossing",  "shared_run", "label_over_box",
  "label_over_route", "label_far",    "loop_unanchored"
};

// The Tier-0 counts in `TIER0_NAMES` order; `cost_of` sums them.
std::array<int32_t, TIER0_TERMS> tier0_terms(CostTerms const &t);

// Compared lexicographically, in this order.
struct Cost {
  int32_t t0_violations{ 0 };
  int64_t t1_hints{ 0 };
  int64_t t2{ 0 };
};

// Tier 0 sums the violation counts; Tier 2 sums each term times its weight, with
// lengths in ems and areas in ems squared, ceiled.
Cost cost_of(CostTerms const &t, scav_profile const &p);

// Each weighted term's share of `cost_of`'s Tier-2 sum in basis points, floored, in
// CostTerms order.
std::array<int64_t, TIER2_TERMS> cost_shares(CostTerms const &t, scav_profile const &p);

bool cost_less(Cost const &a, Cost const &b);

// Cost terms of a laid-out chart's geometry columns, decomposing the chart first.
CostTerms layout_cost(Chart const &c,
                      scav_profile const &p,
                      scav_spaces const &s = {},
                      std::vector<scav_rect> const &placed = {});

// Routers ===================================================================

uint32_t router_count();

// The name of the router at `index`. False past the end.
bool router_name(uint32_t index, scav_byte const *&out, uint32_t &len);

// The version of the router at `index`; it changes whenever its output does. False
// past the end.
bool router_version(uint32_t index, uint32_t &out);

// False when nothing registered has that name.
bool router_by_name(scav_byte const *name, uint32_t len, scav_router_id &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_H_INCLUDED
