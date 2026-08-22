// Boundary discovery per transition: climb both endpoint chains to their
// divergence, then emit ports and segments in route order.

#include "layout/split.h"

#include "scav/scav_core.h"

#include <cstdint>
#include <vector>

namespace scav {

namespace {

// The state enclosing `s`; INVALID for a child of a document root.
StateId enclosing(Chart const &c, StateId s) {
  return c.submachines[c.states[s.v].parent.v].owner;
}

// `s` plus every enclosing state, innermost first.
void chain_of(Chart const &c, StateId s, std::vector<StateId> &out) {
  out.clear();
  for (StateId x{ s }; x.v != INVALID; x = enclosing(c, x)) { out.push_back(x); }
}

// One planned boundary crossing, in route order.
struct Crossing {
  enum : uint32_t { Exit, SepSrc, SepDst, Enter } kind;
  StateId state;     // Exit and Enter
  SubmachineId sub;  // SepSrc and SepDst
};

}  // namespace

SplitGraph phase0_split(Chart const &c) {
  SplitGraph g;
  g.state_depth.assign(c.states.size(), 0);
  for (uint32_t s = 0; s < c.states.size(); ++s) {
    uint32_t depth{ 0 };
    for (StateId x{ enclosing(c, { s }) }; x.v != INVALID; x = enclosing(c, x)) {
      ++depth;
    }
    g.state_depth[s] = depth;
  }
  g.state_crossings.assign(c.states.size(), 0);
  g.trans_segments.assign(c.transitions.size(), Span{});
  g.trans_crossings.assign(c.transitions.size(), 0);

  std::vector<StateId> chain_src;  // scratch, reused per transition
  std::vector<StateId> chain_dst;
  std::vector<Crossing> route;

  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    Transition const &tr{ c.transitions[t] };
    if ((tr.live == 0) || (tr.src.v == INVALID) || (tr.dst.v == INVALID) ||
        (c.states[tr.src.v].live == 0) || (c.states[tr.dst.v].live == 0)) {
      continue;
    }
    // An internal or local self-transition has no route: the app reserves a
    // band and draws it inside the state's own rect.
    if ((tr.src == tr.dst) && (tr.kind != TransKind::External)) { continue; }

    route.clear();
    bool src_inner{ false };  // route starts on the source border's inner face
    if (tr.src != tr.dst) {
      chain_of(c, tr.src, chain_src);
      chain_of(c, tr.dst, chain_dst);
      size_t i{ chain_src.size() };
      size_t j{ chain_dst.size() };
      while ((i > 0) && (j > 0) && (chain_src[i - 1] == chain_dst[j - 1])) {
        --i;
        --j;
      }
      // i and j now count the divergent prefix of each chain, endpoint
      // included. One shape covers every case: an empty run contributes nothing.
      if (i == 0) {  // src encloses dst; its border splits only when external
        src_inner = tr.kind != TransKind::External;
        if (!src_inner) { route.push_back({ Crossing::Enter, tr.src, {} }); }
      }
      for (size_t k = 1; k < i; ++k) {
        route.push_back({ Crossing::Exit, chain_src[k], {} });
      }
      if ((i > 0) && (j > 0) && (i < chain_src.size())) {
        // The chains meet at a state; entering through two of its submachines
        // crosses their separator, never that state's border.
        SubmachineId const sub_src{ c.states[chain_src[i - 1].v].parent };
        SubmachineId const sub_dst{ c.states[chain_dst[j - 1].v].parent };
        if (sub_src != sub_dst) {
          route.push_back({ Crossing::SepSrc, {}, sub_src });
          route.push_back({ Crossing::SepDst, {}, sub_dst });
        }
      }
      for (size_t k = j; k-- > 1;) {
        route.push_back({ Crossing::Enter, chain_dst[k], {} });
      }
    }

    // The state an Enter at `at` opens into next, which owns the next frame.
    auto entered_next = [&](size_t at) {
      return ((at + 1) < route.size()) ? route[at + 1].state : tr.dst;
    };

    StateId const first_inner{ route.empty() ? tr.dst : route.front().state };
    SubmachineId frame{ src_inner ? c.states[first_inner.v].parent
                                  : c.states[tr.src.v].parent };
    uint32_t const first_segment{ static_cast<uint32_t>(g.segments.size()) };
    uint32_t prev{ INVALID };
    for (size_t k = 0; k < route.size(); ++k) {
      Crossing const &x{ route[k] };
      uint32_t const port{ static_cast<uint32_t>(g.ports.size()) };
      g.ports.push_back({ .state = (x.kind == Crossing::Exit) || (x.kind == Crossing::Enter)
                                       ? x.state
                                       : StateId{ INVALID },
                          .sub = (x.kind == Crossing::SepSrc) || (x.kind == Crossing::SepDst)
                                     ? x.sub
                                     : SubmachineId{ INVALID },
                          .trans = { t },
                          .crossing = static_cast<uint32_t>(k) });
      g.segments.push_back({ .trans = { t },
                             .ordinal = static_cast<uint32_t>(k),
                             .frame = frame,
                             .src_port = prev,
                             .dst_port = port,
                             .separator = (x.kind == Crossing::SepDst) ? 1U : 0U });
      switch (x.kind) {
        case Crossing::Exit:
          frame = c.states[x.state.v].parent;
          ++g.state_crossings[x.state.v];
          break;
        case Crossing::SepSrc:
          frame = c.states[c.submachines[x.sub.v].owner.v].parent;
          break;
        case Crossing::SepDst: frame = x.sub; break;
        case Crossing::Enter:
          frame = c.states[entered_next(k).v].parent;
          ++g.state_crossings[x.state.v];
          break;
        default: break;
      }
      prev = port;
    }
    g.segments.push_back({ .trans = { t },
                           .ordinal = static_cast<uint32_t>(route.size()),
                           .frame = frame,
                           .src_port = prev,
                           .dst_port = INVALID,
                           .separator = 0 });

    g.trans_segments[t] =
        make_span(first_segment, static_cast<uint32_t>(g.segments.size()) - first_segment);
    g.trans_crossings[t] = static_cast<uint32_t>(route.size());
  }
  return g;
}

}  // namespace scav
