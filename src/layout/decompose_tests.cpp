// Decomposition against hand-written charts: split counts, port and frame
// sequences, the kind rules, and the concurrent direct arrow.

#include "layout/decompose.h"
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

  SplitGraph const g{ decompose(c) };
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

  SplitGraph const g{ decompose(c) };
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

  SplitGraph const g{ decompose(c) };
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

  SplitGraph const g{ decompose(c) };

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

  SplitGraph const g{ decompose(c) };
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

  SplitGraph const g{ decompose(c) };
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

  SplitGraph const g{ decompose(c) };
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

  SplitGraph const g{ decompose(c) };
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

  SplitGraph const g1{ decompose(c1) };
  SplitGraph const g2{ decompose(c2) };
  CHECK(g1.ports == g2.ports);
  CHECK(g1.segments == g2.segments);
  CHECK(g1.trans_segments == g2.trans_segments);
  CHECK(g1.state_crossings == g2.state_crossings);

  c1.transitions[1].live = 0;
  SplitGraph const g3{ decompose(c1) };
  CHECK(segs_of(g3, 0).len == 1);
  CHECK(segs_of(g3, 1).len == 0);
}

namespace {

// Ancestor-or-self, by climbing s's chain.
bool is_ancestor(Chart const &c, StateId anc, StateId s) {
  for (StateId x{ s }; x.v != INVALID;
       x = c.submachines[c.states[x.v].parent.v].owner) {
    if (x == anc) { return true; }
  }
  return false;
}

// The structural invariants every route owes, checked from the POD alone.
void check_route(Chart const &c, SplitGraph const &g, uint32_t t) {
  Span const span{ g.trans_segments[t] };
  Transition const &tr{ c.transitions[t] };
  std::vector<uint32_t> ports;
  for (uint32_t i = 0; i < g.ports.size(); ++i) {
    if (g.ports[i].trans.v == t) { ports.push_back(i); }
  }
  if (span.len == 0) {
    CHECK(ports.empty());
    CHECK(g.trans_crossings[t] == 0);
    return;
  }
  REQUIRE(span.len == ports.size() + 1);
  CHECK(g.trans_crossings[t] == ports.size());

  for (uint32_t k = 0; k < span.len; ++k) {
    SplitSegment const &sg{ g.segments[span.off + k] };
    CHECK(sg.trans.v == t);
    CHECK(sg.ordinal == k);
    CHECK(sg.src_port == ((k == 0) ? INVALID : ports[k - 1]));
    CHECK(sg.dst_port == (((k + 1) == span.len) ? INVALID : ports[k]));
    // Every crossing moves to a different frame.
    if (k > 0) { CHECK(sg.frame != g.segments[span.off + k - 1].frame); }
  }

  for (uint32_t k = 0; k < ports.size(); ++k) {
    SplitPort const &p{ g.ports[ports[k]] };
    CHECK(p.crossing == k);
    CHECK((p.state.v == INVALID) != (p.sub.v == INVALID));
    if (p.state.v != INVALID) {
      if (p.state == tr.src) {
        // The one source-border split: external, source enclosing the target.
        CHECK(tr.kind == TransKind::External);
        CHECK(is_ancestor(c, tr.src, tr.dst));
      } else if (is_ancestor(c, p.state, tr.src)) {
        CHECK(!is_ancestor(c, p.state, tr.dst));  // an exit separates them
      } else {
        CHECK(is_ancestor(c, p.state, tr.dst));  // an enter, never the dst itself
        CHECK(p.state != tr.dst);
      }
    } else {
      StateId const owner{ c.submachines[p.sub.v].owner };
      REQUIRE(owner.v != INVALID);
      CHECK(is_ancestor(c, owner, tr.src));  // separators sit inside a common state
      CHECK(is_ancestor(c, owner, tr.dst));
    }
  }
}

}  // namespace

TEST_CASE("split: every endpoint pair and kind holds the route invariants") {
  // Mixed depths, a concurrent state, and nesting inside one of its regions.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  build_state(c, root, "A", StateKind::Normal, {});
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  SubmachineId const b_m{ build_submachine(c, b, {}, {}) };
  build_state(c, b_m, "B1", StateKind::Normal, {});
  StateId const b2{ build_state(c, b_m, "B2", StateKind::Normal, {}) };
  SubmachineId const b2_m{ build_submachine(c, b2, {}, {}) };
  build_state(c, b2_m, "B21", StateKind::Normal, {});
  StateId const o{ build_state(c, root, "O", StateKind::Normal, {}) };
  SubmachineId const m1{ build_submachine(c, o, "m1", {}) };
  SubmachineId const m2{ build_submachine(c, o, "m2", {}) };
  StateId const p{ build_state(c, m1, "P", StateKind::Normal, {}) };
  SubmachineId const p_m{ build_submachine(c, p, {}, {}) };
  build_state(c, p_m, "P1", StateKind::Normal, {});
  build_state(c, m2, "Q", StateKind::Normal, {});

  uint32_t const n_states{ static_cast<uint32_t>(c.states.size()) };
  for (uint32_t src = 0; src < n_states; ++src) {
    for (uint32_t dst = 0; dst < n_states; ++dst) {
      for (TransKind const kind :
           { TransKind::External, TransKind::Internal, TransKind::Local }) {
        REQUIRE(build_trans(c, { src }, { dst }, kind, {}).v != INVALID);
      }
    }
  }

  SplitGraph const g{ decompose(c) };
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    CAPTURE(t);
    check_route(c, g, t);
  }

  // Ports group by transition, in transition order.
  for (uint32_t i = 1; i < g.ports.size(); ++i) {
    CHECK(g.ports[i - 1].trans.v <= g.ports[i].trans.v);
  }
  // The accumulated pull is exactly the fold of the state-border ports.
  std::vector<uint32_t> fold(c.states.size(), 0);
  for (SplitPort const &port : g.ports) {
    if (port.state.v != INVALID) { ++fold[port.state.v]; }
  }
  CHECK(fold == g.state_crossings);
}

TEST_CASE("split: intermediate borders split even when the source's does not") {
  // Composite to grandchild: internal and local skip the source border but
  // still cross the child composite between them.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(c, comp, {}, {}) };
  StateId const kid{ build_state(c, m, "K", StateKind::Normal, {}) };
  SubmachineId const km{ build_submachine(c, kid, {}, {}) };
  StateId const grand{ build_state(c, km, "G", StateKind::Normal, {}) };
  build_trans(c, comp, grand, TransKind::External, {});
  build_trans(c, comp, grand, TransKind::Internal, {});
  build_trans(c, comp, grand, TransKind::Local, {});

  SplitGraph const g{ decompose(c) };

  REQUIRE(segs_of(g, 0).len == 3);  // external: source border plus the child's
  CHECK(dst_port(g, 0, 0).state == comp);
  CHECK(dst_port(g, 0, 1).state == kid);
  CHECK(seg(g, 0, 0).frame == root);
  CHECK(seg(g, 0, 1).frame == m);
  CHECK(seg(g, 0, 2).frame == km);

  for (uint32_t t : { 1U, 2U }) {
    CAPTURE(t);
    REQUIRE(segs_of(g, t).len == 2);  // one fewer segment and port
    CHECK(dst_port(g, t, 0).state == kid);
    CHECK(seg(g, t, 0).frame == m);
    CHECK(seg(g, t, 1).frame == km);
  }
  CHECK(g.state_crossings[comp.v] == 1);  // the external row alone
  CHECK(g.state_crossings[kid.v] == 3);   // every kind exits substates
}

TEST_CASE("split: a shallow source enters a deep target outermost first") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const outer{ build_state(c, root, "C", StateKind::Normal, {}) };
  SubmachineId const m{ build_submachine(c, outer, {}, {}) };
  StateId const kid{ build_state(c, m, "K", StateKind::Normal, {}) };
  SubmachineId const km{ build_submachine(c, kid, {}, {}) };
  StateId const grand{ build_state(c, km, "G", StateKind::Normal, {}) };
  build_trans(c, a, grand, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(segs_of(g, 0).len == 3);
  CHECK(dst_port(g, 0, 0).state == outer);
  CHECK(dst_port(g, 0, 1).state == kid);
  CHECK(seg(g, 0, 0).frame == root);
  CHECK(seg(g, 0, 1).frame == m);
  CHECK(seg(g, 0, 2).frame == km);
}

TEST_CASE("split: a nested concurrent crossing exits, crosses, and enters") {
  // The concurrent owner sits inside another composite, so the separator
  // channel's frame is that composite's region, not the root.
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const wrap{ build_state(c, root, "W", StateKind::Normal, {}) };
  SubmachineId const wm{ build_submachine(c, wrap, {}, {}) };
  StateId const o{ build_state(c, wm, "O", StateKind::Normal, {}) };
  SubmachineId const m1{ build_submachine(c, o, "m1", {}) };
  SubmachineId const m2{ build_submachine(c, o, "m2", {}) };
  StateId const p{ build_state(c, m1, "P", StateKind::Normal, {}) };
  SubmachineId const pm{ build_submachine(c, p, {}, {}) };
  StateId const p1{ build_state(c, pm, "P1", StateKind::Normal, {}) };
  StateId const q{ build_state(c, m2, "Q", StateKind::Normal, {}) };
  SubmachineId const qm{ build_submachine(c, q, {}, {}) };
  StateId const q1{ build_state(c, qm, "Q1", StateKind::Normal, {}) };
  build_trans(c, p1, q1, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(segs_of(g, 0).len == 5);
  CHECK(dst_port(g, 0, 0).state == p);  // exit
  CHECK(dst_port(g, 0, 1).sub == m1);   // separator, both sides
  CHECK(dst_port(g, 0, 2).sub == m2);
  CHECK(dst_port(g, 0, 3).state == q);  // enter
  CHECK(seg(g, 0, 0).frame == pm);
  CHECK(seg(g, 0, 1).frame == m1);
  CHECK(seg(g, 0, 2).frame == wm);  // the channel, in the owner's region
  CHECK(seg(g, 0, 2).separator == 1);
  CHECK(seg(g, 0, 3).frame == m2);
  CHECK(seg(g, 0, 4).frame == qm);
  CHECK(g.state_crossings[o.v] == 0);
  CHECK(g.state_crossings[wrap.v] == 0);
  CHECK(g.trans_crossings[0] == 4);
}

TEST_CASE("split: siblings deep inside one composite meet in its region") {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const n{ build_state(c, root, "N", StateKind::Normal, {}) };
  SubmachineId const nm{ build_submachine(c, n, {}, {}) };
  StateId const x{ build_state(c, nm, "X", StateKind::Normal, {}) };
  SubmachineId const xm{ build_submachine(c, x, {}, {}) };
  StateId const x1{ build_state(c, xm, "x1", StateKind::Normal, {}) };
  StateId const y{ build_state(c, nm, "Y", StateKind::Normal, {}) };
  SubmachineId const ym{ build_submachine(c, y, {}, {}) };
  StateId const y1{ build_state(c, ym, "y1", StateKind::Normal, {}) };
  build_trans(c, x1, y1, TransKind::External, {});

  SplitGraph const g{ decompose(c) };
  REQUIRE(segs_of(g, 0).len == 3);
  CHECK(seg(g, 0, 1).frame == nm);  // the common frame is nested, not the root
  CHECK(seg(g, 0, 1).separator == 0);
  CHECK(g.state_crossings[n.v] == 0);
}

TEST_CASE("split: degenerate inputs are sized, empty, and skipped") {
  Chart empty;
  SplitGraph const g0{ decompose(empty) };
  CHECK(g0.ports.empty());
  CHECK(g0.segments.empty());
  CHECK(g0.trans_segments.empty());
  CHECK(g0.state_depth.empty());

  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const comp{ build_state(c, root, "C", StateKind::Normal, {}) };
  build_submachine(c, comp, {}, {});
  StateId const a{ build_state(c, root, "A", StateKind::Normal, {}) };
  StateId const b{ build_state(c, root, "B", StateKind::Normal, {}) };
  build_trans(c, comp, comp, TransKind::External, {});  // composite self-loop
  build_trans(c, a, b, TransKind::External, {});
  build_trans(c, a, b, TransKind::External, {});

  // A hand-poked unresolved endpoint and a tombstoned endpoint state.
  c.transitions[1].src = { INVALID };
  c.states[b.v].live = 0;

  SplitGraph const g{ decompose(c) };
  REQUIRE(segs_of(g, 0).len == 1);  // the loop routes outside, no ports
  CHECK(seg(g, 0, 0).frame == root);
  CHECK(g.ports.empty());
  CHECK(segs_of(g, 1).len == 0);
  CHECK(segs_of(g, 2).len == 0);
}

TEST_CASE("split: a loaded network splits across its include host") {
  Loader loader;
  std::vector<Diagnostic> diags;
  auto add = [&](std::string_view text, char const *name) {
    return load_add(loader,
                    reinterpret_cast<scav_byte const *>(text.data()),
                    text.size(),
                    name);
  };
  REQUIRE(add(R"(chart root {
    include "leaf.scav" as leaf,
    state A,
    trans * -> A,
    trans A -> leaf/L,
  })",
              "root.scav"));
  REQUIRE(load_pending(loader).size() == 1);
  REQUIRE(add(R"(chart leaf { state L, })", "leaf.scav"));

  Chart c;
  REQUIRE(load_finish(loader, c, diags));

  StateId a{ INVALID };
  StateId l{ INVALID };
  REQUIRE(resolve_path(c, c.root_submachine, "A", a) == ResolveStatus::Ok);
  REQUIRE(resolve_path(c, c.root_submachine, "leaf/L", l) == ResolveStatus::Ok);
  StateId const host{ c.submachines[c.states[l.v].parent.v].owner };
  REQUIRE(host.v != INVALID);

  SplitGraph const g{ decompose(c) };
  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    CAPTURE(t);
    check_route(c, g, t);
    if ((c.transitions[t].src == a) && (c.transitions[t].dst == l)) {
      REQUIRE(segs_of(g, t).len == 2);  // one crossing: the alias host's border
      CHECK(dst_port(g, t, 0).state == host);
    } else {
      CHECK(segs_of(g, t).len == 1);  // the initial pseudostate is a sibling
    }
  }
  CHECK(g.state_crossings[host.v] == 1);
  CHECK(g.state_depth[l.v] == 1);
}
