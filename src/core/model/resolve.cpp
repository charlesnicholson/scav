// The first segment is lexically scoped -- innermost submachine outward,
// nearest match winning -- and every later segment descends strictly.

#include "core/model/model.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

namespace {

// The live child of `m` named `name`, or INVALID. A `$` spelling matches each
// unnamed candidate's segment through the helper chart_path_of prints with.
StateId find_in_submachine(Chart const &c, SubmachineId m, std::string_view name) {
  Span const kids{ c.submachines[m.v].children };
  bool const synthetic{ !name.empty() && (name[0] == '$') };
  std::string spelled;
  for (uint32_t i = 0; i < kids.len; ++i) {
    StateId const id{ c.state_ids[kids.off + i] };
    if (id.v >= c.states.size()) { continue; }  // validation's finding, not ours
    State const &s{ c.states[id.v] };
    if (s.live == 0) { continue; }
    if (synthetic) {
      if (s.name.len != 0) { continue; }
      spelled.clear();
      model_state_segment(c, id, spelled);
      if (spelled == name) { return id; }
    } else if (s.name.len != 0) {
      if (string_pool_view(c.strings, s.name) == name) { return id; }
    }
  }
  return { INVALID };
}

// A state an include synthesized whose target is not attached; descending into
// one gives CrossesInclude, since its interior is in another file.
bool is_unresolved_alias(Chart const &c, StateId id) {
  for (Include const &inc : c.includes) {
    if ((inc.host == id) && (inc.target.v == INVALID)) { return true; }
  }
  return false;
}

// Picks which of `state`'s submachines the next segment descends into. A
// qualifier is required exactly when more than one is live.
ResolveStatus select_submachine(Chart const &c,
                                StateId state,
                                ResolveSeg const &seg,
                                SubmachineId &out) {
  State const &s{ c.states[state.v] };
  SubmachineId sole{ INVALID };
  uint32_t live_count{ 0 };
  for (uint32_t i = 0; i < s.submachines.len; ++i) {
    SubmachineId const m{ c.submachine_ids[s.submachines.off + i] };
    if ((m.v >= c.submachines.size()) || (c.submachines[m.v].live == 0)) { continue; }
    Submachine const &row{ c.submachines[m.v] };
    if (!seg.qualifier.empty()) {
      if (string_pool_view(c.strings, row.name) == seg.qualifier) {
        out = m;
        return ResolveStatus::Ok;
      }
    } else if (seg.ordinal != INVALID) {
      if (row.ordinal == seg.ordinal) {
        out = m;
        return ResolveStatus::Ok;
      }
    }
    sole = m;
    ++live_count;
  }
  if (live_count == 0) {
    return is_unresolved_alias(c, state) ? ResolveStatus::CrossesInclude
                                         : ResolveStatus::NotFound;
  }
  if (!seg.qualifier.empty() || (seg.ordinal != INVALID)) {
    return ResolveStatus::BadQualifier;  // qualified, and nothing matched
  }
  if (live_count > 1) {
    return ResolveStatus::BadQualifier;  // ambiguous, so a qualifier is required
  }
  out = sole;
  return ResolveStatus::Ok;
}

}  // namespace

ResolveStatus model_resolve_segments(Chart const &c,
                                     SubmachineId scope,
                                     ResolveSeg const *segs,
                                     uint32_t count,
                                     StateId &out) {
  if ((count == 0) || (scope.v >= c.submachines.size())) {
    return ResolveStatus::NotFound;
  }

  // Innermost-outward, stopping at the alias-host edge, where a submachine and
  // its owner carry different InstIds. The guard bounds a corrupted chain.
  StateId const found{ [&] {
    SubmachineId sm{ scope };
    for (size_t guard = 0; guard <= c.submachines.size(); ++guard) {
      StateId const hit{ find_in_submachine(c, sm, segs[0].name) };
      if (hit.v != INVALID) { return hit; }
      StateId const owner{ c.submachines[sm.v].owner };
      if ((owner.v == INVALID) || (owner.v >= c.states.size())) { break; }
      if (c.submachines[sm.v].inst != c.states[owner.v].inst) { break; }
      sm = c.states[owner.v].parent;
      if (sm.v >= c.submachines.size()) { break; }
    }
    return StateId{ INVALID };
  }() };
  if (found.v == INVALID) { return ResolveStatus::NotFound; }

  StateId cur{ found };
  for (uint32_t i = 0; i + 1 < count; ++i) {
    SubmachineId next{ INVALID };
    ResolveStatus const status{ select_submachine(c, cur, segs[i], next) };
    if (status != ResolveStatus::Ok) { return status; }
    cur = find_in_submachine(c, next, segs[i + 1].name);
    if (cur.v == INVALID) { return ResolveStatus::NotFound; }
  }

  // The last segment names a state, so there is nothing left for a qualifier
  // to select.
  ResolveSeg const &last{ segs[count - 1] };
  if (!last.qualifier.empty() || (last.ordinal != INVALID)) {
    return ResolveStatus::BadQualifier;
  }
  out = cur;
  return ResolveStatus::Ok;
}

ResolveStatus resolve_path(Chart const &c,
                           SubmachineId scope,
                           std::string_view path,
                           StateId &out) {
  // Split on '/', then each segment on one ':'. An all-digit qualifier is an
  // ordinal, anything else a submachine name; `$initial` is accepted here.
  std::vector<ResolveSeg> segs;
  size_t start{ 0 };
  while (start <= path.size()) {
    size_t const slash{ path.find('/', start) };
    size_t const end{ (slash == std::string_view::npos) ? path.size() : slash };
    std::string_view const seg_text{ path.substr(start, end - start) };
    if (seg_text.empty()) { return ResolveStatus::NotFound; }

    ResolveSeg seg{ .name = seg_text, .qualifier = {}, .ordinal = INVALID };
    if (size_t const colon{ seg_text.find(':') }; colon != std::string_view::npos) {
      seg.name = seg_text.substr(0, colon);
      std::string_view const qual{ seg_text.substr(colon + 1) };
      if (seg.name.empty() || qual.empty()) { return ResolveStatus::NotFound; }
      bool all_digits{ true };
      uint64_t value{ 0 };
      for (char const ch : qual) {
        if ((ch < '0') || (ch > '9')) {
          all_digits = false;
          break;
        }
        value = (value * 10U) + static_cast<uint64_t>(ch - '0');
        if (value > INVALID) { return ResolveStatus::BadQualifier; }
      }
      if (all_digits) {
        seg.ordinal = static_cast<uint32_t>(value);
      } else {
        seg.qualifier = qual;
      }
    }
    segs.push_back(seg);
    if (slash == std::string_view::npos) { break; }
    start = slash + 1;
  }

  return model_resolve_segments(c,
                                scope,
                                segs.data(),
                                narrow_clamp<uint32_t>(segs.size()),
                                out);
}

}  // namespace scav
