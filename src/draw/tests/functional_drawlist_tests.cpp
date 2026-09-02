// The corpus through the whole pipeline: measure with the bundled font, lay
// out, build, canonicalize, hash. This is where the measurement policy the
// layout goldens are stated against finally exists.

#include "scav/scav_core.h"
#include "scav/scav_draw.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace scav;

// The corpus, in the order the layout golden lists it.
constexpr std::array<char const *, 11> CORPUS{
  "axis.scav", "bottler.scav", "brew.scav", "dock.scav",        "estop.scav", "led.scav",
  "mill.scav", "ota.scav",     "tcp.scav",  "toolchanger.scav", "vac.scav"
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

// One chart, all the way through. The pipeline every corpus golden is produced
// by, and the one an application writes for itself.
struct Run {
  Chart chart;
  Spaces spaces;
  std::vector<scav_placed> placed;
  DrawList list;
};

Run run_pipeline(char const *name, Metrics const &m, scav_profile const &p) {
  Run r{ .chart = load_corpus(name), .spaces = {}, .placed = {}, .list = {} };
  REQUIRE_MESSAGE(measure_chart(r.chart, m, p, r.spaces), name);
  std::vector<Diagnostic> diags;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  bool const laid{ layout_run(r.chart, as_spaces(r.spaces), opts, r.placed, diags) };
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

}  // namespace

TEST_CASE("drawlist corpus: every chart builds and hashes to the committed golden") {
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };

  std::string actual;
  for (char const *name : CORPUS) {
    CAPTURE(name);
    Run const r{ run_pipeline(name, m, p) };
    actual += name;
    actual += ' ';
    string_append_hex32(actual, drawlist_digest(r.list, m));
    actual += ' ';
    string_append_u32(actual, static_cast<uint32_t>(r.list.prims.size()));
    actual += '\n';
  }

  std::vector<scav_byte> golden;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/golden/drawlist/corpus.txt", golden));
  std::string const want{ reinterpret_cast<char const *>(golden.data()), golden.size() };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/drawlist_corpus.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/drawlist_corpus.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("drawlist corpus: the layout hashes under the reference measurement") {
  // A second layout golden, and the one section 6 actually asks for: the same
  // hashes as layout's own, but taken against the reference builder's
  // measurement rather than no requests at all. It lives here because layout
  // cannot depend on draw, so its own suite has no way to measure anything.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };

  std::string actual;
  for (char const *name : CORPUS) {
    CAPTURE(name);
    Run const r{ run_pipeline(name, m, p) };
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
  std::string const want{ reinterpret_cast<char const *>(golden.data()), golden.size() };
  if (want != actual) {
    write_file(SCAV_TEST_OUT_DIR "/corpus_measured.txt",
               reinterpret_cast<scav_byte const *>(actual.data()),
               actual.size());
    MESSAGE("actual written to " SCAV_TEST_OUT_DIR "/corpus_measured.txt:\n", actual);
  }
  CHECK(want == actual);
}

TEST_CASE("drawlist corpus: the layout goldens' measurement policy is stated here") {
  // The layout goldens are hashed against all-zero spaces, and this pass is
  // what a real application hands layout instead. Both are legitimate policies;
  // the point is that the digest tells them apart rather than leaving a reader
  // to guess which one produced a hash.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };
  Chart measured{ load_corpus("vac.scav") };
  Chart unmeasured{ load_corpus("vac.scav") };

  Spaces spaces;
  REQUIRE(measure_chart(measured, m, p, spaces));
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_layout_opts const opts{ .profile = p, .router = 0, .threads = 0 };
  REQUIRE(layout_run(measured, as_spaces(spaces), opts, placed, diags));
  REQUIRE(layout_run(unmeasured, {}, opts, placed, diags));

  CHECK(layout_inputs_digest(measured) != layout_inputs_digest(unmeasured));
  // Real text makes every box bigger, so the coordinates move.
  CHECK(layout_coordinate_hash(measured) != layout_coordinate_hash(unmeasured));
}

TEST_CASE("drawlist corpus: a second font is a second picture at the same layout") {
  // Two metrics over one chart. The font is not a layout argument, so it
  // reaches the layout hash only through the space tables -- and reaches the
  // drawlist digest directly, which is the split this exists to demonstrate.
  Metrics const m{ bundled() };
  Metrics doppelganger{ bundled() };
  doppelganger.identity ^= 0xFFFFU;  // same tables, a different identity

  scav_profile const p{ readable() };
  Run const r{ run_pipeline("vac.scav", m, p) };
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
  REQUIRE(layout_run(c, as_spaces(spaces), opts, placed, diags));

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
    emit_route(by_hand, c, palette, i, 0);
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
  // The chart the whole project exists for: transitions out of a nested
  // concurrent submachine to a top-level state. Every one of them has to draw.
  Metrics const m{ bundled() };
  Run const r{ run_pipeline("tcp.scav", m, readable()) };

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
  CHECK(drawn == live);  // nothing dropped, which is the incumbent's failure
}

TEST_CASE("drawlist corpus: the extent estimate holds under the real font") {
  // P4 validated the coordinate domain against deliberately fat fabricated
  // advances. This is the same question asked of the font that actually ships,
  // which is the only measurement any golden is against.
  Metrics const m{ bundled() };
  scav_profile const p{ readable() };

  Chart c;
  SubmachineId parent{ build_chart(c, "extent", {}) };
  for (uint32_t level = 0; level < 16; ++level) {
    StateId const composite{
      build_state(c, parent, "Composite" + std::to_string(level), StateKind::Normal, {})
    };
    for (uint32_t i = 0; i < 128; ++i) {
      // Names as long as a real chart's deepest, so the measurement is not
      // flattered by short ones.
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
  // The same bar P4 set for itself, so a change that eats the margin trips in
  // both places rather than only the fabricated one.
  CHECK(extent.w <= ((COORD_MAX / 4) * 3));
  CHECK(extent.h <= ((COORD_MAX / 4) * 3));
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
