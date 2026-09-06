#ifndef SCAV_LAYOUT_TESTS_TEST_SYNTH_H_INCLUDED
#define SCAV_LAYOUT_TESTS_TEST_SYNTH_H_INCLUDED

// The charts more than one suite builds: the two at the scale target, and the
// sealed channel with the profile that seals it.

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

namespace scav {

// Depth 16, ~2k states, ~3.7k transitions including one long hierarchical
// edge per level.
Chart nested_2k_chart();

// The same count in one submachine, which is the largest single routing graph
// any chart produces (11.5).
Chart flat_2k_chart();

// The bar ranks before `outer` and stands taller than it, so under the profile
// below it spans `outer`'s frame and walls off the route into `deep`.
Chart sealed_chart();

// Clearance is a third of `node_sep`, so this asks for 192 grid units of it
// across a rank boundary nothing wide.
scav_profile sealed_profile(scav_profile const &base);

}  // namespace scav

#endif  // SCAV_LAYOUT_TESTS_TEST_SYNTH_H_INCLUDED
