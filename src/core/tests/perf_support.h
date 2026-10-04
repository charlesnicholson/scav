#ifndef SCAV_CORE_TESTS_PERF_SUPPORT_H_INCLUDED
#define SCAV_CORE_TESTS_PERF_SUPPORT_H_INCLUDED

// Timing helpers shared by the perf suites. Kept out of test_support.h: <chrono>
// pulls in <iomanip>, whose `std::quoted` wins ADL over a test's `quoted`.

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace scav::test {

inline uint64_t micros_since(std::chrono::steady_clock::time_point start) {
  auto const elapsed{ std::chrono::steady_clock::now() - start };
  uint64_t const us{ static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) };
  return (us == 0) ? 1U : us;  // never zero; callers divide by it
}

inline uint64_t nanos_since(std::chrono::steady_clock::time_point start) {
  auto const elapsed{ std::chrono::steady_clock::now() - start };
  uint64_t const ns{ static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) };
  return (ns == 0) ? 1U : ns;
}

inline constexpr uint32_t SCALING_RUNS{ 5 };
inline constexpr uint32_t SCALING_ATTEMPTS{ 3 };

// Fastest of SCALING_RUNS runs, in microseconds: the throughput floors' estimator.
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

// Target length of one timed batch, in nanoseconds; `nanos_per_run` sizes its
// repeat count from a probe run to fill it, and returns nanoseconds per run.
inline constexpr uint64_t SCALING_WINDOW_NANOS{ 20'000'000 };

template <typename Once>
uint64_t nanos_per_run(Once &&once) {
  // The probe run sizes `reps` and warms up; only the repeated runs are timed.
  auto const probe_start{ std::chrono::steady_clock::now() };
  once();
  // `probe` and `reps` are at least one, so neither division below is by zero.
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

// Both sides of a ratio, measured together. `best_pair` keeps the attempt with
// the lowest large-to-small growth.
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
