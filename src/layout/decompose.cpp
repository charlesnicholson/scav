// Boundary discovery per transition: climb both endpoint chains to their
// divergence, then emit ports and segments in route order.

#include "layout/decompose.h"

#include "layout/memo.h"
#include "layout/order.h"
#include "scav/scav_core.h"
#include "scav_cold.h"
#include "scav_vector.h"

#include <cstdint>
#include <utility>

namespace scav {

namespace {

// `s` plus every enclosing state, innermost first; at most one entry per state.
SCAV_COLD void chain_of(Chart const &c, StateId s, Vector<StateId> &out) {
  out.clear();
  for (StateId x{ s }; (x.v != INVALID) && (out.size() < c.states.size());
       x = enclosing_state(c, x)) {
    out.push_back(x);
  }
}

// `tr`'s `CommonAncestor` from its ends' chains; `i` and `j` count each chain's divergent
// prefix.
CommonAncestor common_of(Chart const &c,
                         Transition const &tr,
                         Vector<StateId> const &chain_src,
                         Vector<StateId> const &chain_dst,
                         size_t i,
                         size_t j) {
  if (tr.src == tr.dst) {
    return { .state = tr.src,
             .frame = c.states[tr.src.v].parent,
             .child = { tr.src, tr.src } };
  }
  CommonAncestor out;
  out.state = (i < chain_src.size()) ? chain_src[i] : StateId{ INVALID };
  out.child = { (i > 0) ? chain_src[i - 1] : StateId{ INVALID },
                (j > 0) ? chain_dst[j - 1] : StateId{ INVALID } };
  SubmachineId const a{ (i > 0) ? c.states[chain_src[i - 1].v].parent
                                : SubmachineId{ INVALID } };
  SubmachineId const b{ (j > 0) ? c.states[chain_dst[j - 1].v].parent
                                : SubmachineId{ INVALID } };
  if (a.v == INVALID) {
    out.frame = b;
  } else if ((b.v == INVALID) || (a == b)) {
    out.frame = a;
  }
  return out;
}

// One planned boundary crossing, in route order.
struct Crossing {
  enum : uint32_t { Exit, Divider, Enter } kind;
  StateId state;      // Exit and Enter
  SubmachineId sub;   // Divider: the region left
  SubmachineId into;  // Divider: the region entered
};

}  // namespace

StateId enclosing_state(Chart const &c, StateId s) {
  if (s.v == INVALID) { return { INVALID }; }
  SubmachineId const parent{ c.states[s.v].parent };
  return (parent.v == INVALID) ? StateId{ INVALID } : c.submachines[parent.v].owner;
}

bool inner_loop(Chart const &c, uint32_t t) {
  Transition const &tr{ c.transitions[t] };
  return (tr.live != 0) && (tr.src == tr.dst) && (tr.src.v < c.states.size()) &&
         ((tr.kind == TransKind::Internal) || (tr.kind == TransKind::Local)) &&
         (c.states[tr.src.v].live != 0) && (c.states[tr.src.v].kind == StateKind::Normal);
}

bool ancestor_or_self(Chart const &c, StateId ancestor, StateId of) {
  StateId at{ of };
  for (size_t step = 0; (step < c.states.size()) && (at.v != INVALID); ++step) {
    if (at == ancestor) { return true; }
    at = enclosing_state(c, at);
  }
  return false;
}

SCAV_COLD SplitGraph decompose(Chart const &c) {
  SplitGraph g;
  Vector<StateId> chain_src;  // scratch, reused per state and transition
  Vector<StateId> chain_dst;
  Vector<Crossing> route;

  g.state_depth.assign(c.states.size(), 0);
  for (uint32_t s = 0; s < c.states.size(); ++s) {
    chain_of(c, { s }, chain_src);
    g.state_depth[s] = static_cast<uint32_t>(chain_src.size() - 1);
  }
  g.trans_segments.assign(c.transitions.size(), Span{});
  g.trans_common.assign(c.transitions.size(), CommonAncestor{});

  for (uint32_t t = 0; t < c.transitions.size(); ++t) {
    Transition const &tr{ c.transitions[t] };
    if ((tr.src.v >= c.states.size()) || (tr.dst.v >= c.states.size())) { continue; }
    chain_of(c, tr.src, chain_src);
    chain_of(c, tr.dst, chain_dst);
    size_t i{ chain_src.size() };
    size_t j{ chain_dst.size() };
    while ((i > 0) && (j > 0) && (chain_src[i - 1] == chain_dst[j - 1])) {
      --i;
      --j;
    }
    // `i` and `j` count each chain's divergent prefix, endpoint included.
    g.trans_common[t] = common_of(c, tr, chain_src, chain_dst, i, j);
    if ((tr.live == 0) || (c.states[tr.src.v].live == 0) ||
        (c.states[tr.dst.v].live == 0)) {
      continue;
    }
    route.clear();
    bool src_inner{ false };  // route starts on the source border's inner face
    bool dst_inner{ false };  // route ends on the target border's inner face
    if (tr.src != tr.dst) {
      bool const external{ tr.kind == TransKind::External };
      if (i == 0) {  // src encloses dst; its border splits unless internal or local
        src_inner = (tr.kind == TransKind::Internal) || (tr.kind == TransKind::Local);
        if (!src_inner) {
          route.push_back(
              { .kind = Crossing::Enter, .state = tr.src, .sub = {}, .into = {} });
        }
      }
      for (size_t k = 1; k < i; ++k) {
        route.push_back(
            { .kind = Crossing::Exit, .state = chain_src[k], .sub = {}, .into = {} });
      }
      if ((i > 0) && (j > 0) && (i < chain_src.size())) {
        // The chains meet at a state. An external route out of a machine exits and
        // re-enters it; any other crosses the divider between two of its submachines.
        SubmachineId const sub_src{ c.states[chain_src[i - 1].v].parent };
        SubmachineId const sub_dst{ c.states[chain_dst[j - 1].v].parent };
        if (external && ((i > 1) || (j > 1) || (sub_src != sub_dst))) {
          route.push_back(
              { .kind = Crossing::Exit, .state = chain_src[i], .sub = {}, .into = {} });
          route.push_back(
              { .kind = Crossing::Enter, .state = chain_src[i], .sub = {}, .into = {} });
        } else if (sub_src != sub_dst) {
          route.push_back(
              { .kind = Crossing::Divider, .state = {}, .sub = sub_src, .into = sub_dst });
        }
      }
      // `j == 0` when dst encloses src: an external route exits dst and ends on its
      // border; any other ends inside dst, on its inner face.
      if ((j == 0) && external) {
        route.push_back(
            { .kind = Crossing::Exit, .state = tr.dst, .sub = {}, .into = {} });
      }
      for (size_t k = j; k-- > 1;) {
        route.push_back(
            { .kind = Crossing::Enter, .state = chain_dst[k], .sub = {}, .into = {} });
      }
      dst_inner = (j == 0) && !external;
    }

    // The state entered after the Enter at `at`, or dst; its parent is the next frame.
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
      bool const divider{ x.kind == Crossing::Divider };
      uint32_t const port{ static_cast<uint32_t>(g.ports.size()) };
      g.ports.push_back({ .state = divider ? StateId{ INVALID } : x.state,
                          .sub = divider ? x.sub : SubmachineId{ INVALID },
                          .into = divider ? x.into : SubmachineId{ INVALID },
                          .trans = { t },
                          .crossing = static_cast<uint32_t>(k) });
      g.segments.push_back({ .trans = { t },
                             .ordinal = static_cast<uint32_t>(k),
                             .frame = frame,
                             .src_port = prev,
                             .dst_port = port,
                             .src_inner = ((k == 0) && src_inner) ? 1U : 0U,
                             .dst_inner = 0 });
      switch (x.kind) {
        case Crossing::Exit: frame = c.states[x.state.v].parent; break;
        case Crossing::Divider: frame = x.into; break;
        case Crossing::Enter: frame = c.states[entered_next(k).v].parent; break;
        default: break;
      }
      prev = port;
    }
    g.segments.push_back({ .trans = { t },
                           .ordinal = static_cast<uint32_t>(route.size()),
                           .frame = frame,
                           .src_port = prev,
                           .dst_port = INVALID,
                           .src_inner = (route.empty() && src_inner) ? 1U : 0U,
                           .dst_inner = dst_inner ? 1U : 0U });

    g.trans_segments[t] =
        make_span(first_segment, static_cast<uint32_t>(g.segments.size()) - first_segment);
  }
  // `g.trans_label` stays empty until every `label_segment` call returns.
  Vector<uint32_t> label;
  label.assign(c.transitions.size(), INVALID);
  for (uint32_t t = 0; t < label.size(); ++t) { label[t] = label_segment(c, g, t); }
  g.trans_label = std::move(label);
  g.serial = memo_serial();
  return g;
}

}  // namespace scav
