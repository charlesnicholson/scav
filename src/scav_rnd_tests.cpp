// The finalizer against values computed from the published algorithm rather
// than from this header:
//
//   M = (1 << 64) - 1
//   def splitmix64(z):
//       z ^= z >> 30; z = (z * 0xbf58476d1ce4e5b9) & M
//       z ^= z >> 27; z = (z * 0x94d049bb133111eb) & M
//       return z ^ (z >> 31)
//
// The golden-gamma row is the published first output of the seed-0 generator,
// which is the same finalizer applied to one gamma step.

#include "scav_rnd.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <set>
#include <vector>

namespace {

using namespace scav;

struct Vector {
  uint64_t input;
  uint64_t want;
};

struct Coord {
  uint64_t seed;
  uint32_t phase;
  uint32_t item;
  uint32_t step;
};

// The four fields as one comparable key, so a sweep can ask whether it asked
// for the same coordinate twice.
std::array<uint64_t, 4> key_of(Coord const &c) {
  return { c.seed,
           static_cast<uint64_t>(c.phase),
           static_cast<uint64_t>(c.item),
           static_cast<uint64_t>(c.step) };
}

std::vector<Coord> dense_sweep() {
  std::vector<Coord> out;
  out.reserve(size_t{ 5 } * (4096U + 4096U + 256U));
  for (uint64_t seed : { UINT64_C(0),
                         UINT64_C(1),
                         UINT64_C(12345),
                         UINT64_C(0x9E37'79B9'7F4A'7C15),
                         UINT64_C(0xFFFF'FFFF'FFFF'FFFF) }) {
    // Three families that cannot name the same coordinate: the first two differ
    // in phase, and the third differs from both in item and in step.
    for (uint32_t item = 0; item < 4096U; ++item) {
      out.push_back({ .seed = seed, .phase = 0U, .item = item, .step = 0U });
    }
    for (uint32_t step = 0; step < 4096U; ++step) {
      out.push_back({ .seed = seed, .phase = 1U, .item = 0U, .step = step });
    }
    for (uint32_t phase = 0; phase < 256U; ++phase) {
      out.push_back({ .seed = seed, .phase = phase, .item = 7U, .step = 9U });
    }
  }
  return out;
}

}  // namespace

TEST_CASE("rnd: splitmix64 matches the published finalizer") {
  constexpr std::array<Vector, 8> VECTORS{ {
      { .input = UINT64_C(0), .want = UINT64_C(0) },
      { .input = UINT64_C(1), .want = UINT64_C(0x5692'161D'100B'05E5) },
      { .input = UINT64_C(2), .want = UINT64_C(0xDBD2'3897'3A2B'148A) },
      { .input = UINT64_C(3), .want = UINT64_C(0x1E53'5EED'E314'28F0) },
      { .input = UINT64_C(0x9E37'79B9'7F4A'7C15),
        .want = UINT64_C(0xE220'A839'7B1D'CDAF) },
      { .input = UINT64_C(0x0123'4567'89AB'CDEF),
        .want = UINT64_C(0xB2C0'58E4'EBB5'112C) },
      { .input = UINT64_C(0x8000'0000'0000'0000),
        .want = UINT64_C(0x25C2'6EA5'79CE'A98A) },
      { .input = UINT64_C(0xFFFF'FFFF'FFFF'FFFF),
        .want = UINT64_C(0xB4D0'55FC'F2CB'BD7B) },
  } };
  for (Vector const &v : VECTORS) {
    CAPTURE(v.input);
    CHECK(splitmix64(v.input) == v.want);
  }
}

TEST_CASE("rnd: zero is the finalizer's fixed point") {
  CHECK(splitmix64(0) == 0U);
  static_assert(splitmix64(0) == 0U);
}

TEST_CASE("rnd: rnd matches the finalizer rounds it is built from") {
  constexpr std::array<Vector, 4> VECTORS{ {
      { .input = 0U, .want = UINT64_C(0) },
      { .input = 1U, .want = UINT64_C(0x71CA'C374'4804'9CE4) },
      { .input = 2U, .want = UINT64_C(0xF1C3'F4E8'BCCE'6D11) },
      { .input = UINT64_C(0xDEAD'BEEF'CAFE'F00D),
        .want = UINT64_C(0xFA03'7385'218F'C867) },
  } };
  for (Vector const &v : VECTORS) {
    CAPTURE(v.input);
    CHECK(rnd(v.input, 0U, 0U, 0U) == v.want);
  }
  CHECK(rnd(0U, 1U, 0U, 0U) == UINT64_C(0xEDFE'A30C'FBBF'01C3));
  CHECK(rnd(0U, 0U, 1U, 0U) == UINT64_C(0x7AB4'0E09'0F36'3A7D));
  CHECK(rnd(0U, 0U, 0U, 1U) == UINT64_C(0x5692'161D'100B'05E5));
  CHECK(rnd(UINT64_C(0xDEAD'BEEF'CAFE'F00D), 7U, 1234U, 9U) ==
        UINT64_C(0x55A4'D928'A689'7F9D));
  CHECK(rnd(UINT64_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX) ==
        UINT64_C(0xC115'5550'B7C5'FB74));
}

TEST_CASE("rnd: both are usable in a constant expression") {
  static_assert(splitmix64(1) == UINT64_C(0x5692'161D'100B'05E5));
  static_assert(rnd(1U, 0U, 0U, 0U) == UINT64_C(0x71CA'C374'4804'9CE4));
  constexpr uint64_t AT_ORIGIN{ rnd(0U, 0U, 0U, 0U) };
  static_assert(AT_ORIGIN == 0U);
  CHECK(AT_ORIGIN == 0U);
}

TEST_CASE("rnd: moving any one argument moves the output") {
  constexpr Coord BASE{ .seed = 0x0123'4567'89AB'CDEF, .phase = 3, .item = 77, .step = 5 };
  uint64_t const base{ rnd(BASE.seed, BASE.phase, BASE.item, BASE.step) };
  CHECK(rnd(BASE.seed + 1U, BASE.phase, BASE.item, BASE.step) != base);
  CHECK(rnd(BASE.seed - 1U, BASE.phase, BASE.item, BASE.step) != base);
  CHECK(rnd(BASE.seed, BASE.phase + 1U, BASE.item, BASE.step) != base);
  CHECK(rnd(BASE.seed, BASE.phase - 1U, BASE.item, BASE.step) != base);
  CHECK(rnd(BASE.seed, BASE.phase, BASE.item + 1U, BASE.step) != base);
  CHECK(rnd(BASE.seed, BASE.phase, BASE.item - 1U, BASE.step) != base);
  CHECK(rnd(BASE.seed, BASE.phase, BASE.item, BASE.step + 1U) != base);
  CHECK(rnd(BASE.seed, BASE.phase, BASE.item, BASE.step - 1U) != base);
  // The two 32-bit arguments are folded into one word, so a carry between them
  // is the collision a naive sum would produce.
  CHECK(rnd(0U, 1U, 0U, 0U) != rnd(0U, 0U, 1U, 0U));
  CHECK(rnd(0U, 0U, 1U, 0U) != rnd(0U, 0U, 0U, 1U));
}

TEST_CASE("rnd: a dense sweep of every axis collides nowhere") {
  std::vector<Coord> const coords{ dense_sweep() };
  std::set<std::array<uint64_t, 4>> asked;
  std::set<uint64_t> values;
  for (Coord const &c : coords) {
    asked.insert(key_of(c));
    values.insert(rnd(c.seed, c.phase, c.item, c.step));
  }
  CHECK(asked.size() == coords.size());
  CHECK(values.size() == coords.size());
}

TEST_CASE("rnd: the value at a coordinate does not depend on when it is asked") {
  std::vector<Coord> const coords{ dense_sweep() };
  std::vector<uint64_t> forward;
  forward.reserve(coords.size());
  for (Coord const &c : coords) {
    forward.push_back(rnd(c.seed, c.phase, c.item, c.step));
  }

  std::vector<uint64_t> backward;
  backward.reserve(coords.size());
  for (size_t i = coords.size(); i > 0; --i) {
    Coord const &c{ coords[i - 1] };
    backward.push_back(rnd(c.seed, c.phase, c.item, c.step));
  }

  bool same{ true };
  for (size_t i = 0; i < forward.size(); ++i) {
    same = same && (forward[i] == backward[backward.size() - 1 - i]);
  }
  CHECK(same);
}
