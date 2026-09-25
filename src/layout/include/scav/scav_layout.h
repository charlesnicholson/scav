#ifndef SCAV_LAYOUT_H_INCLUDED
#define SCAV_LAYOUT_H_INCLUDED

// libscavlayout's public API: validation and digest over the space tables,
// the shipped profiles, and the router registry, over the C ABI's own PODs.

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scav {

// Space requests ============================================================

// A quarter of the coordinate domain, so a legal request cannot compose into
// an illegal box: the box formula adds requests, padding, and packed children.
inline constexpr int32_t SPACE_MAX{ COORD_MAX / 4 };

// Every row of every table against the domain, findings sorted by (code,
// kind, ordinal). False when it found anything; the caller rejects, never clamps.
bool spaces_validate(Chart const &c, scav_spaces const &s, std::vector<Diagnostic> &diags);

// xxh32 over counts and rows, field by field. A hashed layout input: a golden
// is reproducible only against a stated measurement policy. The strides are ABI
// facts rather than layout inputs and are deliberately not hashed.
uint32_t spaces_digest(scav_spaces const &s);

// Profile ===================================================================

// Fills `out` from a shipped profile, "compact" or "readable". False for an
// unknown name, writing nothing.
bool profile_named(char const *name, scav_profile &out);

// Every bound in the C header's table. scav_layout_run revalidates regardless.
bool profile_validate(scav_profile const &p);

// Drawn silhouette =========================================================

// The corner arc a `Normal` state is drawn with. Zero for every other kind --
// a fork or join bar is square, and an inscribed glyph takes the one face
// midpoint 11.5 already gives it.
//
// Stated here rather than in the reference builder because three places need
// it and only one may own it: the builder draws this arc, the router keeps its
// seats out of it, and phase 2 keeps children out of it.
//
// **Capped at the interior ring.** An eighth of the shorter side is a
// proportion, and a proportion has no bound: a 12,904-unit composite draws a
// 1,613-unit arc, twelve times the ring its children are inset by, so a child
// at the ring sits outside the shape and a seat near a corner points at blank
// canvas. Past `pad` the arc stops growing, which leaves a large box a rounded
// rectangle rather than a stadium and puts the whole ring in the straight
// zone. `pad` comes off the rects rather than the profile, the way
// `size_owner_holes` reads it, so the three sites cannot disagree about it.
inline int32_t state_corner_radius(StateKind kind, scav_rect const &box, int32_t pad) {
  if (kind != StateKind::Normal) { return 0; }
  int32_t const proportional{ ((box.w < box.h) ? box.w : box.h) / 8 };
  return (proportional < pad) ? proportional : pad;
}

// The distance a label's chosen attachment point is held from the point on its
// own polyline that anchors it: half an em, so the tighter profile sits tighter
// for free and a font-size change cannot leave the leader looking wrong
// (11.9.4).
//
// **Half an em of box, not of whitespace.** The distance is to the placed
// `scav_path_box`, and that box is a line height tall, so the gap a reader
// sees is this plus whatever of the box the glyphs do not fill. Measuring to
// the ink instead would need the app to declare its inset, which the space
// tables do not carry; recorded as owed rather than assumed (11.9.4).
inline int32_t label_leader(scav_profile const &p) {
  return (p.font_size_grid / 2 > 1) ? (p.font_size_grid / 2) : 1;
}

// One line of the profile's type. Restated from `scav_draw.h`'s `line_height`
// because layout is below draw; `functional_drawlist_tests.cpp` pins them equal
// over the whole domain, zero outside it included.
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

// One state held at a rank longest path did not give it: 11.10a's placement
// move, expressed as an input to phase 1 rather than a mutation of its output.
// Public because a drawing is a function of its tuple *and* its pins, so a
// caller re-deriving one from the model needs both.
//
// **Keyed by state and not by node**, because a node index is an artefact of
// how a frame was built -- chaining appends bends, and which index a state
// landed on is not something a caller can predict or should have to.
struct RankPin {
  StateId state{ INVALID };
  uint32_t rank{ 0 };
};

// A segment phase 1 leaves unchained, so it reaches the router as a net with no
// waypoints and is drawn against the frame's obstacles rather than through the
// corridor the layering would have sent it down (11.10b).
//
// **Keyed by transition and leg, never by segment ordinal**, for the reason
// above: an index into an internal array is not a name a caller can produce.
// `leg` indexes the transition's own run of segments, which phase 0 splits it
// into (11.1), and is 0 for a transition that crosses no boundary.
struct ChainCut {
  TransId trans{ INVALID };
  uint32_t leg{ 0 };
};

// A segment phase 1 turns around before it breaks cycles, so some other edge of
// the cycle carries the reversal (11.10d).
//
// **Which edge of a cycle is reversed decides the whole drawing, and nothing
// chose it.** Cycle-breaking is a depth-first walk in node order, so the edge
// that happens to close the walk is the one turned around -- an artefact of
// declaration order. On `estop`, a three-state cycle, reversing `Tripped ->
// Latched` instead of `Latched -> Clear` leaves no edge spanning two ranks, so
// nothing is chained, nothing carries a bend waypoint, and Tier 2 falls from
// 3,073 to 1,502 with bends 5 to 2.
struct ReversePin {
  TransId trans{ INVALID };
  uint32_t leg{ 0 };
};

// The face one end of a segment leaves its box by: 0 left, 1 right, 2 top, 3
// bottom, and `end` 0 is the departure and 1 the arrival (11.10e).
//
// **Which face a transition leaves by decides how many times it turns**, and
// the router picks it by which separation to the other end is larger -- a rule
// handed one point and one box, so it cannot see the turn it is buying. On
// `estop`, `Tripped -> Latched` leaves the left face and turns twice where the
// bottom face turns once. A better rule was measured and is not the answer
// (11.5); the choice is small, discrete, and belongs to the search.
struct FacePin {
  TransId trans{ INVALID };
  uint32_t leg{ 0 };
  uint32_t end{ 0 };
  uint32_t face{ 0 };
};

// Everything besides the tuple that a drawing is a function of, so re-deriving
// one is two arguments and not seven (11.10a, 11.10b, 11.10d, 11.10e).
struct SearchPins {
  std::vector<RankPin> ranks;
  std::vector<ChainCut> cuts;
  std::vector<ReversePin> reverses;
  std::vector<FacePin> faces;
};

// Rows in the fixed table of chart-global phase-2 tuples Level 2 chooses
// between: the box packer, compaction, where a frame's desired ratio comes
// from, and whether its rank run folds (11.10, 11.10a). The bound on
// `portfolio_m` and on `layout_run`'s `row`.
inline constexpr uint32_t LAYOUT_SEARCH_ROWS{ 16 };

// Decomposes, orders, then sizes, places and routes every phase-2 tuple the
// chart's size admits and keeps the one exact `Cost` ranks first (11.10).
// Writes the geometry columns and sizes `placed` to the path boxes. False
// leaves the last successful run's columns. True can still leave RouteDegraded
// findings in `diags`, one per transition drawn as a straight line.
// `inflations` receives how many spacing inflations the written geometry took
// and `tuple` which row of the table produced it, row 0 being the profile as
// the caller passed it.
//
// `row` runs one named row of the table in place of the search, which is what
// lets calibration render a candidate the objective would never pick and score
// it beside the one it does (11.10, 11.12). It is not a search knob: nothing
// shipping passes it, `portfolio_m` is unread when it is set, and the caller
// bounds it -- `INVALID` searches, and anything else must be below
// `LAYOUT_SEARCH_ROWS`.
//
// `pins` seeds phase 1 with what a previous run's `taken` reported, which is the
// other half of re-deriving a drawing from the model: with the tuple in `row`
// and `portfolio_k` at zero it reproduces that run exactly, searching nothing.
// Level 1 continues from them when it has budget.
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

// The decisions behind the drawing that ships, as JSON in `out` (11.16).
// Debug-only: the trace is not a geometry column, is not hashed, and no builder
// reads it.
//
// **Searches first, then traces what won.** Tracing the search itself records
// every candidate it rejected and leaves no way to tell which one shipped, so
// this runs normally, takes the winning tuple and pins, and re-derives that one
// drawing with the sink attached and nothing searching. `threads` is one for
// the traced run, so event order is the algorithm's and not the scheduler's.
// The geometry is the searched run's, byte for byte.
// `Shipped` traces the drawing that won and nothing else. `Search` traces the
// search itself, every rejected candidate included, which is what answers "why
// was that move not taken" -- at the price that the stream holds drawings
// nobody sees and no marker says which of them shipped.
enum class TraceScope : uint32_t { Shipped, Search };

bool layout_trace_json(Chart &c,
                       scav_spaces const &s,
                       scav_layout_opts const &o,
                       std::vector<scav_placed> &placed,
                       std::vector<Diagnostic> &diags,
                       std::vector<char> &out,
                       uint32_t row = INVALID,
                       TraceScope scope = TraceScope::Shipped);

// Split so a pure translation moves the coordinate hash and not the structural
// one: structure is sides, depths and turn tokens; coordinates are the rest.
// The structural hash is seeded with the model's structural digest (6).
uint32_t layout_structural_hash(Chart const &c);
uint32_t layout_coordinate_hash(Chart const &c);

// Every non-geometry input: profile, router name and version, space tables --
// through which the font reaches a digest it cannot be an argument to. A third
// value, since seeding the other two would cost the split its point.
uint32_t layout_inputs_digest(Chart const &c);

// Cost ======================================================================

inline constexpr uint32_t TIER2_TERMS{ 10 };

// The ten Tier-2 quantities before weighting, so a test reads one of them
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
  // Per pair of different transitions' segments on one axis, overlapping along
  // it and closer than an em but not collinear: the overlap scaled by the
  // shortfall, `along * (em - apart) / em`. Continuous with `corridor` at
  // `apart = 0` (11.6).
  int64_t crowding{ 0 };

  // Tier 0, forbidden rather than priced: the obstacle set makes these
  // unrepresentable, and the count survives as a net (11.6).
  int32_t through_box{ 0 };
  int32_t box_overlap{ 0 };
  // Transitions with a segment to draw whose route came out as fewer than two
  // points, so nothing is drawn. **Forbidden because every Tier-2 term scores it
  // perfect** -- no bends, no length, no excess, no crowding -- and a search that
  // can reach one will prefer it (11.6).
  int32_t vanished{ 0 };
  // Route segments running along a state's border. A reader cannot tell a
  // route on a border from the border, and nothing in Tier 2 sees it (11.10g).
  int32_t flush{ 0 };
  // Route segments entering a region neither end lies in: a concurrent
  // sibling of an endpoint's own region, or any region of a state the route
  // only leaves or reaches. `through_box` sees the states; this sees the
  // regions inside a state a route is allowed to be in (11.10g).
  int32_t through_region{ 0 };
};

// Compared lexicographically, in this order.
struct Cost {
  int32_t t0_violations{ 0 };
  int64_t t1_hints{ 0 };
  int64_t t2{ 0 };
};

// Every term is converted to the unit the profile names it in before its weight
// applies, so a weight is an exchange rate between comparable quantities (11.6).
Cost cost_of(CostTerms const &t, scav_profile const &p);

// Each weighted term's share of `cost_of`'s sum in basis points, floored and in
// CostTerms order, so a golden watches the balance a weight change moves.
std::array<int64_t, TIER2_TERMS> cost_shares(CostTerms const &t, scav_profile const &p);

bool cost_less(Cost const &a, Cost const &b);

// The cost vector of a chart's own geometry columns, decomposing as it goes, so
// a caller holding a laid-out chart needs nothing internal to score one. The
// scoring itself is `cost_columns`, which is what a test reaches for when it
// already has the split graph.
CostTerms layout_cost(Chart const &c,
                      scav_profile const &p,
                      scav_spaces const &s = {},
                      std::vector<scav_rect> const &placed = {});

// Routers ===================================================================

uint32_t router_count();

// The name of the router at `index`. False past the end.
bool router_name(uint32_t index, scav_byte const *&out, uint32_t &len);

// Its version, bumped whenever its output moves. False past the end.
bool router_version(uint32_t index, uint32_t &out);

// False when nothing registered has that name.
bool router_by_name(scav_byte const *name, uint32_t len, scav_router_id &out);

}  // namespace scav

#endif  // SCAV_LAYOUT_H_INCLUDED
