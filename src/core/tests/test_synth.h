#ifndef SCAV_CORE_TESTS_TEST_SYNTH_H_INCLUDED
#define SCAV_CORE_TESTS_TEST_SYNTH_H_INCLUDED

// Synthetic documents generated in RAM, for the test harness only.
// test_charts.h holds the hand-written charts.

#include <cstdint>
#include <string>

namespace scav {

struct SynthSpec {
  uint32_t depth;                  // composite-state nesting below the chart
  uint32_t states_per_block;       // leaf siblings inside each submachine
  uint32_t submachines_per_state;  // more than one makes the state concurrent
  uint32_t transitions_per_block;
  uint32_t attrs_per_state;
  uint32_t comment_every;  // emit a comment every n statements; 0 for none
  uint32_t min_roots;      // top-level subtrees, before the size target
  uint64_t min_bytes;      // keep adding subtrees until the text is this big
};

// Counts of what the generator emitted.
struct SynthStats {
  uint32_t states;
  uint32_t submachines;
  uint32_t transitions;
  uint32_t attrs;
  uint32_t comments;
  uint32_t statements;  // the chart included
};

// Depth 16, four leaves per submachine and two submachines per state: the scale target.
SynthSpec synth_default_spec();

std::string synth_document(SynthSpec const &spec, SynthStats &stats);

// A document of `depth` nested states around one leaf, for the parser's depth cap.
std::string synth_deep_document(uint32_t depth);

}  // namespace scav

#endif  // SCAV_CORE_TESTS_TEST_SYNTH_H_INCLUDED
