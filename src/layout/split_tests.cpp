// Phase 0 against hand-written charts: split counts, port and frame sequences,
// the kind rules, and the concurrent direct arrow.

#include "layout/split.h"
#include "scav/scav_core.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <vector>

namespace {

using namespace scav;

Span segs_of(SplitGraph const &g, uint32_t t) { return g.trans_segments[t]; }

SplitSegment const &seg(SplitGraph const &g, uint32_t t, uint32_t i) {
  return g.segments[segs_of(g, t).off + i];
}

// The port a segment ends on, so a test names boundaries rather than indices.
SplitPort const &dst_port(SplitGraph const &g, uint32_t t, uint32_t i) {
  return g.ports[seg(g, t, i).dst_port];
}

}  // namespace

TEST_CASE("split: a sibling transition is one segment in the shared frame") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ phase0_split(c) };
  CHECK(g.ports.empty());
  REQUIRE(segs_of(g, 0).len == 1);
  CHECK(seg(g, 0, 0) == SplitSegment{ .trans = { 0 },
                                      .ordinal = 0,
                                      .frame = root,
                                      .src_port = INVALID,
                                      .dst_port = INVALID,
                                      .separator = 0 });
  CHECK(g.trans_crossings[0] == 0);
  CHECK(g.state_depth[a.v] == 0);
}

TEST_CASE("split: exiting a composite splits once at its border") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const s{ build_state(c, inner, "S", StateKind::Normal, {}) };
  StateId const d{ build_state(c, root, "D", StateKind::Normal, {}) };
  build_trans(c, s, d, TransKind::External, {});

  SplitGraph const g{ phase0_split(c) };
  REQUIRE(segs_of(g, 0).len == 2);
  CHECK(seg(g, 0, 0).frame == inner);
  CHECK(seg(g, 0, 1).frame == root);
  CHECK(dst_port(g, 0, 0).state == comp);
  CHECK(dst_port(g, 0, 0).crossing == 0);
  CHECK(g.trans_crossings[0] == 1);
  CHECK(g.state_crossings[comp.v] == 1);
  CHECK(g.state_crossings[s.v] == 0);
  CHECK(g.state_depth[s.v] == 1);
}

TEST_CASE("split: exits run innermost-out, enters outermost-in, frames follow") {
  // src two levels deep in one branch, dst one level deep in a sibling branch.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const c1{ build_state(c, root, "C1", StateKind::Normal, {}) };
  SubmachineId const m1{ build_submachine(c, c1, {}, {}) };
  StateId const c2{ build_state(c, m1, "C2", StateKind::Normal, {}) };
  SubmachineId const m2{ build_submachine(c, c2, {}, {}) };
  StateId const s{ build_state(c, m2, "S", StateKind::Normal, {}) };
  StateId const e1{ build_state(c, root, "E1", StateKind::Normal, {}) };
  SubmachineId const n1{ build_submachine(c, e1, {}, {}) };
  StateId const d{ build_state(c, n1, "D", StateKind::Normal, {}) };
  build_trans(c, s, d, TransKind::External, {});

  SplitGraph const g{ phase0_split(c) };
  REQUIRE(segs_of(g, 0).len == 4);
  CHECK(dst_port(g, 0, 0).state == c2);  // innermost exit first
  CHECK(dst_port(g, 0, 1).state == c1);
  CHECK(dst_port(g, 0, 2).state == e1);  // then the enter
  CHECK(seg(g, 0, 0).frame == m2);
  CHECK(seg(g, 0, 1).frame == m1);
  CHECK(seg(g, 0, 2).frame == root);  // the middle segment, in the common frame
  CHECK(seg(g, 0, 3).frame == n1);
  CHECK(g.trans_crossings[0] == 3);
  for (uint32_t i = 0; i < 4; ++i) { CHECK(seg(g, 0, i).ordinal == i); }
}

TEST_CASE("split: kind decides whether the source border splits") {
  // A composite transitions to its own child, one row per kind.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, comp, {}, {}) };
  StateId const child{ build_state(c, inner, "K", StateKind::Normal, {}) };
  build_trans(c, comp, child, TransKind::External, {});
  build_trans(c, comp, child, TransKind::Internal, {});
  build_trans(c, comp, child, TransKind::Local, {});

  SplitGraph const g{ phase0_split(c) };

  // External exits and re-enters: a port on the source's own border.
  REQUIRE(segs_of(g, 0).len == 2);
  CHECK(dst_port(g, 0, 0).state == comp);
  CHECK(seg(g, 0, 0).frame == root);  // the stub outside the source box
  CHECK(seg(g, 0, 1).frame == inner);
  CHECK(g.trans_crossings[0] == 1);

  // Internal and local start on the inner face: one fewer segment and port.
  for (uint32_t t : { 1U, 2U }) {
    CAPTURE(t);
    REQUIRE(segs_of(g, t).len == 1);
    CHECK(seg(g, t, 0).frame == inner);
    CHECK(g.trans_crossings[t] == 0);
  }
  CHECK(g.state_crossings[comp.v] == 1);  // only the external row crossed
}

TEST_CASE("split: self-transitions route outside or not at all") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  build_trans(c, a, a, TransKind::External, {});
  build_trans(c, a, a, TransKind::Internal, {});
  build_trans(c, a, a, TransKind::Local, {});

  SplitGraph const g{ phase0_split(c) };
  REQUIRE(segs_of(g, 0).len == 1);  // the loop outside, in the parent frame
  CHECK(seg(g, 0, 0).frame == root);
  CHECK(segs_of(g, 1).len == 0);  // the app draws these inside the box
  CHECK(segs_of(g, 2).len == 0);
  CHECK(g.ports.empty());
}

TEST_CASE("split: concurrent siblings get a direct arrow through the separator") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const owner{ build_state(c, root, "O", StateKind::Normal, {}) };
  SubmachineId const m1{ build_submachine(c, owner, "m1", {}) };
  SubmachineId const m2{ build_submachine(c, owner, "m2", {}) };
  StateId const a{ build_state(c, m1, "a", StateKind::Normal, {}) };
  StateId const b{ build_state(c, m2, "b", StateKind::Normal, {}) };
  build_trans(c, a, b, TransKind::External, {});

  SplitGraph const g{ phase0_split(c) };
  REQUIRE(segs_of(g, 0).len == 3);
  CHECK(dst_port(g, 0, 0).sub == m1);
  CHECK(dst_port(g, 0, 1).sub == m2);
  CHECK(seg(g, 0, 0).frame == m1);
  CHECK(seg(g, 0, 1).frame == root);  // the separator channel, owned upward
  CHECK(seg(g, 0, 1).separator == 1);
  CHECK(seg(g, 0, 2).frame == m2);
  CHECK(g.state_crossings[owner.v] == 0);  // the owner's border is never crossed
  CHECK(g.trans_crossings[0] == 2);
}

TEST_CASE("split: a deep exit pulls on every ancestor it crosses") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const top{ build_state(c, root, "T", StateKind::Normal, {}) };

  constexpr uint32_t DEPTH{ 16 };
  std::vector<StateId> ancestors;
  SubmachineId parent{ root };
  for (uint32_t i = 0; i < DEPTH; ++i) {
    StateId const s{ build_state(c, parent, {}, StateKind::Normal, {}) };
    ancestors.push_back(s);
    parent = build_submachine(c, s, {}, {});
  }
  StateId const leaf{ build_state(c, parent, "leaf", StateKind::Normal, {}) };
  build_trans(c, leaf, top, TransKind::External, {});
  build_trans(c, leaf, top, TransKind::External, {});

  SplitGraph const g{ phase0_split(c) };
  CHECK(g.state_depth[leaf.v] == DEPTH);
  for (uint32_t t : { 0U, 1U }) {
    CAPTURE(t);
    CHECK(segs_of(g, t).len == DEPTH + 1);
    CHECK(g.trans_crossings[t] == DEPTH);
  }
  for (StateId const s : ancestors) { CHECK(g.state_crossings[s.v] == 2); }
  CHECK(g.ports.size() == 2 * DEPTH);
}

TEST_CASE("split: a transition into an enclosing composite stops on its inner face") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const outer{ build_state(c, root, "O", StateKind::Normal, {}) };
  SubmachineId const m_outer{ build_submachine(c, outer, {}, {}) };
  StateId const mid{ build_state(c, m_outer, "M", StateKind::Normal, {}) };
  SubmachineId const m_mid{ build_submachine(c, mid, {}, {}) };
  StateId const s{ build_state(c, m_mid, "S", StateKind::Normal, {}) };
  build_trans(c, s, outer, TransKind::External, {});

  SplitGraph const g{ phase0_split(c) };
  REQUIRE(segs_of(g, 0).len == 2);  // exits mid only; the endpoint is outer itself
  CHECK(dst_port(g, 0, 0).state == mid);
  CHECK(seg(g, 0, 0).frame == m_mid);
  CHECK(seg(g, 0, 1).frame == m_outer);
  CHECK(g.state_crossings[outer.v] == 0);
}

TEST_CASE("split: tombstones drop out and identical charts split identically") {
  auto build = [] {
    Chart c;
    SubmachineId const root{ build_chart(c, "t", {}) };
    StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
    StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
    build_trans(c, a, b, TransKind::External, {});
    build_trans(c, b, a, TransKind::External, {});
    return c;
  };
  Chart c1{ build() };
  Chart const c2{ build() };

  SplitGraph const g1{ phase0_split(c1) };
  SplitGraph const g2{ phase0_split(c2) };
  CHECK(g1.ports == g2.ports);
  CHECK(g1.segments == g2.segments);
  CHECK(g1.trans_segments == g2.trans_segments);
  CHECK(g1.state_crossings == g2.state_crossings);

  c1.transitions[1].live = 0;
  SplitGraph const g3{ phase0_split(c1) };
  CHECK(segs_of(g3, 0).len == 1);
  CHECK(segs_of(g3, 1).len == 0);
}
