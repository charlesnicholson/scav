// Boundary discovery per transition: climb both endpoint chains to their
// divergence, then emit ports and segments in route order.

#include "layout/decompose.h"

#include "scav/scav_core.h"

#include <cstdint>
#include <vector>

namespace scav {

namespace {

// `s` plus every enclosing state, innermost first. Capped at one entry per
// state, so a containment cycle stops instead of growing without end.
void chain_of(Chart const &c, StateId s, std::vector<StateId> &out) {
  out.clear();
  for (StateId x{ s }; (x.v != INVALID) && (out.size() < c.states.size());
       x = enclosing_state(c, x)) {
    out.push_back(x);
  }
}

// One planned boundary crossing, in route order.
struct Crossing {
  enum : uint32_t { Exit, SepSrc, SepDst, Enter } kind;
  StateId state;     // Exit and Enter
  SubmachineId sub;  // SepSrc and SepDst
};

}  // namespace

StateId enclosing_state(Chart const &c, StateId s) {
  if (s.v == INVALID) { return { INVALID }; }
  SubmachineId const parent{ c.states[s.v].parent };
  return (parent.v == INVALID) ? StateId{ INVALID } : c.submachines[parent.v].owner;
}

bool ancestor_or_self(Chart const &c, StateId ancestor, StateId of) {
  StateId at{ of };
  for (size_t step = 0; (step < c.states.size()) && (at.v != INVALID); ++step) {
    if (at == ancestor) { return true; }
    at = enclosing_state(c, at);
  }
  return false;
}

SplitGraph decompose(Chart const &c) {
  SplitGraph g;
  std::vector<StateId> chain_src;  // scratch, reused per state and transition
  std::vector<StateId> chain_dst;
  std::vector<Crossing> route;

  g.state_depth.assign(c.states.size(), 0);
  for (uint32_t s = 0; s < c.states.size(); ++s) {
    chain_of(c, { s }, chain_src);
    g.state_depth[s] = static_cast<uint32_t>(chain_src.size() - 1);
  }
  g.state_crossings.assign(c.states.size(), 0);
  g.trans_segments.assign(c.transitions.size(), Span{});

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
    bool dst_inner{ false };  // route ends on the target border's inner face
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
        if (!src_inner) {
          route.push_back({ .kind = Crossing::Enter, .state = tr.src, .sub = {} });
        }
      }
      for (size_t k = 1; k < i; ++k) {
        route.push_back({ .kind = Crossing::Exit, .state = chain_src[k], .sub = {} });
      }
      if ((i > 0) && (j > 0) && (i < chain_src.size())) {
        // The chains meet at a state; entering through two of its submachines
        // crosses their separator, never that state's border.
        SubmachineId const sub_src{ c.states[chain_src[i - 1].v].parent };
        SubmachineId const sub_dst{ c.states[chain_dst[j - 1].v].parent };
        if (sub_src != sub_dst) {
          route.push_back({ .kind = Crossing::SepSrc, .state = {}, .sub = sub_src });
          route.push_back({ .kind = Crossing::SepDst, .state = {}, .sub = sub_dst });
        }
      }
      for (size_t k = j; k-- > 1;) {
        route.push_back({ .kind = Crossing::Enter, .state = chain_dst[k], .sub = {} });
      }
      // The target chain ran out first, so dst is one of src's ancestors and
      // the last frame is a submachine of dst: the route arrives inside it
      // without crossing anything. i == 0 is the mirror and cannot coincide,
      // since both chains running out means src == dst.
      dst_inner = (j == 0);
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
      g.ports.push_back(
          { .state = (x.kind == Crossing::Exit) || (x.kind == Crossing::Enter)
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
                             .separator = (x.kind == Crossing::SepDst) ? 1U : 0U,
                             .src_inner = ((k == 0) && src_inner) ? 1U : 0U,
                             .dst_inner = 0 });
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
                           .separator = 0,
                           .src_inner = (route.empty() && src_inner) ? 1U : 0U,
                           .dst_inner = dst_inner ? 1U : 0U });

    g.trans_segments[t] =
        make_span(first_segment, static_cast<uint32_t>(g.segments.size()) - first_segment);
  }
  return g;
}

}  // namespace scav
