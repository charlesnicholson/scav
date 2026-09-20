// The four phases in a line, the portfolio of phase-2 tuples wrapped around
// the last two of them, then the geometry columns as the only output.
// Everything else here is the columns and the two hashes over them.

#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/route.h"
#include "layout/router.h"
#include "layout/shard.h"
#include "layout/size.h"
#include "layout/trace.h"
#include "layout/wire.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav_int.h"
#include "scav_internal.h"
#include "scav_thread.h"
#include "scav_xxhash.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace scav {

SCAV_INTERNAL_BEGIN
// The inflation loop's decision and the portfolio's three pure parts,
// bracketed so a test reaches cases no chart does. The prototypes a test uses
// are its own; see scav_internal.h.
bool inflation_done(uint32_t fewest, uint32_t degraded, uint32_t unreachable, bool &keep);
uint32_t search_tuple_count(scav_profile const &p, uint32_t entity_count);
uint32_t search_move_budget(scav_profile const &p, uint32_t entity_count);
void search_tuple(scav_profile &p,
                  DarSource &dar,
                  Compaction &pack,
                  Fold &fold,
                  uint32_t index);
uint32_t search_argmin(std::vector<Cost> const &cost, std::vector<uint8_t> const &viable);
SCAV_INTERNAL_END

namespace {

constexpr uint32_t RECT{ sizeof(scav_rect) };

// One name and the shape it is registered under. `write_rows` copies a row per
// entity through whichever column carries the name, so the shape has to be the
// one layout would have registered or the copy runs past the column's bytes.
struct GeomShape {
  char const *name;
  ElemKind entity;
  ValueKind kind;
  uint32_t elem_size;
};

// Index into GEOM, so the writer below and the check in `layout_run` name the
// same row rather than repeating its fields.
enum GeomColumnIndex : uint32_t {
  GeomState,
  GeomBefore,
  GeomAfter,
  GeomSub,
  GeomRoute,
  GeomPort,
  GeomPoint,
  GeomPortSlot,
  GeomChart,
  GeomInputs,
  GeomGen,
  GeomCount,
};

constexpr std::array<GeomShape, GeomCount> GEOM{ {
    { .name = "scav.geom.state",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.state_before",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.state_after",
      .entity = ElemKind::State,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.sub",
      .entity = ElemKind::Submachine,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.route",
      .entity = ElemKind::Transition,
      .kind = ValueKind::Span,
      .elem_size = 8 },
    { .name = "scav.geom.port",
      .entity = ElemKind::Transition,
      .kind = ValueKind::Span,
      .elem_size = 8 },
    { .name = "scav.geom.point",
      .entity = ElemKind::Point,
      .kind = ValueKind::Pod,
      .elem_size = 8 },
    { .name = "scav.geom.portslot",
      .entity = ElemKind::Point,
      .kind = ValueKind::Pod,
      .elem_size = sizeof(scav_port_slot) },
    { .name = "scav.geom.chart",
      .entity = ElemKind::Chart,
      .kind = ValueKind::Pod,
      .elem_size = RECT },
    { .name = "scav.geom.inputs",
      .entity = ElemKind::Chart,
      .kind = ValueKind::U32,
      .elem_size = 4 },
    { .name = "scav.geom.gen",
      .entity = ElemKind::Chart,
      .kind = ValueKind::U32,
      .elem_size = 4 },
} };

// Registered on first use, found thereafter; every run overwrites in place. The
// found column's shape was checked before any geometry was computed.
ColumnId geom_column(Chart &c, GeomShape const &g) {
  ColumnId const found{ column_find(c, g.name) };
  if (found.v != INVALID) { return found; }
  return column_register(c, g.name, g.entity, g.kind, g.elem_size, 4, COLUMN_DERIVED);
}

// The first name already registered under another entity, value kind, or
// element size. GeomCount when every one of them is layout's own to write.
uint32_t geom_column_clash(Chart const &c) {
  for (uint32_t i = 0; i < GeomCount; ++i) {
    ColumnId const found{ column_find(c, GEOM[i].name) };
    if (found.v == INVALID) { continue; }
    ColumnDesc const &d{ c.columns[found.v].desc };
    if ((d.entity != GEOM[i].entity) || (d.kind != GEOM[i].kind) ||
        (d.elem_size != GEOM[i].elem_size)) {
      return i;
    }
  }
  return GeomCount;
}

template <typename T>
void write_rows(Chart &c, ColumnId id, std::vector<T> const &rows) {
  if (!rows.empty()) {
    std::memcpy(column_data(c, id), rows.data(), rows.size() * sizeof(T));
  }
}

static_assert(sizeof(scav_profile) == 48 * sizeof(int32_t),
              "the profile must stay a flat block of int32 with no padding, or the "
              "inputs digest below would hash bytes whose values are unspecified");

// Every non-geometry input a golden depends on. Without it a hash names the
// numbers that came out and not the run that produced them.
uint32_t inputs_digest(scav_spaces const &s, scav_layout_opts const &o) {
  std::vector<scav_byte> b;
  // Padding is what forbids hashing a struct's bytes, and the assert above
  // proves there is none, so the copy reads all 48 knobs and nothing else.
  std::array<int32_t, sizeof(scav_profile) / sizeof(int32_t)> profile{};
  std::memcpy(profile.data(), &o.profile, sizeof(scav_profile));
  for (int32_t const field : profile) { append_i32(b, field); }

  scav_byte const *name{ nullptr };
  uint32_t name_len{ 0 };
  uint32_t version{ 0 };
  if (router_name(o.router, name, name_len) && router_version(o.router, version)) {
    append_u32(b, name_len);
    b.insert(b.end(), name, name + name_len);
    append_u32(b, version);
  }
  // The font reaches layout only as the integers it measured, so its identity
  // rides in here rather than as an argument layout would never read.
  append_u32(b, spaces_digest(s));
  return xxhash32(b.data(), b.size(), 0);
}

void write_columns(Chart &c, SizedLayout const &z, Routes const &r, uint32_t inputs) {
  write_rows(c, geom_column(c, GEOM[GeomState]), z.state);
  write_rows(c, geom_column(c, GEOM[GeomBefore]), z.before);
  write_rows(c, geom_column(c, GEOM[GeomAfter]), z.after);
  write_rows(c, geom_column(c, GEOM[GeomSub]), z.sub);
  write_rows(c, geom_column(c, GEOM[GeomRoute]), r.route);
  write_rows(c, geom_column(c, GEOM[GeomPort]), r.port);

  ColumnId const pts{ geom_column(c, GEOM[GeomPoint]) };
  column_resize(c, pts, static_cast<uint32_t>(r.points.size()));
  write_rows(c, pts, r.points);
  ColumnId const slots{ geom_column(c, GEOM[GeomPortSlot]) };
  column_resize(c, slots, static_cast<uint32_t>(r.slots.size()));
  write_rows(c, slots, r.slots);

  ColumnId const chart{ geom_column(c, GEOM[GeomChart]) };
  std::memcpy(column_data(c, chart), &z.chart, sizeof(z.chart));

  ColumnId const in{ geom_column(c, GEOM[GeomInputs]) };
  std::memcpy(column_data(c, in), &inputs, 4);

  ColumnId const gen{ geom_column(c, GEOM[GeomGen]) };
  uint32_t n{ 0 };
  std::memcpy(&n, column_data(c, gen), 4);
  ++n;
  std::memcpy(column_data(c, gen), &n, 4);
}

// Every separation raised by one increment. False when the result leaves the
// range the validator admits.
bool inflate(scav_profile &p, int32_t by) {
  p.rank_sep += by;
  p.node_sep += by;
  p.sub_sep += by;
  return profile_validate(p);
}

// One candidate: its geometry and what it took to reach.
struct Candidate {
  SizedLayout sized;
  Routes routes;
  uint32_t inflations{ 0 };
  bool viable{ false };
};

// Phases 2 and 3 for one tuple, with the spacing-inflation retry exactly as a
// lone run has it. `knobs` is the caller's profile with the tuple applied.
Candidate search_candidate(Chart const &c,
                           SplitGraph const &g,
                           SubmachineOrders const &orders,
                           scav_spaces const &s,
                           scav_profile const &knobs,
                           DarSource dar,
                           Compaction pack,
                           Fold fold,
                           Router const &router,
                           uint32_t threads,
                           std::vector<Diagnostic> &diags,
                           RouteCache const *reuse = nullptr,
                           RouteCache *fill = nullptr) {
  Candidate out;
  if (!size_layout(c, g, orders, s, knobs, out.sized, diags, dar, pack, fold)) {
    return out;
  }
  out.routes = route_transitions(c, g, orders, out.sized, s, knobs, router, threads,
                                 reuse, fill);

  // `out` carries the best attempt so far, and `done` is set from that one
  // rather than from whichever attempt was just made.
  scav_profile wider{ knobs };
  uint32_t fewest{ out.routes.degraded() };
  bool done{ out.routes.unreachable == 0 };
  // An increment of zero repeats one attempt to the cap, so it is not one.
  for (int32_t k = 0; !done && (knobs.spacing_inflation_increment > 0) &&
                      (k < knobs.spacing_inflation_cap);
       ++k) {
    if (!inflate(wider, knobs.spacing_inflation_increment)) { break; }
    SizedLayout next_sized;
    std::vector<Diagnostic> spilled;
    if (!size_layout(c, g, orders, s, wider, next_sized, spilled, dar, pack, fold)) {
      break;
    }
    Routes next{ route_transitions(c, g, orders, next_sized, s, wider, router,
                                   threads) };
    bool keep{ false };
    done = inflation_done(fewest, next.degraded(), next.unreachable, keep);
    if (keep) {
      fewest = next.degraded();
      out.sized = std::move(next_sized);
      out.routes = std::move(next);
      out.inflations = static_cast<uint32_t>(k) + 1;
      trace_emit({ .kind = TraceKind::SpacingInflated,
                   .inflate = { .node_sep = wider.node_sep, .rank_sep = wider.rank_sep } });
    }
  }

  // Bounds everything laid out, not just the root submachine: a route bends into
  // a frame's padding and a path box centres on one, so both can reach past it.
  // Before the score rather than after the pick, so `area` and `aspect` price
  // the canvas that ships.
  auto const cover = [&out](int32_t x, int32_t y) {
    scav_rect &chart{ out.sized.chart };
    int32_t const right{ imax(chart.x + chart.w, x) };
    int32_t const bottom{ imax(chart.y + chart.h, y) };
    chart.x = imin(chart.x, x);
    chart.y = imin(chart.y, y);
    chart.w = right - chart.x;
    chart.h = bottom - chart.y;
  };
  for (scav_point const &at : out.routes.points) { cover(at.x, at.y); }
  for (scav_rect const &at : out.routes.placed) {
    cover(at.x, at.y);
    cover(at.x + at.w, at.y + at.h);
  }
  out.viable = true;
  return out;
}

}  // namespace

SCAV_INTERNAL_BEGIN

// Whether the loop is finished, and through `keep` whether this attempt
// replaces the best so far. The case that matters is one no chart reaches: only
// a router answering `outside_region` or `too_large` where it used to answer
// `unreachable` produces an attempt that reaches every end while degrading
// more, and the shipped one does not do that on any chart in the corpus or the
// suite.
bool inflation_done(uint32_t fewest, uint32_t degraded, uint32_t unreachable, bool &keep) {
  keep = degraded < fewest;
  // Only the attempt that is kept can end the loop, because the kept attempt is
  // the geometry that ships. One that reaches every end while degrading more
  // elsewhere is discarded, and stopping on it would leave behind exactly the
  // unreachable ends the retry existed to remove.
  return keep && (unreachable == 0);
}

// What a bounded-move pass kept, so the caller replaces its candidate only when
// something was taken (11.10a).
struct Improved {
  Candidate best;
  Cost cost{};
  SearchPins held;
  uint32_t took{ 0 };
  uint32_t scored{ 0 };
};

// One Level 1 move: a state held at a rank it was not given (11.10a), or a
// segment left unchained (11.10b). `cut` names which of the two.
struct Move {
  RankPin pin{};
  ChainCut leg{};
  bool cut{ false };
};

// What scoring one move yields, and all of it: the geometry is deliberately
// not here, so a round in flight costs a `Cost` per candidate rather than a
// layout per candidate (11.10c).
struct Scored {
  Cost cost{};
  std::array<int32_t, TIER2_TERMS> share{};
  bool viable{ false };
  bool inflated{ false };
};

// Phases 1 to 3 for one set of pins, then the exact objective over the result.
// Pure: reads the model, the split, the tables and its arguments, writes
// nothing either of its callers can see, which is what lets a round of them run
// at once (11.10c).
Scored score_move(Chart const &c,
                  SplitGraph const &g,
                  scav_spaces const &s,
                  scav_profile const &objective,
                  scav_profile const &knobs,
                  DarSource dar,
                  Compaction pack,
                  Fold fold,
                  Router const &router,
                  SearchPins const &pins,
                  RouteCache const *reuse) {
  Scored out;
  SubmachineOrders const moved{ order_submachines(c, g, s, objective, 1, pins) };
  std::vector<Diagnostic> spilled;
  Candidate const cand{ search_candidate(c, g, moved, s, knobs, dar, pack, fold, router,
                                         1, spilled, reuse) };
  if (!cand.viable) { return out; }
  out.viable = true;
  if (cand.inflations != 0) {
    out.inflated = true;
    return out;
  }
  CostTerms const terms{ cost_terms(c, g, cand.sized, cand.routes, s, objective) };
  out.cost = cost_of(terms, objective);
  std::array<int64_t, TIER2_TERMS> const share{ cost_shares(terms, objective) };
  for (uint32_t k = 0; k < TIER2_TERMS; ++k) {
    out.share[k] = static_cast<int32_t>(share[k]);
  }
  return out;
}

// Level 1: hold one state at a rank longest path did not give it, re-derive
// phase 1 from that, and run phases 2 and 3 whole. Greedy and strictly
// improving on the exact objective, in state order then rank order, so the pass
// is a function of the model rather than of the order candidates happened to be
// enumerated in (6).
//
// **A move is a pin and pins compose**, so accepting one is appending to the
// list the next round re-derives from, and rejecting one is not appending. No
// geometry is ever mutated and nothing has to be rolled back (11.10a).
Improved search_moves(Chart const &c,
                      SplitGraph const &g,
                      scav_spaces const &s,
                      scav_profile const &objective,
                      scav_profile const &knobs,
                      DarSource dar,
                      Compaction pack,
                      Fold fold,
                      Router const &router,
                      uint32_t threads,
                      uint32_t budget,
                      Cost from,
                      SearchPins const &seed) {
  Improved out;
  out.cost = from;
  // Continued from, not restarted: `taken` reports every pin the drawing rests
  // on, so a caller handing them back gets the moves it already has plus more.
  SearchPins &held{ out.held };
  held = seed;
  SubmachineOrders here{ order_submachines(c, g, s, objective, threads, held) };

  // The incumbent every candidate of a round is one frame away from. Read by
  // all of them at once and written only here, between rounds (11.10c).
  RouteCache base;
  {
    std::vector<Diagnostic> spilled;
    Candidate first{ search_candidate(c, g, here, s, knobs, dar, pack, fold, router,
                                      threads, spilled, nullptr, &base) };
    out.best = std::move(first);
  }

  auto const with = [](SearchPins base_pins, Move const &m) {
    if (m.cut) {
      base_pins.cuts.push_back(m.leg);
    } else {
      base_pins.ranks.push_back(m.pin);
    }
    return base_pins;
  };

  // One counter per dimension, each capped at the budget. Shared, the sweep
  // that runs first starves the other -- measured: cuts ahead of placements on
  // one shared 24 took `axis` from 11,780 to 12,702, every move still strictly
  // improving and the walk simply cut short (11.10b).
  uint32_t cut_scored{ 0 };
  uint32_t pin_scored{ 0 };
  std::vector<Move> round;
  std::vector<Scored> got;
  std::vector<uint8_t> chained;
  while ((cut_scored < budget) || (pin_scored < budget)) {
    // Enumerated first, scored second, reduced third. The scan used to do all
    // three at once, which made it sequential for no reason: a candidate is a
    // pure function of the model, the tuple and the pins, and nothing it
    // computes is read by the next one (11.10c).
    round.clear();

    // Unchaining first: the segments a cut can free are exactly the ones phase
    // 1 chained, which the bends it created name (11.10b).
    chained.assign(g.segments.size(), 0);
    for (OrderNode const &nd : here.nodes) {
      if ((nd.kind == OrderKind::Bend) && (nd.subject < chained.size())) {
        chained[nd.subject] = 1;
      }
    }
    for (ChainCut const &already : held.cuts) {
      if (already.trans.v >= g.trans_segments.size()) { continue; }
      Span const segs{ g.trans_segments[already.trans.v] };
      if (already.leg < segs.len) { chained[segs.off + already.leg] = 0; }
    }
    for (uint32_t seg = 0; (seg < chained.size()) && (cut_scored < budget); ++seg) {
      if (chained[seg] == 0) { continue; }
      TransId const t{ g.segments[seg].trans };
      if ((t.v == INVALID) || (t.v >= g.trans_segments.size())) { continue; }
      ++cut_scored;
      round.push_back({ .leg = { .trans = t, .leg = seg - g.trans_segments[t.v].off },
                        .cut = true });
    }

    for (uint32_t st = 0; (st < c.states.size()) && (pin_scored < budget); ++st) {
      if ((c.states[st].live == 0) || (here.state_node[st] == INVALID)) { continue; }
      uint32_t const at{ here.nodes[here.state_node[st]].rank };
      uint32_t const frame{ c.states[st].parent.v };
      if (frame >= here.sub_ranks.size()) { continue; }
      uint32_t const ranks{ here.sub_ranks[frame] };
      for (uint32_t r = 0; (r < ranks) && (pin_scored < budget); ++r) {
        if (r == at) { continue; }
        ++pin_scored;
        round.push_back({ .pin = { .state = StateId{ st }, .rank = r } });
      }
    }
    if (round.empty()) { break; }

    // Each worker runs its candidate's phases on one thread: `parallel_for` is
    // fork-join, so sharding frames underneath sharded candidates would
    // oversubscribe every core it already has work on.
    got.assign(round.size(), {});
    parallel_for(static_cast<uint32_t>(round.size()), threads, [&](uint32_t i) {
      got[i] = score_move(c, g, s, objective, knobs, dar, pack, fold, router,
                          with(held, round[i]), &base);
    });
    out.scored += static_cast<uint32_t>(round.size());

    // Reduced in enumeration order, so the pass is a function of the model and
    // not of which worker finished first (6). The trace is emitted here for the
    // same reason -- a worker writing it would order it by the scheduler.
    Move take{};
    Cost best{ out.cost };
    bool found{ false };
    for (uint32_t i = 0; i < round.size(); ++i) {
      Move const &m{ round[i] };
      Scored const &sc{ got[i] };
      MoveVerdict verdict{ MoveVerdict::NotBetter };
      if (!sc.viable) {
        verdict = MoveVerdict::NotViable;
      } else if (sc.inflated) {
        // A move that only fits once the whole chart's spacing was widened is
        // not a better placement, it is a bigger drawing -- and it is one no
        // caller can re-derive from the pins alone, because the geometry
        // belongs to a profile the pins do not name.
        verdict = MoveVerdict::Inflated;
      } else if (cost_less(sc.cost, best)) {
        verdict = MoveVerdict::Taken;
        best = sc.cost;
        take = m;
        found = true;
      }
      trace_emit({ .kind = TraceKind::CandidateScored,
                   .pass = static_cast<uint16_t>(verdict),
                   .score = { .row = INVALID,
                              .state = m.cut ? INVALID : m.pin.state.v,
                              .rank = m.cut ? 0 : m.pin.rank,
                              .trans = m.cut ? m.leg.trans.v : INVALID,
                              .leg = m.cut ? m.leg.leg : 0,
                              .t0 = sc.viable ? sc.cost.t0_violations : 0,
                              .t2 = sc.viable ? sc.cost.t2 : 0 } });
      // Beside the score, so a rejected move says which term rejected it.
      if ((trace_sink() != nullptr) && sc.viable) {
        TraceEvent e{ .kind = TraceKind::CandidateTerms, .terms = {} };
        for (uint32_t k = 0; k < TIER2_TERMS; ++k) {
          e.terms.share[k] = sc.share[k];
        }
        trace_emit(e);
      }
    }

    if (!found) { break; }
    held = with(held, take);
    here = order_submachines(c, g, s, objective, threads, held);
    // Recomputed rather than carried out of the scan: keeping every
    // candidate's geometry in flight is a whole layout per candidate, and one
    // re-run is a round's cost divided by its width.
    std::vector<Diagnostic> spilled;
    out.best = search_candidate(c, g, here, s, knobs, dar, pack, fold, router, threads,
                                spilled, nullptr, &base);
    out.cost = best;
    ++out.took;
  }
  return out;
}

// How many of the table's rows this chart runs, and how many bounded moves it
// scores: all of `portfolio_m` and all of `portfolio_k`, whatever its size.
//
// **Both used to halve for every doubling of the entity count past 512**, which
// answered an expensive candidate by searching less and, past 16k entities, set
// the move budget to zero and switched Level 1 off entirely. That is quality
// paying for latency, and the cost of a candidate is the thing to fix instead
// (11.10c): a round of them now runs at once, and a candidate reuses every
// frame its move did not touch. The entity count is kept in the signature
// because what should bound a big chart is the work a candidate actually does,
// which is the next thing to measure rather than a closed form over size.
uint32_t search_tuple_count(scav_profile const &p, uint32_t /*entity_count*/) {
  return imin(static_cast<uint32_t>(imax(p.portfolio_m, 1)), LAYOUT_SEARCH_ROWS);
}

uint32_t search_move_budget(scav_profile const &p, uint32_t /*entity_count*/) {
  return static_cast<uint32_t>(imax(p.portfolio_k, 0));
}

// Row `index` of the fixed table, as a delta from the profile as given: bit 0
// flips the box packer, bit 1 turns compaction on, bit 2 hands each frame its
// owner's hole. Row 0 is therefore the caller's own tuple, and `portfolio_m` of
// 1 is the pipeline as it ran before the portfolio existed. The packer is bit 0
// because it is the knob that moves a chart: every pick the corpus makes on
// either scale is row 1. **`sm_tiebreak` is no longer a row**: it won on no
// chart at either scale, so the table spent a bit on a knob that decided
// nothing, and compaction — which does move charts, in both directions — took
// it. The field stays a profile knob a caller may set; no row flips it.
void search_tuple(scav_profile &p,
                  DarSource &dar,
                  Compaction &pack,
                  Fold &fold,
                  uint32_t index) {
  p.trybox ^= static_cast<int32_t>(index & 1U);
  pack = (((index >> 1U) & 1U) != 0) ? Compaction::On : Compaction::Off;
  dar = (((index >> 2U) & 1U) != 0) ? DarSource::OwnerHole : DarSource::Profile;
  fold = (((index >> 3U) & 1U) != 0) ? Fold::Always : Fold::Scale;
}

// `argmin(Cost, index)` over the candidates, in index order: the combine is
// associative and not commutative, so the order is what makes it one value (6).
// `viable` is parallel to `cost`, and row 0 answers for a set with nothing in
// it -- the caller's own tuple, whose failure it was already told about.
uint32_t search_argmin(std::vector<Cost> const &cost, std::vector<uint8_t> const &viable) {
  uint32_t best{ 0 };
  bool found{ false };
  for (uint32_t i = 0; i < cost.size(); ++i) {
    if (viable[i] == 0) { continue; }
    if (!found || cost_less(cost[i], cost[best])) {
      best = i;
      found = true;
    }
  }
  return best;
}

SCAV_INTERNAL_END

bool layout_run(Chart &c,
                scav_spaces const &s,
                scav_layout_opts const &o,
                std::vector<scav_placed> &placed,
                std::vector<Diagnostic> &diags,
                uint32_t *inflations,
                uint32_t *tuple,
                uint32_t row,
                uint32_t *moves,
                SearchPins *taken,
                SearchPins const *pins) {
  if (inflations != nullptr) { *inflations = 0; }
  if (tuple != nullptr) { *tuple = 0; }
  scav_profile const &p{ o.profile };
  if (!profile_validate(p)) {
    diags.push_back({ .code = DiagCode::ProfileOutOfRange,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }
  if (!spaces_validate(c, s, diags)) { return false; }

  if (geom_column_clash(c) != GeomCount) {
    diags.push_back({ .code = DiagCode::GeometryColumnClash,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }

  Router const *const router{ router_at(o.router) };
  if (router == nullptr) {
    diags.push_back({ .code = DiagCode::RouterUnknown,
                      .subject = { .kind = ElemKind::Chart, .ordinal = 0 },
                      .doc = { INVALID },
                      .src = {} });
    return false;
  }

  SplitGraph const g{ decompose(c) };
  // Once for every attempt below: phase 1 reads `sweep_count` and no extent, so
  // the inflated copies the retry loop makes order to the same rows.
  SearchPins const seed{ (pins != nullptr) ? *pins : SearchPins{} };
  SubmachineOrders const orders{ order_submachines(c, g, s, p, o.threads, seed) };

  // Level 2: every row of the table this chart's size admits, each its own
  // whole phase 2 and 3, collected rather than folded into a best-so-far, and
  // reduced in index order afterwards (6, 11.10). Row 0 is the profile as
  // given, so it is what a failure is reported for and what M of 1 runs alone.
  //
  // A pinned row is a table of one, so it is slot 0 of everything below: the
  // row a failure is reported for, the row nothing is ranked against, and the
  // row `tuple` comes back as.
  bool const pinned{ row != INVALID };
  uint32_t const rows{ pinned ? 1U : search_tuple_count(p, layout_entity_count(c)) };
  std::vector<Candidate> candidates(rows);
  std::vector<Cost> cost(rows);
  std::vector<uint8_t> viable(rows, 0);
  // Rows are whole independent layouts of one chart, so they run at once and
  // are reduced afterwards in index order (11.10c). Each runs its own phases on
  // one thread, since `parallel_for` is fork-join.
  std::vector<std::vector<Diagnostic>> spilled(rows);
  parallel_for(rows, (rows > 1) ? o.threads : 1U, [&](uint32_t i) {
    scav_profile knobs{ p };
    DarSource dar{ DarSource::Profile };
    Compaction pack{ Compaction::Off };
    Fold fold{ Fold::Scale };
    search_tuple(knobs, dar, pack, fold, pinned ? row : i);
    candidates[i] = search_candidate(c,
                                     g,
                                     orders,
                                     s,
                                     knobs,
                                     dar,
                                     pack,
                                     fold,
                                     *router,
                                     (rows > 1) ? 1U : o.threads,
                                     spilled[i]);
    viable[i] = candidates[i].viable ? 1U : 0U;
    // One row has nothing to rank, so it is not scored: `argmin` over one
    // candidate is that candidate. The objective is the caller's profile and
    // not the tuple's copy, so two rows are compared on one scale even where
    // one of them inflated.
    if ((rows > 1) && (viable[i] != 0)) {
      CostTerms const t{
        cost_terms(c, g, candidates[i].sized, candidates[i].routes, s, p)
      };
      cost[i] = cost_of(t, p);
    }
  });
  // Row 0 is the caller's own tuple, so its findings are the run's and every
  // other row's are a candidate's business. Merged here rather than in the
  // worker, where the order would be the scheduler's.
  diags.insert(diags.end(), spilled[0].begin(), spilled[0].end());
  // A tuple that leaves the coordinate domain is no candidate, and the caller's
  // own tuple leaving it is the run's failure, as it was before anything else
  // was tried.
  if (!candidates[0].viable) { return false; }

  uint32_t best{ search_argmin(cost, viable) };

  // Level 1: bounded moves off the row Level 2 picked, each one a whole phase 1
  // to 3 and scored on the exact objective, greedy and strictly improving
  // (11.10, 11.10a, 11.10b). `portfolio_k` caps the candidates scored across
  // both dimensions, and at zero a run is exactly the run it was before Level 1.
  //
  // Placed here rather than inside the row loop because a move is off the
  // *pick*: scoring moves against a row the argmin is about to discard spends
  // the budget on a drawing nobody sees.
  uint32_t const budget{ search_move_budget(p, layout_entity_count(c)) };
  // Written whichever way the branch below goes: a count nobody set is worse
  // than a zero, and zero is the honest answer for a run with no budget.
  if (moves != nullptr) { *moves = 0; }
  // What the drawing rests on, not what this run added: a run with no budget
  // still stands on the pins it was handed.
  if (taken != nullptr) { *taken = seed; }
  if ((budget != 0) && (viable[best] != 0)) {
    scav_profile knobs{ p };
    DarSource dar{ DarSource::Profile };
    Compaction pack{ Compaction::Off };
    Fold fold{ Fold::Scale };
    search_tuple(knobs, dar, pack, fold, pinned ? row : best);
    Improved const done{ search_moves(c,
                                      g,
                                      s,
                                      p,
                                      knobs,
                                      dar,
                                      pack,
                                      fold,
                                      *router,
                                      o.threads,
                                      budget,
                                      cost[best],
                                      seed) };
    if (done.took != 0) {
      candidates[best] = std::move(done.best);
      cost[best] = done.cost;
    }
    if (moves != nullptr) { *moves = done.took; }
    // The drawing is a function of the tuple *and* the pins, so a caller
    // re-deriving it from the model needs both.
    if (taken != nullptr) { *taken = done.held; }
  }

  SizedLayout sized{ std::move(candidates[best].sized) };
  Routes routes{ std::move(candidates[best].routes) };
  if (inflations != nullptr) { *inflations = candidates[best].inflations; }
  if (tuple != nullptr) { *tuple = pinned ? row : best; }
  placed = routes.placed;

  // `failed` is parallel to the transitions, so one walk emits the findings in
  // ordinal order.
  for (uint32_t t = 0; t < routes.failed.size(); ++t) {
    if (routes.failed[t] == 0) { continue; }
    diags.push_back({ .code = DiagCode::RouteDegraded,
                      .subject = { .kind = ElemKind::Transition, .ordinal = t },
                      .doc = { INVALID },
                      .src = {} });
  }

  write_columns(c, sized, routes, inputs_digest(s, o));
  return true;
}

namespace {

// The column's rows, memcpy'd out so hashing never reads padding in place.
template <typename T>
std::vector<T> rows_of(Chart const &c, char const *name) {
  ColumnId const id{ column_find(c, name) };
  if (id.v == INVALID) { return {}; }
  std::vector<T> rows(column_count(c, id));
  if (!rows.empty()) {
    std::memcpy(rows.data(), column_data(c, id), rows.size() * sizeof(T));
  }
  return rows;
}

// 0, 1, or 2: decreasing, level, or increasing along one axis.
uint32_t direction_token(int32_t from, int32_t to) {
  if (to > from) { return 2U; }
  return (to < from) ? 0U : 1U;
}

}  // namespace

uint32_t layout_inputs_digest(Chart const &c) {
  ColumnId const id{ column_find(c, "scav.geom.inputs") };
  if ((id.v == INVALID) || (column_count(c, id) == 0)) { return 0; }
  uint32_t inputs{ 0 };
  std::memcpy(&inputs, column_data(c, id), 4);
  return inputs;
}

uint32_t layout_coordinate_hash(Chart const &c) {
  std::vector<scav_byte> b;
  for (char const *name : { "scav.geom.state",
                            "scav.geom.state_before",
                            "scav.geom.state_after",
                            "scav.geom.sub",
                            "scav.geom.chart" }) {
    for (scav_rect const &r : rows_of<scav_rect>(c, name)) {
      append_i32(b, r.x);
      append_i32(b, r.y);
      append_i32(b, r.w);
      append_i32(b, r.h);
    }
  }
  for (scav_point const &pt : rows_of<scav_point>(c, "scav.geom.point")) {
    append_i32(b, pt.x);
    append_i32(b, pt.y);
  }
  for (scav_port_slot const &sl : rows_of<scav_port_slot>(c, "scav.geom.portslot")) {
    append_i32(b, sl.x);
    append_i32(b, sl.y);
  }
  return xxhash32(b.data(), b.size(), 0);
}

bool layout_trace_json(Chart &c,
                       scav_spaces const &s,
                       scav_layout_opts const &o,
                       std::vector<scav_placed> &placed,
                       std::vector<Diagnostic> &diags,
                       std::vector<char> &out,
                       uint32_t row,
                       TraceScope scope) {
  if (scope == TraceScope::Search) {
    LayoutTrace t;
    scav_layout_opts serial{ o };
    serial.threads = 1;
    trace_sink_set(&t);
    bool const ran{ layout_run(c, s, serial, placed, diags, nullptr, nullptr, row) };
    trace_sink_set(nullptr);
    trace_to_json(t, c, out);
    return ran;
  }

  // Search first and keep what won, so the trace below is of the one drawing
  // that ships rather than of every candidate the search threw away (11.16).
  uint32_t won{ 0 };
  SearchPins pins;
  if (!layout_run(c, s, o, placed, diags, nullptr, &won, row, nullptr, &pins)) {
    trace_to_json({}, c, out);
    return false;
  }

  // The tuple and the pins together name that drawing, so nothing is left to
  // search: one thread for a deterministic event order, no moves to score.
  scav_layout_opts serial{ o };
  serial.threads = 1;
  serial.profile.portfolio_k = 0;
  LayoutTrace t;
  trace_sink_set(&t);
  std::vector<Diagnostic> again;
  bool const laid{
    layout_run(c, s, serial, placed, again, nullptr, nullptr, won, nullptr, nullptr,
               &pins)
  };
  trace_sink_set(nullptr);
  trace_to_json(t, c, out);
  return laid;
}

uint32_t layout_structural_hash(Chart const &c) {
  std::vector<scav_byte> b;
  std::vector<scav_point> const points{ rows_of<scav_point>(c, "scav.geom.point") };
  for (scav_span const route : rows_of<scav_span>(c, "scav.geom.route")) {
    append_u32(b, route.len);
    // Direction tokens, not coordinates: a translation leaves these alone.
    for (uint32_t k = 0; (k + 1) < route.len; ++k) {
      scav_point const &a{ points[route.off + k] };
      scav_point const &d{ points[route.off + k + 1] };
      append_u32(b, (direction_token(a.x, d.x) * 3U) + direction_token(a.y, d.y));
    }
  }
  for (scav_port_slot const &sl : rows_of<scav_port_slot>(c, "scav.geom.portslot")) {
    append_u32(b, sl.side);
    append_u32(b, sl.boundary_depth);
  }
  return xxhash32(b.data(), b.size(), chart_structural_hash(c));
}

}  // namespace scav
