// The per-frame memos of ordering and sizing against the steps they stand in for. A traced
// run derives every frame and remembers nothing, which gives the uncached answer.

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

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scav {

void order_test_reuse(bool on);
void order_test_reuse_verify(bool on);
uint64_t order_test_reused();
uint64_t order_test_reuse_mismatches();

}  // namespace scav

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

// Space requests on every state and a label box on every transition.
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
         same_rows(a.node, b.node) && (a.lean == b.lean) && (a.folded == b.folded) &&
         (a.chart == b.chart);
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

// What each pin names: the first live non-initial state, a cyclic leg, a chained leg, and
// the first frame of two ranks or more.
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
  // A segment chained through a bend, the only kind a cut acts on.
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

// Disconnected states of seeded sizes in one frame, each its own component.
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

// pack_tests' late arrival as four disconnected states, scaled from a separation of 10: a
// fourth rect that compaction takes into the hole under the third.
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
  // Every variant reorders some chart.
  for (uint32_t k = 0; k < VARIANTS; ++k) {
    CAPTURE(k);
    CHECK(moved[k] > 0);
  }
}

TEST_CASE("size memo: a remembered frame is the layout those inputs size to") {
  scav_profile const p{ readable() };
  constexpr uint32_t VARIANTS{ 17 };
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

    // Orders that differ in one frame's ranks or direction: the frames around it hit, and
    // it must not.
    SearchPins turned;
    if (t.frame.v != INVALID) { turned.orients.push_back({ .frame = t.frame }); }
    SearchPins pinned;
    if (t.state.v != INVALID) { pinned.ranks.push_back({ .state = t.state, .rank = 2 }); }
    SubmachineOrders const o_turned{ order_submachines(c, g, s, p, 1, turned) };
    SubmachineOrders const o_pinned{ order_submachines(c, g, s, p, 1, pinned) };
    SubmachineOrders unlabelled{ o };  // the same gaps, charged to lanes alone
    std::ranges::fill(unlabelled.labels, 0);
    // One frame's fold pinned: the first folded frame unfolded, the first frame folded.
    SearchPins unfolded;
    for (uint32_t m = 0; (m < base.z.folded.size()) && unfolded.folds.empty(); ++m) {
      if (base.z.folded[m] != 0) {
        unfolded.folds.push_back({ .frame = SubmachineId{ m }, .mode = FOLD_NEVER });
      }
    }
    SearchPins folded;
    if (t.frame.v != INVALID) {
      folded.folds.push_back({ .frame = t.frame, .mode = FOLD_ALWAYS });
    }
    SearchPins recut;  // the first folded frame cut before its rank 1 alone
    for (uint32_t m = 0; (m < base.z.folded.size()) && recut.folds.empty(); ++m) {
      if (base.z.folded[m] != 0) {
        recut.folds.push_back(
            { .frame = SubmachineId{ m }, .mode = FOLD_ALWAYS, .layer = 1 });
      }
    }
    SubmachineOrders const o_recut{ order_submachines(c, g, s, p, 1, recut) };
    SubmachineOrders const o_unfolded{ order_submachines(c, g, s, p, 1, unfolded) };
    SubmachineOrders const o_folded{ order_submachines(c, g, s, p, 1, folded) };

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
        { .o = &unlabelled,
          .p = &p,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o_unfolded,
          .p = &p,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o_folded,
          .p = &p,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
        { .o = &o_recut,
          .p = &p,
          .req = &req,
          .dar = DarSource::Profile,
          .pack = Compaction::Off,
          .fold = Fold::Scale },
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

TEST_CASE("size memo: a port on a cross border keys on the port it continues through") {
  // `gauntlet/through` twice, the second with `reach` into `Top` in place of `Target`:
  // `Outer`'s frame differs only in where `Inner`'s port sits along its ranks.
  scav_profile const p{ readable() };
  std::vector<scav_byte> bytes;
  REQUIRE(read_file(SCAV_TEST_DATA_DIR "/charts/gauntlet/through.scav", bytes));
  std::string const text{ reinterpret_cast<char const *>(bytes.data()), bytes.size() };
  std::string moved_text{ text };
  std::string const into{ "Outer/Inner/Target" };
  size_t const at{ moved_text.find(into) };
  REQUIRE(at != std::string::npos);
  moved_text.replace(at, into.size(), "Outer/Inner/Top");

  std::array<Chart, 2> charts;
  std::array<std::string const *, 2> const texts{ &text, &moved_text };
  for (uint32_t k = 0; k < 2; ++k) {
    Loader loader;
    REQUIRE(load_add(loader,
                     reinterpret_cast<scav_byte const *>(texts[k]->data()),
                     texts[k]->size(),
                     "through.scav"));
    REQUIRE(load_pending(loader).empty());
    std::vector<Diagnostic> diags;
    REQUIRE(load_finish(loader, charts[k], diags));
  }
  auto const named = [](Chart const &c, std::string const &name) {
    for (uint32_t st = 0; st < c.states.size(); ++st) {
      if (chart_string(c, c.states[st].name) == name) { return st; }
    }
    return INVALID;
  };
  Chart const &c{ charts[0] };
  uint32_t const outer{ named(c, "Outer") };
  uint32_t const inner{ named(c, "Inner") };
  REQUIRE(outer != INVALID);
  REQUIRE(inner != INVALID);
  TransId const reach{ static_cast<uint32_t>(c.transitions.size()) - 1 };
  SubmachineId const outer_frame{ c.submachine_ids[c.states[outer].submachines.off] };
  SubmachineId const inner_frame{ c.submachine_ids[c.states[inner].submachines.off] };
  SearchPins const pins{ .orients = { { .frame = outer_frame }, { .frame = inner_frame } },
                         .sides = { { .trans = reach, .leg = 1, .end = 0, .side = 0 },
                                    { .trans = reach, .leg = 2, .end = 0, .side = 0 } } };

  std::array<SplitGraph, 2> const g{ decompose(charts[0]), decompose(charts[1]) };
  std::array<SubmachineOrders, 2> const o{
    order_submachines(charts[0], g[0], {}, p, 1, pins),
    order_submachines(charts[1], g[1], {}, p, 1, pins),
  };
  Sized const warm{ sized(charts[0],
                          g[0],
                          o[0],
                          {},
                          p,
                          DarSource::Profile,
                          Compaction::Off,
                          Fold::Scale,
                          false) };
  REQUIRE(warm.ok);
  Sized const got{ sized(charts[1],
                         g[1],
                         o[1],
                         {},
                         p,
                         DarSource::Profile,
                         Compaction::Off,
                         Fold::Scale,
                         false) };
  Sized const want{ sized(charts[1],
                          g[1],
                          o[1],
                          {},
                          p,
                          DarSource::Profile,
                          Compaction::Off,
                          Fold::Scale,
                          true) };
  REQUIRE(want.ok);
  CHECK(got.ok);
  CHECK(same(got.z, want.z));
  CHECK_FALSE(same(want.z, warm.z));
}

TEST_CASE("size memo: a port on a rank border keys on the port it continues through") {
  // Two charts differing only in whether the route reaches `Up` or `Down`, so only the
  // height of `Box`'s port, and `Outer`'s leading port level with it, differ.
  scav_profile const p{ readable() };
  std::string const text{
    "chart nest \"a route through two rank borders\" {\n"
    "  state Source,\n"
    "  state Outer {\n"
    "    state Box {\n"
    "      state First, state Up, state Down,\n"
    "      trans * -> First, trans First -> Up, trans First -> Down,\n"
    "    },\n"
    "    trans * -> Box,\n"
    "  },\n"
    "  trans * -> Source,\n"
    "  trans Source -> Outer/Box/Up,\n"
    "}\n"
  };
  std::string moved_text{ text };
  std::string const into{ "Outer/Box/Up" };
  size_t const at{ moved_text.find(into) };
  REQUIRE(at != std::string::npos);
  moved_text.replace(at, into.size(), "Outer/Box/Down");
  std::array<std::string, 2> const texts{ text, moved_text };
  std::array<Chart, 2> charts;
  for (uint32_t k = 0; k < 2; ++k) {
    Loader loader;
    REQUIRE(load_add(loader,
                     reinterpret_cast<scav_byte const *>(texts[k].data()),
                     texts[k].size(),
                     "nest.scav"));
    REQUIRE(load_pending(loader).empty());
    std::vector<Diagnostic> diags;
    REQUIRE(load_finish(loader, charts[k], diags));
  }
  auto const named = [&](std::string const &name) {
    for (uint32_t st = 0; st < charts[0].states.size(); ++st) {
      if (chart_string(charts[0], charts[0].states[st].name) == name) { return st; }
    }
    return INVALID;
  };
  uint32_t const box{ named("Box") };
  uint32_t const up{ named("Up") };
  REQUIRE(box != INVALID);
  REQUIRE(up != INVALID);
  Requests req;
  for (uint32_t st = 0; st < charts[0].states.size(); ++st) {
    req.box.push_back({ .min_w = 0, .h_before = (st == up) ? 400 : 0, .h_after = 0 });
  }
  scav_spaces const s{ req.view() };
  std::array<SplitGraph, 2> const g{ decompose(charts[0]), decompose(charts[1]) };
  std::array<SubmachineOrders, 2> const o{
    order_submachines(charts[0], g[0], s, p, 1, {}),
    order_submachines(charts[1], g[1], s, p, 1, {}),
  };
  auto const sized_as = [&](uint32_t k, bool traced) {
    return sized(charts[k],
                 g[k],
                 o[k],
                 s,
                 p,
                 DarSource::Profile,
                 Compaction::Off,
                 Fold::Scale,
                 traced);
  };
  Sized const warm{ sized_as(0, false) };
  REQUIRE(warm.ok);
  Sized const got{ sized_as(1, false) };
  Sized const want{ sized_as(1, true) };
  REQUIRE(want.ok);
  REQUIRE(want.z.state[box].w == warm.z.state[box].w);
  REQUIRE(want.z.state[box].h == warm.z.state[box].h);
  uint32_t const outer{ named("Outer") };
  REQUIRE(outer != INVALID);
  uint32_t const frame{
    charts[1].submachine_ids[charts[1].states[outer].submachines.off].v
  };
  Span const nodes{ o[1].sub_nodes[frame] };
  REQUIRE(o[0].sub_nodes[frame] == nodes);
  bool moved{ false };
  for (uint32_t k = nodes.off; k < (nodes.off + nodes.len); ++k) {
    moved = moved || (want.z.node[k].x != warm.z.node[k].x) ||
            (want.z.node[k].y != warm.z.node[k].y);
  }
  REQUIRE(moved);
  CHECK(got.ok);
  CHECK(same(got.z, want.z));
  CHECK_FALSE(same(want.z, warm.z));
}

namespace {

// Every field, the inputs recorded for a later call included.
bool same_all(SubmachineOrders const &a, SubmachineOrders const &b) {
  return same(a, b) && (a.edges == b.edges) && (a.sub_fold == b.sub_fold) &&
         (a.sub_fold_cut == b.sub_fold_cut) && (a.labels == b.labels) &&
         (a.seg_cross == b.seg_cross) && (a.seg_sided == b.seg_sided) &&
         (a.serial == b.serial) && (a.seg_pins == b.seg_pins) &&
         (a.seg_label == b.seg_label) && (a.state_pin == b.state_pin);
}

struct ReuseGuard {
  ReuseGuard() = default;
  ReuseGuard(ReuseGuard const &) = delete;
  ReuseGuard &operator=(ReuseGuard const &) = delete;
  ~ReuseGuard() {
    order_test_reuse(true);
    order_test_reuse_verify(false);
  }
};

}  // namespace

TEST_CASE("order reuse: a move in one frame leaves the others taken from the base") {
  ReuseGuard const guard;
  // Two composites side by side, each with a frame of its own: three frames, `B`'s
  // ordered before `A`'s, and in `B`'s a long edge chained through one bend.
  Chart c;
  SubmachineId const root{ build_chart(c, "two", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  SubmachineId const in_b{ build_submachine(c, b, {}, {}) };
  SubmachineId const in_a{ build_submachine(c, a, {}, {}) };
  StateId const a1{ build_state(c, in_a, "A1", StateKind::Normal, {}) };
  StateId const a2{ build_state(c, in_a, "A2", StateKind::Normal, {}) };
  StateId const b1{ build_state(c, in_b, "B1", StateKind::Normal, {}) };
  StateId const b2{ build_state(c, in_b, "B2", StateKind::Normal, {}) };
  StateId const b3{ build_state(c, in_b, "B3", StateKind::Normal, {}) };
  build_trans(c, a1, a2, TransKind::External, {});
  build_trans(c, b1, b2, TransKind::External, {});
  build_trans(c, b2, b3, TransKind::External, {});
  TransId const longest{ build_trans(c, b1, b3, TransKind::External, {}) };
  build_trans(c, a, b, TransKind::External, {});
  SplitGraph const g{ decompose(c) };
  scav_profile const p{ readable() };
  scav_spaces const s{};

  SubmachineOrders const base{ order_submachines(c, g, s, p, 1, {}) };
  SearchPins moved;
  moved.cuts.push_back({ .trans = longest, .leg = 0 });  // drops `B`'s bend
  SubmachineOrders const want{ ordered(c, g, s, p, moved, true) };
  REQUIRE(want.nodes.size() < base.nodes.size());

  order_test_reuse_verify(false);
  SubmachineOrders const got{ order_submachines(c, g, s, p, 1, moved, &base) };
  CHECK(order_test_reused() == 2);  // the root's and `A`'s, not `B`'s
  CHECK(same_all(got, want));
  CHECK(got.sub_nodes[in_a.v].off < base.sub_nodes[in_a.v].off);  // moved up, not copied
  for (uint32_t m : { root.v, in_a.v }) {
    Span const was{ base.sub_nodes[m] };
    Span const now{ got.sub_nodes[m] };
    REQUIRE(was.len == now.len);
    for (uint32_t k = 0; k < was.len; ++k) {
      CHECK(got.nodes[now.off + k] == base.nodes[was.off + k]);
    }
  }

  // The same pin is no move: every frame is taken.
  order_test_reuse_verify(false);
  SubmachineOrders const again{ order_submachines(c, g, s, p, 1, moved, &got) };
  CHECK(order_test_reused() == 3);
  CHECK(same_all(again, want));

  // A different profile takes nothing.
  scav_profile swept{ p };
  swept.sweep_count = 0;
  order_test_reuse_verify(false);
  (void)order_submachines(c, g, s, swept, 1, moved, &got);
  CHECK(order_test_reused() == 0);
}

TEST_CASE("order reuse: a frame taken from the base is the frame its inputs order to") {
  ReuseGuard const guard;
  scav_profile const p{ readable() };
  constexpr uint32_t VARIANTS{ 7 };
  std::vector<uint64_t> taken(VARIANTS, 0);
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
    std::vector<SearchPins> pins(6);
    if (t.state.v != INVALID) { pins[0].ranks.push_back({ .state = t.state, .rank = 2 }); }
    if (t.trans.v != INVALID) {
      pins[1].reverses.push_back({ .trans = t.trans, .leg = t.leg });
      pins[5].sides.push_back({ .trans = t.trans, .leg = t.leg, .end = 0, .side = 2 });
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
        { .pins = &pins[4], .p = &p, .req = &wider },
        { .pins = &pins[5], .p = &p, .req = &req } }
    };
    for (uint32_t k = 0; k < VARIANTS; ++k) {
      CAPTURE(k);
      Variant const &v{ variants[k] };
      scav_spaces const vs{ v.req->view() };
      order_test_reuse_verify(false);
      SubmachineOrders const got{ order_submachines(c, g, vs, *v.p, 1, *v.pins, &base) };
      taken[k] += order_test_reused();
      SubmachineOrders const want{ ordered(c, g, vs, *v.p, *v.pins, true) };
      CHECK(same_all(got, want));
    }
  }
  // Every pin leaves some frame to take; a profile changed takes none.
  for (uint32_t const k : { 0U, 1U, 2U, 3U, 6U }) {
    CAPTURE(k);
    CHECK(taken[k] > 0);
  }
  CHECK(taken[4] == 0);
}

namespace {

// `layout_run` on a corpus or gauntlet chart, with every frame a search takes from a base
// ordered again and compared; label boxes of one fabricated size where `labelled`.
void lay_out_checked(char const *name, bool labelled) {
  CAPTURE(name);
  CAPTURE(labelled);
  Chart c;
  load(name, c);
  std::vector<scav_path_box> boxes;
  for (uint32_t t = 0; labelled && (t < c.transitions.size()); ++t) {
    Transition const &tr{ c.transitions[t] };
    if ((tr.live == 0) || ((tr.src == tr.dst) && (tr.kind != TransKind::External))) {
      continue;
    }
    boxes.push_back({ .subject = t, .w = 40, .h = 12, .order = 0 });
  }
  scav_spaces const s{ .box_state_stride = sizeof(scav_box_space),
                       .path_box = boxes.data(),
                       .n_path_box = static_cast<uint32_t>(boxes.size()),
                       .path_box_stride = sizeof(scav_path_box) };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  scav_layout_opts const opts{ .profile = readable(), .router = 0, .threads = 0 };
  order_test_reuse_verify(true);
  REQUIRE(layout_run(c, s, opts, placed, diags));
  CHECK(order_test_reused() > 0);
  CHECK(order_test_reuse_mismatches() == 0);
}

}  // namespace

TEST_CASE("order reuse: every frame a search takes is the frame ordering it gives") {
  ReuseGuard const guard;
  constexpr std::array<char const *, 8> SMALL{ "axis.scav",
                                               "brew.scav",
                                               "dock.scav",
                                               "ota.scav",
                                               "vac.scav",
                                               "gauntlet/carried.scav",
                                               "gauntlet/through.scav",
                                               "gauntlet/level.scav" };
  for (char const *name : SMALL) {
    lay_out_checked(name, false);
    lay_out_checked(name, true);
  }
}

TEST_CASE("order reuse: every corpus and gauntlet chart at both scales" *
          doctest::skip()) {
  ReuseGuard const guard;
  constexpr std::array<char const *, 39> ALL{ "axis.scav",
                                              "bottler.scav",
                                              "brew.scav",
                                              "dock.scav",
                                              "estop.scav",
                                              "led.scav",
                                              "mill.scav",
                                              "ota.scav",
                                              "tcp.scav",
                                              "toolchanger.scav",
                                              "vac.scav",
                                              "gauntlet/above.scav",
                                              "gauntlet/carried.scav",
                                              "gauntlet/chain.scav",
                                              "gauntlet/corner.scav",
                                              "gauntlet/crossing.scav",
                                              "gauntlet/crowd.scav",
                                              "gauntlet/enclosing.scav",
                                              "gauntlet/entered.scav",
                                              "gauntlet/fanin.scav",
                                              "gauntlet/folded.scav",
                                              "gauntlet/fork.scav",
                                              "gauntlet/lane.scav",
                                              "gauntlet/level.scav",
                                              "gauntlet/long.scav",
                                              "gauntlet/loop.scav",
                                              "gauntlet/marks.scav",
                                              "gauntlet/mutual.scav",
                                              "gauntlet/ported.scav",
                                              "gauntlet/pulled.scav",
                                              "gauntlet/regions.scav",
                                              "gauntlet/roundtrip.scav",
                                              "gauntlet/seated.scav",
                                              "gauntlet/stretch.scav",
                                              "gauntlet/through.scav",
                                              "gauntlet/tight.scav",
                                              "gauntlet/transit.scav",
                                              "gauntlet/under.scav",
                                              "gauntlet/unfolded.scav" };
  for (char const *name : ALL) {
    lay_out_checked(name, false);
    lay_out_checked(name, true);
  }
}
