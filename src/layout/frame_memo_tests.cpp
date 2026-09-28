// The per-frame memos of ordering and sizing against the steps they stand in
// for. A traced run derives every frame and remembers nothing, so the same call
// with a sink attached is the uncached answer. Each case warms a memo on one
// input, changes one thing its key must cover, and requires the remembered
// answer to be the computed one: a key missing that input would hand back the
// frame as it was before the change.

#include "layout/decompose.h"
#include "layout/order.h"
#include "layout/pack.h"
#include "layout/size.h"
#include "layout/tests/pod_eq.h"
#include "layout/trace.h"
#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace scav;

scav_profile readable() {
  scav_profile p{};
  REQUIRE(profile_named("readable", p));
  return p;
}

void load(char const *name, Chart &c) {
  std::string path{ SCAV_TEST_DATA_DIR "/charts/" };
  path += name;
  Loader loader;
  std::vector<Diagnostic> diags;
  std::string failed;
  REQUIRE(load_file(path.c_str(), loader, c, diags, failed));
}

// Space requests on every state and a label box on every transition, so the
// sizes and label extents the keys carry have values to change.
struct Requests {
  std::vector<scav_box_space> box;
  std::vector<scav_path_box> path;
  [[nodiscard]] scav_spaces view() const {
    return { .box_state = box.data(),
             .n_box_state = static_cast<uint32_t>(box.size()),
             .box_state_stride = sizeof(scav_box_space),
             .path_box = path.data(),
             .n_path_box = static_cast<uint32_t>(path.size()),
             .path_box_stride = sizeof(scav_path_box) };
  }
};

Requests requests_for(Chart const &c) {
  Requests r;
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    r.box.push_back({ .min_w = 40 + static_cast<int32_t>((i % 5U) * 10U),
                      .h_before = static_cast<int32_t>((i % 3U) * 8U),
                      .h_after = static_cast<int32_t>((i % 2U) * 6U) });
  }
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    if (c.transitions[t].live == 0) { continue; }
    r.path.push_back({ .subject = t,
                       .w = 30 + static_cast<int32_t>((t % 4U) * 12U),
                       .h = 12 + static_cast<int32_t>((t % 3U) * 4U),
                       .order = 0 });
  }
  return r;
}

bool same(SubmachineOrders const &a, SubmachineOrders const &b) {
  return (a.nodes == b.nodes) && (a.edges == b.edges) && (a.sub_nodes == b.sub_nodes) &&
         (a.sub_edges == b.sub_edges) && (a.sub_ranks == b.sub_ranks) &&
         (a.sub_down == b.sub_down) && (a.sub_gaps == b.sub_gaps) && (a.gaps == b.gaps) &&
         (a.state_node == b.state_node) && (a.seg_node == b.seg_node) &&
         (a.seg_port == b.seg_port) && (a.seg_cyclic == b.seg_cyclic);
}

bool same(SizedLayout const &a, SizedLayout const &b) {
  return same_rows(a.state, b.state) && same_rows(a.before, b.before) &&
         same_rows(a.after, b.after) && same_rows(a.sub, b.sub) &&
         same_rows(a.node, b.node) && (a.chart == b.chart);
}

SubmachineOrders ordered(Chart const &c,
                         SplitGraph const &g,
                         scav_spaces const &s,
                         scav_profile const &p,
                         SearchPins const &pins,
                         bool traced) {
  LayoutTrace t;
  if (traced) { trace_sink_set(&t); }
  SubmachineOrders o{ order_submachines(c, g, s, p, 1, pins) };
  trace_sink_set(nullptr);
  return o;
}

struct Sized {
  SizedLayout z;
  bool ok{ false };
};

Sized sized(Chart const &c,
            SplitGraph const &g,
            SubmachineOrders const &o,
            scav_spaces const &s,
            scav_profile const &p,
            DarSource dar,
            Compaction pack,
            Fold fold,
            bool traced) {
  LayoutTrace t;
  if (traced) { trace_sink_set(&t); }
  Sized out;
  std::vector<Diagnostic> diags;
  out.ok = size_layout(c, g, o, s, p, out.z, diags, dar, pack, fold);
  trace_sink_set(nullptr);
  return out;
}

// The first live state with a parent frame of two ranks or more, a cyclic
// segment's transition and leg, and a frame of two ranks, so each pin names
// something the ordering can act on.
struct Targets {
  StateId state{ INVALID };
  TransId trans{ INVALID };
  uint32_t leg{ 0 };
  TransId chained{ INVALID };
  uint32_t chained_leg{ 0 };
  SubmachineId frame{ INVALID };
};

Targets targets_of(Chart const &c, SplitGraph const &g, SubmachineOrders const &o) {
  Targets t;
  for (uint32_t m = 0; m < o.sub_ranks.size(); ++m) {
    if ((o.sub_ranks[m] >= 2) && (t.frame.v == INVALID)) { t.frame = SubmachineId{ m }; }
  }
  for (uint32_t i = 0; i < c.states.size(); ++i) {
    StateKind const k{ c.states[i].kind };
    if ((c.states[i].live == 0) || (k == StateKind::Initial) ||
        (o.state_node[i] == INVALID)) {
      continue;
    }
    t.state = StateId{ i };
    break;
  }
  for (uint32_t seg = 0; seg < g.segments.size(); ++seg) {
    TransId const tr{ g.segments[seg].trans };
    if ((o.seg_cyclic[seg] == 0) || (tr.v == INVALID)) { continue; }
    t.trans = tr;
    t.leg = seg - g.trans_segments[tr.v].off;
    break;
  }
  // A segment the ordering chained through a bend, which is the only kind a
  // cut acts on.
  for (OrderNode const &nd : o.nodes) {
    if ((nd.kind != OrderKind::Bend) || (nd.subject >= g.segments.size())) { continue; }
    TransId const tr{ g.segments[nd.subject].trans };
    if (tr.v == INVALID) { continue; }
    t.chained = tr;
    t.chained_leg = nd.subject - g.trans_segments[tr.v].off;
    break;
  }
  return t;
}

// Disconnected states of seeded sizes in one frame: each its own component,
// so the frame's packing of them is the step compaction changes.
struct Scatter {
  Chart c;
  Requests req;
};

Scatter scattered(uint32_t seed) {
  Scatter out;
  SubmachineId const root{ build_chart(out.c, "scatter", {}) };
  uint64_t s{ seed };
  auto const next = [&s](uint32_t n) {
    s = (s * 6364136223846793005ULL) + 1442695040888963407ULL;
    return static_cast<uint32_t>((s >> 33U) % n);
  };
  uint32_t const n{ 4 + next(4) };
  for (uint32_t i = 0; i < n; ++i) {
    build_state(out.c, root, "S" + std::to_string(i), StateKind::Normal, {});
    out.req.box.push_back({ .min_w = 60 + static_cast<int32_t>(next(500)),
                            .h_before = static_cast<int32_t>(next(400)),
                            .h_after = 0 });
  }
  return out;
}

// pack_tests' late arrival -- a fourth rect with nowhere but a row of its own
// until compaction takes it back into the hole under the third -- scaled from
// a separation of 10 to the profile's, as four disconnected states.
Scatter late_arrival(scav_profile const &p) {
  Scatter out;
  SubmachineId const root{ build_chart(out.c, "late", {}) };
  int32_t const k{ p.node_sep / 10 };
  std::array<std::pair<int32_t, int32_t>, 4> const rects{
    { { 500, 100 }, { 200, 400 }, { 100, 100 }, { 200, 400 } }
  };
  for (uint32_t i = 0; i < rects.size(); ++i) {
    build_state(out.c, root, "S" + std::to_string(i), StateKind::Normal, {});
    out.req.box.push_back({ .min_w = (rects[i].first * k) - (2 * p.pad),
                            .h_before = (rects[i].second * k) - (2 * p.pad),
                            .h_after = 0 });
  }
  return out;
}

constexpr std::array<char const *, 7> CHARTS{ "axis.scav", "bottler.scav",
                                              "brew.scav", "ota.scav",
                                              "tcp.scav",  "toolchanger.scav",
                                              "vac.scav" };

}  // namespace

TEST_CASE("order memo: a remembered frame is the frame those inputs order to") {
  scav_profile const p{ readable() };
  constexpr uint32_t VARIANTS{ 6 };
  std::vector<uint32_t> moved(VARIANTS, 0);
  for (char const *name : CHARTS) {
    CAPTURE(name);
    Chart c;
    load(name, c);
    SplitGraph const g{ decompose(c) };
    Requests const req{ requests_for(c) };
    scav_spaces const s{ req.view() };
    SubmachineOrders const base{ ordered(c, g, s, p, {}, false) };
    Targets const t{ targets_of(c, g, base) };

    // Each variant changes one input the ordering reads.
    std::vector<SearchPins> pins(5);
    if (t.state.v != INVALID) { pins[0].ranks.push_back({ .state = t.state, .rank = 2 }); }
    if (t.trans.v != INVALID) {
      pins[1].reverses.push_back({ .trans = t.trans, .leg = t.leg });
    }
    if (t.chained.v != INVALID) {
      pins[2].cuts.push_back({ .trans = t.chained, .leg = t.chained_leg });
    }
    if (t.frame.v != INVALID) { pins[3].orients.push_back({ .frame = t.frame }); }
    scav_profile swept{ p };
    swept.sweep_count = 0;
    Requests wider{ req };
    for (scav_path_box &b : wider.path) { b.w += 45; }

    struct Variant {
      SearchPins const *pins;
      scav_profile const *p;
      Requests const *req;
    };
    std::array<Variant, VARIANTS> const variants{
      { { .pins = pins.data(), .p = &p, .req = &req },
        { .pins = &pins[1], .p = &p, .req = &req },
        { .pins = &pins[2], .p = &p, .req = &req },
        { .pins = &pins[3], .p = &p, .req = &req },
        { .pins = &pins[4], .p = &swept, .req = &req },
        { .pins = &pins[4], .p = &p, .req = &wider } }
    };
    for (uint32_t k = 0; k < VARIANTS; ++k) {
      CAPTURE(k);
      Variant const &v{ variants[k] };
      scav_spaces const vs{ v.req->view() };
      SubmachineOrders const got{ ordered(c, g, vs, *v.p, *v.pins, false) };
      SubmachineOrders const want{ ordered(c, g, vs, *v.p, *v.pins, true) };
      CHECK(same(got, want));
      if (!same(want, base)) { ++moved[k]; }
    }
    CHECK(same(ordered(c, g, s, p, {}, false), base));
  }
  // Each variant reordered some chart, so each was a case the key had to tell
  // apart rather than one it could have ignored.
  for (uint32_t k = 0; k < VARIANTS; ++k) {
    CAPTURE(k);
    CHECK(moved[k] > 0);
  }
}

TEST_CASE("size memo: a remembered frame is the layout those inputs size to") {
  scav_profile const p{ readable() };
  constexpr uint32_t VARIANTS{ 13 };
  std::vector<uint32_t> moved(VARIANTS, 0);
  std::vector<Scatter> charts;
  for (char const *name : CHARTS) {
    Scatter x;
    load(name, x.c);
    x.req = requests_for(x.c);
    charts.push_back(std::move(x));
  }
  for (uint32_t seed = 1; seed <= 12; ++seed) { charts.push_back(scattered(seed)); }
  charts.push_back(late_arrival(p));
  for (Scatter const &x : charts) {
    Chart const &c{ x.c };
    CAPTURE(c.submachines.size());
    SplitGraph const g{ decompose(c) };
    Requests const &req{ x.req };
    scav_spaces const s{ req.view() };
    SubmachineOrders const o{ order_submachines(c, g, s, p, 1, {}) };
    Sized const base{
      sized(c, g, o, s, p, DarSource::Profile, Compaction::Off, Fold::Scale, false)
    };
    REQUIRE(base.ok);
    Targets const t{ targets_of(c, g, o) };

    // Orders that differ in one frame's ranks or direction, so the frames
    // around the changed one hit and the changed one must not.
    SearchPins turned;
    if (t.frame.v != INVALID) { turned.orients.push_back({ .frame = t.frame }); }
    SearchPins pinned;
    if (t.state.v != INVALID) { pinned.ranks.push_back({ .state = t.state, .rank = 2 }); }
    SubmachineOrders const o_turned{ order_submachines(c, g, s, p, 1, turned) };
    SubmachineOrders const o_pinned{ order_submachines(c, g, s, p, 1, pinned) };

    scav_profile node_sep{ p };
    node_sep.node_sep += 9;
    scav_profile rank_sep{ p };
    rank_sep.rank_sep += 9;
    scav_profile pad{ p };
    pad.pad += 4;
    scav_profile trybox{ p };
    trybox.trybox = (p.trybox != 0) ? 0 : 1;
    // Compaction moves only what the row packer did not win, so its variant is
    // weighed against the same profile without the box packer.
    scav_profile plain{ p };
    plain.trybox = 0;
    Sized const plain_base{
      sized(c, g, o, s, plain, DarSource::Profile, Compaction::Off, Fold::Scale, false)
    };
    scav_profile ratio{ p };
    ratio.dar_num = p.dar_den;
    ratio.dar_den = p.dar_num + 3;
    Requests wide_state{ req };
    for (scav_box_space &b : wide_state.box) { b.min_w += 60; }
    Requests tall_band{ req };
    for (scav_box_space &b : tall_band.box) { b.h_before += 20; }
    Requests tall_label{ req };
    for (scav_path_box &b : tall_label.path) { b.h += 30; }

    struct Variant {
      SubmachineOrders const *o;
      scav_profile const *p;
      Requests const *req;
      DarSource dar;
      Compaction pack;
      Fold fold;
    };
    std::array<Variant, VARIANTS> const variants{ {
        { .o = &o_turned,
          .p = &p,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o_pinned,
          .p = &p,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &node_sep,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &rank_sep,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &pad,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &trybox,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &ratio,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &p,
          .req = &wide_state,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &p,
          .req = &tall_band,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &p,
          .req = &tall_label,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &p,
          .req = &req,
          .dar = DarSource::OwnerHole,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &plain,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::On,
          .fold = Fold::Scale },
        { .o = &o,
          .p = &p,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Always },
    } };
    for (uint32_t k = 0; k < VARIANTS; ++k) {
      CAPTURE(k);
      Variant const &v{ variants[k] };
      scav_spaces const vs{ v.req->view() };
      Sized const got{ sized(c, g, *v.o, vs, *v.p, v.dar, v.pack, v.fold, false) };
      Sized const want{ sized(c, g, *v.o, vs, *v.p, v.dar, v.pack, v.fold, true) };
      CHECK(got.ok == want.ok);
      CHECK(same(got.z, want.z));
      SizedLayout const &before{ (v.p == &plain) ? plain_base.z : base.z };
      if (!same(want.z, before)) { ++moved[k]; }
    }
    Sized const again{
      sized(c, g, o, s, p, DarSource::Profile, Compaction::Off, Fold::Scale, false)
    };
    CHECK(same(again.z, base.z));
  }
  for (uint32_t k = 0; k < VARIANTS; ++k) {
    CAPTURE(k);
    CHECK(moved[k] > 0);
  }
}
