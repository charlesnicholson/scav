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

// The face of its box that end `end` (0 departure, 1 arrival) of leg `leg` of `trans`
// attaches to: 0 left, 1 right, 2 top, 3 bottom.
struct FacePin {
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

// The side of its state's border a leg end's port sits on: 0 left, 1 right, 2 top,
// 3 bottom; `end` as in `FacePin`. Ignored for a port on a region's border.
struct SidePin {
  TransId trans{ INVALID };
  uint32_t leg{ 0 };
  uint32_t end{ 0 };
  uint32_t side{ 0 };
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

// Every input besides the tuple that a drawing depends on.
struct SearchPins {
  std::vector<RankPin> ranks;
  std::vector<ChainCut> cuts;
  std::vector<ReversePin> reverses;
  std::vector<FacePin> faces;
  std::vector<OrientPin> orients;
  std::vector<SidePin> sides;
  std::vector<FoldPin> folds;  // the last pin naming a frame decides it
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
  // Route segments running along a state's border.
  int32_t flush{ 0 };
  // Route segments entering a region neither end lies in: a concurrent sibling of an
  // endpoint's own region, or any region of a state the route only leaves or reaches.
  int32_t through_region{ 0 };
  // Route vertices where an axis-aligned segment turns straight back along the one
  // before it.
  int32_t retrace{ 0 };
  // Per placed box, each state rect it overlaps except those enclosing both endpoints.
  int32_t label_over_box{ 0 };
  // Per placed box, each segment of another transition's route it overlaps.
  int32_t label_over_route{ 0 };
};

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
