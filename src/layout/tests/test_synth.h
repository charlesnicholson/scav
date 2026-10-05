#ifndef SCAV_LAYOUT_TESTS_TEST_SYNTH_H_INCLUDED
#define SCAV_LAYOUT_TESTS_TEST_SYNTH_H_INCLUDED

// Charts more than one suite builds: the two scale targets, and a fork bar routed into a
// nested state, with its profile.

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

namespace scav {

// Depth 16, ~2k states, ~3.7k transitions including one long hierarchical
// edge per level.
Chart nested_2k_chart();

// 2048 states in one submachine: a chain with a back edge every 16 states.
Chart flat_2k_chart();

// A fork bar ranked before `outer`, with a transition into `deep`, nested in `outer`;
// under `sealed_profile` the bar is flush against `outer`.
Chart sealed_chart();

// Zero `rank_sep` and a router clearance of 192 (`node_sep` / 3), with Level 1 off.
scav_profile sealed_profile(scav_profile const &base);

}  // namespace scav

#endif  // SCAV_LAYOUT_TESTS_TEST_SYNTH_H_INCLUDED
