// Runs the corpus through measure with the bundled font, layout, build,
// canonicalize and hash, and checks the results against the goldens.

#include "core/tests/corpus.h"
#include "layout/cost.h"
#include "layout/decompose.h"
#include "layout/label.h"
#include "layout/size.h"
#include "scav/scav_core.h"
#include "scav/scav_draw.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace scav {

void layout_test_label_bound(bool on, bool verify);
uint64_t layout_test_label_bound_skipped();
uint64_t layout_test_label_bound_labelled();
uint64_t layout_test_label_bound_mismatches();

}  // namespace scav

namespace {

using namespace scav;

// The corpus, in the order the layout golden lists it.
constexpr std::array<char const *, 12> CORPUS{
  "axis.scav", "bottler.scav", "brew.scav", "dock.scav", "estop.scav",       "kiln.scav",
  "led.scav",  "mill.scav",    "ota.scav",  "tcp.scav",  "toolchanger.scav", "vac.scav"
};

Metrics bundled() {
  Metrics m;
  REQUIRE(metrics_create(nullptr, 0, m));
  return m;
}

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

scav_profile compact() {
  scav_profile p{};
  REQUIRE(profile_named("compact", p));
  return p;
}

Chart load_corpus(char const *name) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  Chart c;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE_MESSAGE(load_file(path.c_str(), loader, c, diags, failed), path);
  return c;
}

// One corpus chart after measure, layout and emit, with each stage's output.
struct Run {
  Chart chart;
  Spaces spaces;
  std::vector<scav_placed> placed;
  DrawList list;
};

// `pinned` lays a corpus chart out from its committed pins, with no search.
Run run_pipeline(char const *name, Metrics const &m, scav_profile const &p, bool pinned) {
  Run r{ .chart = load_corpus(name), .spaces = {}, .placed = {}, .list = {} };
  REQUIRE_MESSAGE(measure_chart(r.chart, m, p, r.spaces), name);
  std::vector<Diagnostic> diags;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  bool const laid{ pinned
                       ? scav::test::corpus_layout(r.chart,
                                                   name,
                                                   as_spaces(r.spaces),
                                                   opts,
                                                   r.placed,
                                                   diags)
                       : layout_run(r.chart, as_spaces(r.spaces), opts, r.placed, diags) };
  std::string why{ name };
  for (Diagnostic const &d : diags) {
    why += ": ";
    why += diag_message(d.code);
  }
  REQUIRE_MESSAGE(laid, why);
  REQUIRE_MESSAGE(emit_chart(r.list,
                             r.chart,
                             m,
                             palette_standard(),
                             as_spaces(r.spaces),
                             r.placed.data(),
                             static_cast<uint32_t>(r.placed.size()),
                             0),
                  name);
  uint32_t bad{ 0 };
  REQUIRE_MESSAGE(drawlist_validate(r.list, bad), name);
  drawlist_canonicalize(r.list);
  return r;
}

// The pipeline over one chart at `readable` with the bundled font, from its
// committed pins, once per process.
Run const &laid_pipeline(char const *name) {
  static std::map<std::string, Run> laid;
  auto const found{ laid.find(name) };
  if (found != laid.end()) { return found->second; }
  Metrics const m{ bundled() };
  return laid.emplace(name, run_pipeline(name, m, readable(), true)).first->second;
}

// The column's rows, memcpy'd out so nothing reads padding in place.
template <typename T>
std::vector<T> rows(Chart const &c, char const *name) {
  ColumnId const id{ column_find(c, name) };
  if (id.v == INVALID) { return {}; }
  std::vector<T> out(column_count(c, id));
  if (!out.empty()) {
    std::memcpy(out.data(), column_data(c, id), out.size() * sizeof(T));
  }
  return out;
}

}  // namespace

TEST_CASE(
    "drawlist corpus: under real text every bounded round picks what scoring it whole "
    "picks" *
    doctest::test_suite("full")) {
  // Verification scores each round whole and requires the same pick at the same cost,
  // no bound above a cost, and labelled candidates matching a full layout.
  struct Restore {
    Restore() = default;
    Restore(Restore const &) = delete;
    Restore &operator=(Restore const &) = delete;
    ~Restore() { layout_test_label_bound(true, false); }
  } const restore;
  Metrics const m{ bundled() };
  for (char const *name :
       { "brew.scav", "dock.scav", "ota.scav", "vac.scav", "tcp.scav" }) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    layout_test_label_bound(true, true);
    (void)run_pipeline(name, m, readable(), false);
    CHECK(layout_test_label_bound_skipped() > 0);
    CHECK(layout_test_label_bound_labelled() > 0);
    CHECK(layout_test_label_bound_mismatches() == 0);
  }
}

TEST_CASE("drawlist corpus: under real text the search reaches the committed pins") {
  // Every other real-text corpus case lays out from these pins with the search off.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };
  std::string actual;
  for (char const *name : CORPUS) {
    if (!scav::test::corpus_searched(name)) {
      actual += scav::test::corpus_pins_of(name, true);  // taken as committed
      continue;
    }
    CAPTURE(name);
    Chart c{ load_corpus(name) };
    Spaces spaces;
    REQUIRE(measure_chart(c, m, p, spaces));
    std::vector<scav_placed> placed;
    std::vector<Diagnostic> diags;
    scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
    uint32_t row{ INVALID };
    SearchPins taken;
    REQUIRE(layout_run(c,
                       as_spaces(spaces),
                       opts,
                       placed,
                       diags,
                       nullptr,
                       &row,
                       INVALID,
                       nullptr,
                       &taken));
    actual += scav::test::corpus_pins_line(name, row, taken, true);
  }

  std::string const want{ scav::test::corpus_pins_at(scav::test::corpus_pins_file(),
                                                     true) };
  if (want != actual) {
    scav::test::corpus_pins_write(actual, true);
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_pins.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("drawlist corpus: every chart builds and hashes to the committed golden") {
  Metrics const m{ bundled() };

  std::string actual;
  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Run const &r{ laid_pipeline(name) };
    actual += name;
    actual += ' ';
    string_append_hex32(actual, drawlist_digest(r.list, m));
    actual += ' ';
    string_append_u32(actual, static_cast<uint32_t>(r.list.prims.size()));
    actual += '\n';
  }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/drawlist/corpus.txt", golden));
  std::string const want{ scav::test::corpus_golden(
      { reinterpret_cast<char const *>(golden.data()), golden.size() }) };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/drawlist_corpus.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/drawlist_corpus.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("drawlist corpus: the layout hashes under the reference measurement") {
  // Layout's corpus hashes taken against the reference builder's measurement.
  Metrics const m{ bundled() };

  std::string actual;
  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Run const &r{ laid_pipeline(name) };
    actual += name;
    actual += ' ';
    string_append_hex32(actual, layout_inputs_digest(r.chart));
    actual += ' ';
    string_append_hex32(actual, layout_structural_hash(r.chart));
    actual += ' ';
    string_append_hex32(actual, layout_coordinate_hash(r.chart));
    actual += '\n';
  }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/layout/corpus_measured.txt", golden));
  std::string const want{ scav::test::corpus_golden(
      { reinterpret_cast<char const *>(golden.data()), golden.size() }) };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_measured.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_measured.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("drawlist corpus: the cost terms on the rendered scale") {
  // Cost terms and shares on the real-text scale. `label` and the path-box part of
  // `excess_len` are nonzero only under real-text measurement.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };

  std::string actual;
  std::string shares;
  // `label` and `label_near` cost O(placed x states) and O(placed x pieces); timed
  // here, not asserted.
  int64_t scoring_us{ 0 };
  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Run const &r{ laid_pipeline(name) };
    SplitGraph const g{ decompose(r.chart) };
    auto const t0{ std::chrono::steady_clock::now() };
    CostTerms const t{ cost_columns(r.chart, g, p, as_spaces(r.spaces), r.placed) };
    auto const t1{ std::chrono::steady_clock::now() };
    scoring_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    Cost const scored{ cost_of(t, p) };
    CHECK(scored.t0_violations == 0);  // every corpus chart ships clean
    actual += name;
    for (int64_t const term : { int64_t{ scored.t0_violations },
                                t.bends,
                                t.corridor,
                                t.crossings,
                                t.excess_len,
                                t.adjacency,
                                t.label,
                                t.label_near,
                                t.aspect,
                                t.area,
                                t.length,
                                t.transit_bends,
                                t.whitespace,
                                scored.t2 }) {
      actual += ' ';
      actual += std::to_string(term);
    }
    actual += '\n';

    shares += name;
    for (int64_t const bp : cost_shares(t, p)) {
      shares += ' ';
      shares += std::to_string(bp);
    }
    shares += '\n';
  }
  MESSAGE("cost_columns over the corpus under real text: ", scoring_us, " us");

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/layout/corpus_cost_measured.txt", golden));
  std::string const want{ scav::test::corpus_golden(
      { reinterpret_cast<char const *>(golden.data()), golden.size() }) };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_cost_measured.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_cost_measured.txt:\n", actual);
  }
  CHECK(want == actual);

  std::vector<scav_byte> shares_golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/layout/corpus_cost_shares_measured.txt",
                    shares_golden));
  std::string const want_shares{ scav::test::corpus_golden(
      { reinterpret_cast<char const *>(shares_golden.data()), shares_golden.size() }) };
  if (want_shares != shares) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_cost_shares_measured.txt",
               reinterpret_cast<scav_byte const *>(shares.data()),
               shares.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_cost_shares_measured.txt:\n",
            shares);
  }
  CHECK(want_shares == shares);
}

TEST_CASE("drawlist gauntlet: what crowd's tighter packing costs its labels") {
  // Under real text, crowd's label boxes overlap no box or enclosing state band, and
  // each sits nearer its own route than any other.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };
  Run const r{ run_pipeline("gauntlet/crowd.scav", m, p, false) };
  CostTerms const t{
    cost_columns(r.chart, decompose(r.chart), p, as_spaces(r.spaces), r.placed)
  };
  CHECK(t.label == 0);
  CHECK(t.label_near == 0);
}

TEST_CASE("drawlist corpus: the strips the labels landed on, and what fell back") {
  // Placement re-run from the emitted columns: the check that the columns carry
  // everything it reads, and the corpus's count of boxes that found no strip.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };

  uint32_t fell{ 0 };
  uint32_t anchored{ 0 };
  uint32_t boxes{ 0 };
  for (char const *name : CORPUS) {
    if (scav::test::corpus_skipped(name)) { continue; }
    CAPTURE(name);
    Run const &r{ laid_pipeline(name) };
    SizedLayout z;
    z.state = rows<scav_rect>(r.chart, "scav.geom.state");
    z.before = rows<scav_rect>(r.chart, "scav.geom.state_before");
    z.after = rows<scav_rect>(r.chart, "scav.geom.state_after");
    z.sub = rows<scav_rect>(r.chart, "scav.geom.sub");
    std::vector<scav_rect> const chart{ rows<scav_rect>(r.chart, "scav.geom.chart") };
    REQUIRE(chart.size() == 1);
    z.chart = chart[0];

    std::vector<scav_rect> again;
    fell += place_labels(r.chart,
                         decompose(r.chart),
                         z,
                         as_spaces(r.spaces),
                         rows<scav_span>(r.chart, "scav.geom.route"),
                         rows<scav_point>(r.chart, "scav.geom.point"),
                         p,
                         again);
    boxes += static_cast<uint32_t>(again.size());
    REQUIRE(again.size() == r.placed.size());
    // Every placed box lies inside the final chart rect.
    for (scav_rect const &at : r.placed) {
      CHECK(at.x >= z.chart.x);
      CHECK(at.y >= z.chart.y);
      CHECK((at.x + at.w) <= (z.chart.x + z.chart.w));
      CHECK((at.y + at.h) <= (z.chart.y + z.chart.h));
    }

    // Each box's Chebyshev gap to its own route is at most `label_leader`.
    scav_spaces const sp{ as_spaces(r.spaces) };
    std::vector<scav_span> const routes{ rows<scav_span>(r.chart, "scav.geom.route") };
    std::vector<scav_point> const pts{ rows<scav_point>(r.chart, "scav.geom.point") };
    for (uint32_t i = 0; i < again.size(); ++i) {
      if (i >= sp.n_path_box) { continue; }
      uint32_t const subject{ sp.path_box[i].subject };
      if (subject >= routes.size()) { continue; }
      scav_span const route{ routes[subject] };
      if (route.len < 2) { continue; }
      Wide nearest{ -1 };
      for (uint32_t k = 0; (k + 1) < route.len; ++k) {
        Wide const away{
          chebyshev_gap(again[i], span_rect(pts[route.off + k], pts[route.off + k + 1]))
        };
        nearest = (nearest < 0) ? away : imin(nearest, away);
      }
      CAPTURE(name);
      CAPTURE(subject);
      // The bound applies to fallback boxes too.
      CHECK(nearest <= Wide{ label_leader(p) });
      anchored += (nearest <= Wide{ label_leader(p) }) ? 1U : 0U;
    }
  }
  MESSAGE("corpus path boxes: ",
          boxes,
          ", centred fallbacks: ",
          fell,
          ", within the leader: ",
          anchored);
  CHECK(anchored == boxes);
  if (!scav::test::corpus_skipped("mill.scav")) { CHECK(boxes == 223); }
  CHECK(fell == 0);
}

TEST_CASE("drawlist corpus: the layout goldens' measurement policy is stated here") {
  // Layout goldens use all-zero spaces; the measured run must differ from them in
  // inputs digest and coordinates.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };
  Chart measured{ load_corpus("vac.scav") };
  Chart unmeasured{ load_corpus("vac.scav") };

  Spaces spaces;
  REQUIRE(measure_chart(measured, m, p, spaces));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  REQUIRE(scav::test::corpus_layout(measured,
                                    "vac.scav",
                                    as_spaces(spaces),
                                    opts,
                                    placed,
                                    diags));
  REQUIRE(scav::test::corpus_layout(unmeasured, "vac.scav", {}, opts, placed, diags));

  CHECK(layout_inputs_digest(measured) != layout_inputs_digest(unmeasured));
  // Real text makes every box bigger, so the coordinates move.
  CHECK(layout_coordinate_hash(measured) != layout_coordinate_hash(unmeasured));
}

TEST_CASE("drawlist corpus: a second font is a second picture at the same layout") {
  // A different metrics identity changes the drawlist digest; the font reaches layout
  // only through the space tables.
  Metrics const m{ bundled() };
  Metrics doppelganger{ bundled() };
  doppelganger.identity ^= 0xFFFFU;  // same tables, a different identity

  Run const &r{ laid_pipeline("vac.scav") };
  CHECK(drawlist_digest(r.list, m) != drawlist_digest(r.list, doppelganger));
}

TEST_CASE("drawlist corpus: canonical form is reached from any emission order") {
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };
  Chart c{ load_corpus("tcp.scav") };
  Spaces spaces;
  REQUIRE(measure_chart(c, m, p, spaces));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  REQUIRE(
      scav::test::corpus_layout(c, "tcp.scav", as_spaces(spaces), opts, placed, diags));

  DrawList wrapper;
  REQUIRE(emit_chart(wrapper,
                     c,
                     m,
                     palette_standard(),
                     as_spaces(spaces),
                     placed.data(),
                     static_cast<uint32_t>(placed.size()),
                     0));

  // The same picture, emitted by calling the per-kind emitters in an order the
  // convenience wrapper does not use: routes and labels first, then the boxes.
  DrawList by_hand;
  Palette const palette{ palette_standard() };
  for (uint32_t i = 0; i < c.transitions.size(); ++i) {
    scav_rect box{};
    if (label_box(c,
                  as_spaces(spaces),
                  placed.data(),
                  static_cast<uint32_t>(placed.size()),
                  i,
                  box)) {
      emit_label(by_hand, c, m, palette, i, box, 0);
    }
  }
  for (auto i = static_cast<uint32_t>(c.transitions.size()); i-- > 0;) {
    emit_route(by_hand, as_spaces(spaces), c, palette, i, 0);
  }
  for (auto i = static_cast<uint32_t>(c.states.size()); i-- > 0;) {
    emit_state(by_hand, c, m, palette, i, 0);
  }
  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    emit_submachine(by_hand, c, palette, i, 0);
  }

  drawlist_canonicalize(wrapper);
  drawlist_canonicalize(by_hand);
  CHECK(wrapper.prims.size() == by_hand.prims.size());
  CHECK(drawlist_digest(wrapper, m) == drawlist_digest(by_hand, m));
}

TEST_CASE("drawlist corpus: tcp's long hierarchical edges reach the drawlist") {
  // tcp has transitions from a nested concurrent submachine to a top-level state;
  // each live transition draws one polyline.
  Metrics const m{ bundled() };
  Run const &r{ laid_pipeline("tcp.scav") };

  uint32_t live{ 0 };
  for (Transition const &t : r.chart.transitions) {
    if (t.live != 0U) { ++live; }
  }
  REQUIRE(live > 0);

  uint32_t drawn{ 0 };
  for (scav_prim const &prim : r.list.prims) {
    if ((prim.kind == SCAV_PRIM_POLYLINE) &&
        (prim.origin_kind == static_cast<uint32_t>(ElemKind::Transition))) {
      ++drawn;
    }
  }
  CHECK(drawn == live);
}

TEST_CASE("drawlist corpus: the extent estimate holds under the real font") {
  // Checks the coordinate-domain estimate with the bundled font on a 2k-state chart.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };

  Chart c;
  SubmachineId parent{ build_chart(c, "extent", {}) };
  for (uint32_t level = 0; level < 16; ++level) {
    StateId const composite{
      build_state(c, parent, "Composite" + std::to_string(level), StateKind::Normal, {})
    };
    for (uint32_t i = 0; i < 128; ++i) {
      // Names as long as the longest in a real chart.
      build_state(c,
                  parent,
                  "WaitingForAcknowledgement" + std::to_string(i),
                  StateKind::Normal,
                  {});
    }
    parent = build_submachine(c, composite, "main", {});
  }
  REQUIRE(c.states.size() >= 2000);

  Spaces spaces;
  REQUIRE(measure_chart(c, m, p, spaces));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  REQUIRE(layout_run(c, as_spaces(spaces), opts, placed, diags));

  ColumnId const id{ column_find(c, "scav.geom.chart") };
  REQUIRE(id.v != INVALID);
  scav_rect extent{};
  std::memcpy(&extent, column_data(c, id), sizeof(extent));
  MESSAGE("2k-state real-font extent: ", extent.w, " x ", extent.h, " of ", COORD_MAX);
  CHECK(extent.w <= ((COORD_MAX / 4) * 3));
  CHECK(extent.h <= ((COORD_MAX / 4) * 3));
}

TEST_CASE("drawlist: layout's line height is draw's, over the whole domain") {
  // Layout's `label_line_height` must equal draw's `line_height` over the domain.
  auto const agree = [](int32_t size, int32_t num, int32_t den) {
    scav_profile p{ readable() };
    p.font_size_grid = size;
    p.line_height_k_num = num;
    p.line_height_k_den = den;
    CAPTURE(size);
    CAPTURE(num);
    CAPTURE(den);
    CHECK(label_line_height(p) == line_height(size, num, den));
  };
  for (scav_profile const &p : { readable(), compact() }) {
    agree(p.font_size_grid, p.line_height_k_num, p.line_height_k_den);
  }
  // The corners, where a restatement diverges: both ratio bounds, either side.
  for (int32_t const k : { 0, 1, 2, 1023, 1024, 1025 }) {
    agree(192, k, 5);
    agree(192, 7, k);
  }
  for (int32_t const size : { -1, 0, 1, 16, COORD_MAX / 4, COORD_MAX, COORD_MAX / 2 }) {
    agree(size, 7, 5);
    agree(size, 1024, 1);
  }
  // Checks rounding up at each remainder of `size / 5`.
  for (int32_t const size : { 10, 11, 12, 13, 14 }) { agree(size, 1, 5); }
  CHECK(label_line_height(readable()) == 269);  // 192 * 7/5
  CHECK(label_line_height(compact()) == 192);   // 160 * 6/5
}

TEST_CASE("drawlist corpus: a 2k-state chart builds, and quickly") {
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };

  // Depth 16, and every state named, so the measurement pass does real work.
  Chart c;
  SubmachineId parent{ build_chart(c, "big", {}) };
  std::vector<SubmachineId> frames{ parent };
  for (uint32_t level = 0; level < 16; ++level) {
    StateId const composite{
      build_state(c, parent, "Level" + std::to_string(level), StateKind::Normal, {})
    };
    for (uint32_t i = 0; i < 128; ++i) {
      build_state(c, parent, "State" + std::to_string(i), StateKind::Normal, {});
    }
    parent = build_submachine(c, composite, "main", {});
    frames.push_back(parent);
  }
  REQUIRE(c.states.size() >= 2000);

  Spaces spaces;
  REQUIRE(measure_chart(c, m, p, spaces));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  REQUIRE(layout_run(c, as_spaces(spaces), opts, placed, diags));

  DrawList d;
  REQUIRE(emit_chart(d,
                     c,
                     m,
                     palette_standard(),
                     as_spaces(spaces),
                     placed.data(),
                     static_cast<uint32_t>(placed.size()),
                     0));
  uint32_t bad{ 0 };
  REQUIRE(drawlist_validate(d, bad));
  drawlist_canonicalize(d);
  CHECK(d.prims.size() >= c.states.size());
  MESSAGE("2k-state drawlist: ", d.prims.size(), " primitives");
}
