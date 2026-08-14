// The public text-path resolver: §9's spellings against a code-built chart,
// so resolution is tested without the parser in the loop.

#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

// The corpus vac chart's shape: Off and On at the root, On:main{Idle, Ready},
// On:aux{Idle}, plus an unresolved include alias `dock`.
struct Rig {
  Chart c;
  SubmachineId root{ INVALID }, main_sm{ INVALID }, aux{ INVALID };
  StateId off{ INVALID }, on{ INVALID }, idle{ INVALID }, ready{ INVALID };
  StateId aux_idle{ INVALID }, dock{ INVALID };
};

Rig rig() {
  Rig r;
  r.root = build_chart(r.c, "vac", {});
  r.off = build_state(r.c, r.root, "Off", StateKind::Normal, {});
  r.on = build_state(r.c, r.root, "On", StateKind::Normal, {});
  r.main_sm = build_submachine(r.c, r.on, "main", {});
  r.aux = build_submachine(r.c, r.on, "aux", {});
  r.idle = build_state(r.c, r.main_sm, "Idle", StateKind::Normal, {});
  r.ready = build_state(r.c, r.main_sm, "Ready", StateKind::Normal, {});
  r.aux_idle = build_state(r.c, r.aux, "Idle", StateKind::Normal, {});
  r.dock = r.c.includes[build_include(r.c, r.root, "dock").v].host;
  return r;
}

ResolveStatus at(Rig const &r, SubmachineId scope, std::string_view p, StateId &out) {
  return resolve_path(r.c, scope, p, out);
}

}  // namespace

TEST_CASE("resolve: absolute paths from the root") {
  Rig const r{ rig() };
  StateId out{ INVALID };
  CHECK(at(r, r.root, "Off", out) == ResolveStatus::Ok);
  CHECK(out == r.off);
  CHECK(at(r, r.root, "On:main/Idle", out) == ResolveStatus::Ok);
  CHECK(out == r.idle);
  CHECK(at(r, r.root, "On:aux/Idle", out) == ResolveStatus::Ok);
  CHECK(out == r.aux_idle);
  // Ordinal spelling reaches the same rows.
  CHECK(at(r, r.root, "On:0/Ready", out) == ResolveStatus::Ok);
  CHECK(out == r.ready);
  CHECK(at(r, r.root, "On:1/Idle", out) == ResolveStatus::Ok);
  CHECK(out == r.aux_idle);
}

TEST_CASE("resolve: the first segment climbs, later segments only descend") {
  Rig const r{ rig() };
  StateId out{ INVALID };
  // From inside main: the local Idle shadows nothing -- it is simply nearest.
  CHECK(at(r, r.main_sm, "Idle", out) == ResolveStatus::Ok);
  CHECK(out == r.idle);
  // Off is not in main; the climb finds it at the root.
  CHECK(at(r, r.main_sm, "Off", out) == ResolveStatus::Ok);
  CHECK(out == r.off);
  // aux' Idle is a sibling submachine's child: never on the climb path.
  CHECK(at(r, r.aux, "Ready", out) == ResolveStatus::NotFound);
  // Descent is strict: On/Off would need Off inside On.
  CHECK(at(r, r.root, "On:main/Off", out) == ResolveStatus::NotFound);
}

TEST_CASE("resolve: qualifiers are required exactly when ambiguous") {
  Rig const r{ rig() };
  StateId out{ INVALID };
  // On has two submachines: unqualified descent is ambiguous.
  CHECK(at(r, r.root, "On/Idle", out) == ResolveStatus::BadQualifier);
  CHECK(at(r, r.root, "On:nope/Idle", out) == ResolveStatus::BadQualifier);
  CHECK(at(r, r.root, "On:7/Idle", out) == ResolveStatus::BadQualifier);
  // A qualifier on the last segment selects nothing.
  CHECK(at(r, r.root, "Off:0", out) == ResolveStatus::BadQualifier);
  // A single-submachine state needs none.
  Rig r2;
  r2.root = build_chart(r2.c, "c", {});
  StateId const solo{ build_state(r2.c, r2.root, "S", StateKind::Normal, {}) };
  SubmachineId const only{ build_submachine(r2.c, solo, {}, {}) };
  StateId const leaf{ build_state(r2.c, only, "L", StateKind::Normal, {}) };
  CHECK(resolve_path(r2.c, r2.root, "S/L", out) == ResolveStatus::Ok);
  CHECK(out == leaf);
}

TEST_CASE("resolve: descending into an unresolved include crosses it") {
  Rig const r{ rig() };
  StateId out{ INVALID };
  CHECK(at(r, r.root, "dock", out) == ResolveStatus::Ok);  // the host is a state
  CHECK(out == r.dock);
  CHECK(at(r, r.root, "dock/Up", out) == ResolveStatus::CrossesInclude);
  CHECK(at(r, r.root, "dock:main/Up", out) == ResolveStatus::CrossesInclude);
}

TEST_CASE("resolve: an attached include is an ordinary descent") {
  Rig r{ rig() };
  // What P2 will do: fill target and give the host the included root.
  r.c.includes[0].target = DocId{ 0 };
  SubmachineId const dock_root{ build_submachine(r.c, r.dock, {}, {}) };
  StateId const up{ build_state(r.c, dock_root, "Up", StateKind::Normal, {}) };
  StateId out{ INVALID };
  CHECK(at(r, r.root, "dock/Up", out) == ResolveStatus::Ok);
  CHECK(out == up);
}

TEST_CASE("resolve: synthetic $kind spellings address unnamed pseudostates") {
  Rig r{ rig() };
  StateId const i0{ build_state(r.c, r.main_sm, {}, StateKind::Initial, {}) };
  StateId const c0{ build_state(r.c, r.main_sm, {}, StateKind::Choice, {}) };
  StateId const c1{ build_state(r.c, r.main_sm, {}, StateKind::Choice, {}) };
  StateId out{ INVALID };
  CHECK(at(r, r.root, "On:main/$initial", out) == ResolveStatus::Ok);
  CHECK(out == i0);
  CHECK(at(r, r.root, "On:main/$choice", out) == ResolveStatus::Ok);
  CHECK(out == c0);
  CHECK(at(r, r.root, "On:main/$choice1", out) == ResolveStatus::Ok);
  CHECK(out == c1);
  // Round trip: the resolver parses what chart_path_of printed.
  CHECK(at(r, r.root, path(r.c, c1), out) == ResolveStatus::Ok);
  CHECK(out == c1);
}

TEST_CASE("resolve: liveness -- a tombstoned state has no address") {
  Rig r{ rig() };
  r.c.states[r.off.v].live = 0;
  StateId out{ INVALID };
  CHECK(at(r, r.root, "Off", out) == ResolveStatus::NotFound);
}

TEST_CASE("resolve: malformed text misses rather than crashing") {
  Rig const r{ rig() };
  StateId out{ INVALID };
  CHECK(at(r, r.root, "", out) == ResolveStatus::NotFound);
  CHECK(at(r, r.root, "/", out) == ResolveStatus::NotFound);
  CHECK(at(r, r.root, "On/", out) == ResolveStatus::NotFound);
  CHECK(at(r, r.root, ":main", out) == ResolveStatus::NotFound);
  CHECK(at(r, r.root, "On:", out) == ResolveStatus::NotFound);
  CHECK(at(r, r.root, "On:99999999999/Idle", out) == ResolveStatus::BadQualifier);
  CHECK(at(r, SubmachineId{ INVALID }, "Off", out) == ResolveStatus::NotFound);
}
