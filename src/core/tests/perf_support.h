#ifndef SCAV_CORE_TESTS_PERF_SUPPORT_H_INCLUDED
#define SCAV_CORE_TESTS_PERF_SUPPORT_H_INCLUDED

// Timing, shared by the three perf suites, which measured the same way in three
// byte-identical copies. Its own header rather than test_support.h because
// <chrono> drags <iomanip> in behind it on libstdc++ and the MSVC STL, and a
// `std::quoted` in scope wins ADL against a test's own `quoted` -- twenty-three
// files include test_support.h and none of the other twenty need a clock.

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace scav::test {

inline uint64_t micros_since(std::chrono::steady_clock::time_point start) {
  auto const elapsed{ std::chrono::steady_clock::now() - start };
  uint64_t const us{ static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) };
  return (us == 0) ? 1U : us;  // a zero denominator says nothing useful
}

inline uint64_t nanos_since(std::chrono::steady_clock::time_point start) {
  auto const elapsed{ std::chrono::steady_clock::now() - start };
  uint64_t const ns{ static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) };
  return (ns == 0) ? 1U : ns;
}

inline constexpr uint32_t SCALING_RUNS{ 5 };
inline constexpr uint32_t SCALING_ATTEMPTS{ 3 };

// A throughput floor's estimator. Noise only adds time, so the fastest run is
// the one least of the machine and most of the code.
template <typename Once>
uint64_t fastest_micros(Once &&once) {
  uint64_t best{ UINT64_MAX };
  for (uint32_t i = 0; i < SCALING_RUNS; ++i) {
    auto const start{ std::chrono::steady_clock::now() };
    once();
    uint64_t const us{ micros_since(start) };
    best = (us < best) ? us : best;
  }
  return best;
}

// A *ratio's* estimator, which no choice of sample makes safe at this duration.
// The scaling inputs run in hundreds of microseconds and a scheduler quantum is
// milliseconds, so one descheduled slice inflates a run by more than the slack a
// ratio allows -- a parse that is linear on an idle machine read 21x for 4x the
// bytes on a hosted runner. The median carries the steal it was measured with;
// min-of-N biases a ratio upward, because the shorter side finds an uncontended
// window more often than the longer one does.
//
// So the window is lengthened rather than the sample made clever: the work
// repeats inside one timed span until the span is tens of milliseconds. The
// ratio is untouched -- both sides are still one run's cost -- and a stolen
// millisecond goes from swamping the measurement to rounding it. Nanoseconds
// per run, so dividing by the repeat count keeps the resolution the division
// would otherwise spend.
inline constexpr uint64_t SCALING_WINDOW_NANOS{ 20'000'000 };

template <typename Once>
uint64_t nanos_per_run(Once &&once) {
  // The probe is also the warm-up, so a cold allocator is never a sample.
  auto const probe_start{ std::chrono::steady_clock::now() };
  once();
  // `nanos_since` never answers zero and `reps` is only ever raised from one, so
  // neither division below can divide by it. Written as a floor and a raise
  // rather than as a ternary because that is the form the analyser can follow:
  // it does not carry the guarantee out of `nanos_since` on its own.
  uint64_t const measured{ nanos_since(probe_start) };
  uint64_t const probe{ (measured == 0) ? 1ULL : measured };
  uint64_t reps{ 1 };
  if (probe < SCALING_WINDOW_NANOS) {
    reps = std::max(reps, SCALING_WINDOW_NANOS / probe);
  }
  auto const start{ std::chrono::steady_clock::now() };
  for (uint64_t i = 0; i < reps; ++i) { once(); }
  uint64_t const total{ nanos_since(start) };
  uint64_t const each{ total / reps };
  return (each == 0) ? 1ULL : each;
}

// Both sides of a ratio, measured together and retried.
//
// A scheduling hiccup inflates one attempt; a quadratic inflates every one. So
// the pair kept is the one with the lowest growth, which noise can only make
// look better and a real quadratic cannot fake.
struct Pair {
  uint64_t small, large;
};

template <typename Small, typename Large>
Pair best_pair(Small &&small, Large &&large) {
  Pair best{ .small = 1, .large = UINT64_MAX };
  for (uint32_t attempt = 0; attempt < SCALING_ATTEMPTS; ++attempt) {
    Pair const got{ .small = nanos_per_run(small), .large = nanos_per_run(large) };
    if ((got.large * best.small) < (best.large * got.small)) { best = got; }
  }
  return best;
}

}  // namespace scav::test

#endif  // SCAV_CORE_TESTS_PERF_SUPPORT_H_INCLUDED
