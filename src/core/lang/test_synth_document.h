#ifndef SCAV_CORE_LANG_TEST_SYNTH_DOCUMENT_H_INCLUDED
#define SCAV_CORE_LANG_TEST_SYNTH_DOCUMENT_H_INCLUDED

// Synthetic documents, in RAM -- a disk-backed benchmark measures the filesystem.
// Harness-only, and a trap alone: corpus_tests.cpp has the hand-written half.

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

// What the generator actually emitted, so a test at 100 MB can assert the parse
// found all of it rather than only that it did not crash.
struct SynthStats {
  uint32_t states;
  uint32_t submachines;
  uint32_t transitions;
  uint32_t attrs;
  uint32_t comments;
  uint32_t statements;  // the chart included
};

// Depth 16 and a handful of siblings: the scale target, small enough to read
// when a test fails.
SynthSpec synth_default_spec();

std::string synth_document(SynthSpec const &spec, SynthStats &stats);

// A document that is `depth` blocks deep and nothing else, for the depth cap.
// Deliberately not routed through SynthSpec: 10,000 levels of anything else
// would be gigabytes.
std::string synth_deep_document(uint32_t depth);

}  // namespace scav

#endif  // SCAV_CORE_LANG_TEST_SYNTH_DOCUMENT_H_INCLUDED
