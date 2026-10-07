// PodVector against std::vector over element sizes 1 to 100 bytes and every alignment up
// to max_align_t, including arguments that name the vector's own elements.

#include "scav_pod_vector.h"

#include "doctest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <ostream>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace scav;

static_assert(sizeof(PodVector<uint8_t>) == sizeof(void *) + 8);
static_assert(sizeof(PodVector<uint64_t>) == sizeof(void *) + 8);
static_assert(std::is_nothrow_move_constructible_v<PodVector<uint32_t>> &&
              std::is_nothrow_move_assignable_v<PodVector<uint32_t>>);

struct B3 {  // 3 bytes, alignment 1
  uint8_t a, b, c;
  bool operator==(B3 const &) const = default;
};

struct W12 {  // 12 bytes, alignment 4
  uint32_t a, b, c;
  bool operator==(W12 const &) const = default;
};

struct Init {  // value-initializes to its member initializers
  uint32_t a{ 7 };
  int32_t b{ -1 };
  uint64_t c{ 9 };
  bool operator==(Init const &) const = default;
};
static_assert(!std::is_trivially_default_constructible_v<Init>);

struct alignas(alignof(std::max_align_t)) Aligned {
  uint64_t lo, hi;
  bool operator==(Aligned const &) const = default;
};

struct Big {  // 100 bytes
  std::array<uint32_t, 25> w;
  bool operator==(Big const &) const = default;
};

// splitmix64's finalizer, position-addressed.
uint64_t rnd(uint64_t seed, uint64_t index) {
  uint64_t x{ seed + (index * 0x9E37'79B9'7F4A'7C15ULL) };
  x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31U);
}

// A T whose bytes come from `seed`.
template <typename T>
T make(uint64_t seed) {
  T x{};
  auto *const bytes{ reinterpret_cast<unsigned char *>(&x) };
  for (size_t i = 0; i < sizeof(T); ++i) {
    bytes[i] = static_cast<unsigned char>(rnd(seed, i));
  }
  return x;
}

template <typename T>
bool same(PodVector<T> const &got, std::vector<T> const &want) {
  if (got.size() != want.size()) { return false; }
  for (size_t i = 0; i < want.size(); ++i) {
    if (!(got[i] == want[i])) { return false; }
  }
  return true;
}

// size() == capacity(), so the next growth reallocates.
template <typename T>
PodVector<T> full(std::initializer_list<T> items) {
  PodVector<T> v;
  v.reserve(items.size());
  v.insert(v.end(), items);
  REQUIRE(v.size() == v.capacity());
  return v;
}

// A run of every member on a PodVector and a std::vector, compared after each step.
template <typename T>
void differential(uint64_t seed) {
  PodVector<T> got;
  std::vector<T> want;
  for (uint64_t step = 0; step < 3000; ++step) {
    uint64_t const r{ rnd(seed, step) };
    size_t const n{ (r >> 8U) % 48U };
    T const x{ make<T>(r) };
    size_t const at{ want.empty() ? 0 : ((r >> 16U) % (want.size() + 1)) };
    size_t const len{ want.empty() ? 0
                                   : ((r >> 24U) % (want.size() - (at % want.size()))) };
    size_t const from{ want.empty() ? 0 : (at % want.size()) };
    auto const pos{ static_cast<ptrdiff_t>(at) };
    switch (r % 20U) {
      case 0:
      case 1:
        got.push_back(x);
        want.push_back(x);
        break;
      case 2:
        got.emplace_back(x);
        want.emplace_back(x);
        break;
      case 3:
        if (!want.empty()) {
          got.pop_back();
          want.pop_back();
        }
        break;
      case 4:
        got.resize(n);
        want.resize(n);
        break;
      case 5:
        got.resize(n, x);
        want.resize(n, x);
        break;
      case 6:
        got.assign(n, x);
        want.assign(n, x);
        break;
      case 7: {
        std::vector<T> src;
        for (size_t i = 0; i < n; ++i) { src.push_back(make<T>(r + i)); }
        got.assign(src.data(), src.data() + src.size());
        want.assign(src.begin(), src.end());
        break;
      }
      case 8:
        got.insert(got.begin() + pos, x);
        want.insert(want.begin() + pos, x);
        break;
      case 9: {
        std::vector<T> src;
        for (size_t i = 0; i < (n % 7U); ++i) { src.push_back(make<T>(r ^ i)); }
        got.insert(got.begin() + pos, src.data(), src.data() + src.size());
        want.insert(want.begin() + pos, src.begin(), src.end());
        break;
      }
      case 10:
        got.insert(got.begin() + pos, { x, make<T>(r + 1) });
        want.insert(want.begin() + pos, { x, make<T>(r + 1) });
        break;
      case 11:
        if (!want.empty()) {
          auto const e{ static_cast<ptrdiff_t>(from) };
          got.erase(got.begin() + e);
          want.erase(want.begin() + e);
        }
        break;
      case 12: {
        auto const e{ static_cast<ptrdiff_t>(from) };
        auto const l{ static_cast<ptrdiff_t>(len) };
        got.erase(got.begin() + e, got.begin() + e + l);
        want.erase(want.begin() + e, want.begin() + e + l);
        break;
      }
      case 13:
        got.reserve(n * 3);
        want.reserve(n * 3);
        break;
      case 14:
        if ((r >> 40U) % 8U == 0) {
          got.clear();
          want.clear();
        }
        break;
      case 15: {  // an element of the vector itself
        if (!want.empty()) {
          got.push_back(got[from]);
          want.push_back(T{ want[from] });
        }
        break;
      }
      case 16: {  // a range of the vector itself, inserted into it
        auto const e{ static_cast<ptrdiff_t>(from) };
        auto const l{ static_cast<ptrdiff_t>(len) };
        std::vector<T> const copy(want.begin() + e, want.begin() + e + l);
        got.insert(got.begin() + pos, got.data() + e, got.data() + e + l);
        want.insert(want.begin() + pos, copy.begin(), copy.end());
        break;
      }
      case 17: {
        PodVector<T> c{ got };
        got = c;
        PodVector<T> m{ std::move(c) };
        got = std::move(m);
        break;
      }
      case 18:
        if (!want.empty()) {  // a value from the vector itself
          T const keep{ want[from] };
          got.resize(want.size() + n, got[from]);
          want.resize(want.size() + n, keep);
        }
        break;
      default:
        if (!want.empty()) {
          T const keep{ want[from] };
          got.assign(n, got[from]);
          want.assign(n, keep);
        }
        break;
    }
    REQUIRE_MESSAGE(same(got, want), "step " << step << " op " << (r % 20U));
    REQUIRE(got.capacity() >= got.size());
  }
}

}  // namespace

TEST_CASE("pod_vector: an empty vector owns no storage") {
  PodVector<uint32_t> v;
  CHECK(v.empty());
  CHECK(v.size() == 0);
  CHECK(v.capacity() == 0);
  CHECK(v.data() == nullptr);
  CHECK(v.begin() == v.end());

  PodVector<uint32_t> const copy{ v };
  CHECK(copy.capacity() == 0);
  CHECK(copy.data() == nullptr);
  PodVector<uint32_t> moved{ std::move(v) };
  CHECK(moved.capacity() == 0);

  v.reserve(0);
  v.resize(0);
  v.assign(0, 5);
  v.assign(copy.begin(), copy.end());
  v.insert(v.end(), copy.begin(), copy.end());
  v.erase(v.begin(), v.end());
  CHECK(v.data() == nullptr);
  CHECK(v.capacity() == 0);
  CHECK(v == copy);
}

TEST_CASE("pod_vector: push_back doubles capacity from one") {
  PodVector<uint32_t> v;
  std::vector<size_t> caps;
  for (uint32_t i = 0; i < 100; ++i) {
    v.push_back(i);
    if (caps.empty() || (caps.back() != v.capacity())) { caps.push_back(v.capacity()); }
  }
  CHECK(caps == std::vector<size_t>{ 1, 2, 4, 8, 16, 32, 64, 128 });
  for (uint32_t i = 0; i < 100; ++i) { CHECK(v[i] == i); }
}

TEST_CASE("pod_vector: reserve, assign, copies and constructors allocate exactly") {
  PodVector<uint32_t> v;
  v.reserve(10);
  CHECK(v.capacity() == 10);
  v.reserve(3);
  CHECK(v.capacity() == 10);
  v.assign(25, 1);
  CHECK(v.capacity() == 25);
  v.push_back(2);
  CHECK(v.capacity() == 50);

  PodVector<uint32_t> const copy{ v };
  CHECK(copy.capacity() == 26);
  PodVector<uint32_t> const sized(7);
  CHECK(sized.capacity() == 7);
  PodVector<uint32_t> const filled(5, 3);
  CHECK(filled.capacity() == 5);
  PodVector<uint32_t> const listed{ 1, 2, 3 };
  CHECK(listed.capacity() == 3);

  PodVector<uint32_t> small{ 1 };
  small = copy;
  CHECK(small.capacity() == 26);
  small.assign(listed.begin(), listed.end());
  CHECK(small.capacity() == 26);
}

TEST_CASE("pod_vector: growth past capacity takes the larger of double and the new size") {
  auto v{ full<uint32_t>({ 1, 2, 3, 4 }) };
  std::array<uint32_t, 10> const add{ 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
  v.insert(v.end(), add.data(), add.data() + 2);
  CHECK(v.capacity() == 8);
  v.insert(v.begin(), add.data(), add.data() + 10);
  CHECK(v.capacity() == 16);
  v.resize(40);
  CHECK(v.capacity() == 40);
  v.resize(41, 1);
  CHECK(v.capacity() == 80);
}

TEST_CASE("pod_vector: resize value-initializes and keeps the prefix") {
  PodVector<uint32_t> v{ 3, 1, 4 };
  v.resize(6);
  CHECK(v == PodVector<uint32_t>{ 3, 1, 4, 0, 0, 0 });
  v.resize(2);
  CHECK(v == PodVector<uint32_t>{ 3, 1 });
  v.resize(4, 8);
  CHECK(v == PodVector<uint32_t>{ 3, 1, 8, 8 });

  PodVector<Init> i(2);
  i.resize(5);
  for (Init const &e : i) { CHECK(e == Init{}); }
  PodVector<W12> w;
  w.resize(3);
  for (W12 const &e : w) { CHECK(e == W12{ 0, 0, 0 }); }
}

TEST_CASE(
    "vector: push_back, emplace_back and insert of an element of the same full vector") {
  auto v{ full<uint32_t>({ 7, 8, 9 }) };
  v.push_back(v[0]);
  CHECK(v == PodVector<uint32_t>{ 7, 8, 9, 7 });

  auto b{ full<Big>({ make<Big>(1), make<Big>(2) }) };
  b.push_back(b.back());
  CHECK(b[2] == make<Big>(2));

  auto e{ full<W12>({ { 1, 2, 3 } }) };
  W12 &made{ e.emplace_back(e[0]) };
  CHECK(&made == &e.back());
  CHECK(e == PodVector<W12>{ { 1, 2, 3 }, { 1, 2, 3 } });

  auto s{ full<uint32_t>({ 4, 5, 6 }) };
  uint32_t *const at{ s.insert(s.begin(), s[2]) };
  CHECK(at == s.begin());
  CHECK(s == PodVector<uint32_t>{ 6, 4, 5, 6 });
}

TEST_CASE("pod_vector: assign and resize from an element of the same vector") {
  auto v{ full<uint32_t>({ 4, 5, 6 }) };
  v.assign(32, v[1]);  // reallocating frees the element named
  CHECK(v == PodVector<uint32_t>(32, 5));
  v.assign(2, v[31]);  // in place
  CHECK(v == PodVector<uint32_t>{ 5, 5 });

  auto w{ full<uint32_t>({ 6, 5 }) };
  w.resize(20, w[1]);
  CHECK(w ==
        PodVector<uint32_t>{ 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5 });

  PodVector<uint32_t> r{ 1, 2, 3, 4, 5 };
  r.assign(r.begin() + 1, r.end() - 1);
  CHECK(r == PodVector<uint32_t>{ 2, 3, 4 });
}

TEST_CASE("pod_vector: insert a range of the same vector, with and without room") {
  auto v{ full<uint32_t>({ 1, 2, 3 }) };
  v.insert(v.begin() + 1, v.begin(), v.end());
  CHECK(v == PodVector<uint32_t>{ 1, 1, 2, 3, 2, 3 });

  PodVector<uint32_t> w{ 1, 2, 3 };
  w.reserve(10);
  uint32_t *const at{ w.insert(w.begin(), w.begin() + 1, w.end()) };
  CHECK(at == w.begin());
  CHECK(w == PodVector<uint32_t>{ 2, 3, 1, 2, 3 });
  CHECK(w.capacity() == 10);
}

TEST_CASE("pod_vector: insert and erase at the front, middle and end") {
  PodVector<uint32_t> v{ 1, 2, 3 };
  std::array<uint32_t, 2> const src{ 7, 8 };
  uint32_t *at{ v.insert(v.begin() + 1, src.data(), src.data() + 2) };
  CHECK(v == PodVector<uint32_t>{ 1, 7, 8, 2, 3 });
  CHECK(at == v.begin() + 1);
  at = v.insert(v.end(), { 5, 6 });
  CHECK(v == PodVector<uint32_t>{ 1, 7, 8, 2, 3, 5, 6 });
  CHECK(at == v.begin() + 5);
  at = v.insert(v.begin() + 2, src.data(), src.data());
  CHECK(at == v.begin() + 2);
  CHECK(v.size() == 7);

  at = v.erase(v.begin());
  CHECK(at == v.begin());
  CHECK(v == PodVector<uint32_t>{ 7, 8, 2, 3, 5, 6 });
  at = v.erase(v.begin() + 1, v.begin() + 4);
  CHECK(at == v.begin() + 1);
  CHECK(v == PodVector<uint32_t>{ 7, 5, 6 });
  at = v.erase(v.end() - 1);
  CHECK(at == v.end());
  CHECK(v == PodVector<uint32_t>{ 7, 5 });
}

TEST_CASE("pod_vector: copy, move, swap and self-assignment") {
  PodVector<uint32_t> a{ 1, 2, 3 };
  PodVector<uint32_t> b{ a };
  CHECK(b == a);
  CHECK(b.data() != a.data());
  b.push_back(4);
  CHECK(a == PodVector<uint32_t>{ 1, 2, 3 });

  PodVector<uint32_t> &alias{ a };
  a = alias;
  CHECK(a == PodVector<uint32_t>{ 1, 2, 3 });
  a = std::move(alias);
  CHECK(a == PodVector<uint32_t>{ 1, 2, 3 });

  uint32_t const *const storage{ b.data() };
  PodVector<uint32_t> c{ std::move(b) };
  CHECK(c.data() == storage);
  CHECK(b.empty());  // NOLINT(bugprone-use-after-move)
  CHECK(b.capacity() == 0);
  b = std::move(c);
  CHECK(b.data() == storage);
  CHECK(c.capacity() == 0);  // NOLINT(bugprone-use-after-move)

  a.swap(b);
  CHECK(a == PodVector<uint32_t>{ 1, 2, 3, 4 });
  CHECK(b == PodVector<uint32_t>{ 1, 2, 3 });
  a = b;  // fits, so a keeps its storage
  CHECK(a == b);
  CHECK(a.capacity() == 6);
}

TEST_CASE("pod_vector: front, back, pop_back and clear keep capacity") {
  PodVector<uint32_t> v{ 4, 5, 6 };
  CHECK(v.front() == 4);
  CHECK(v.back() == 6);
  v.pop_back();
  CHECK(v.back() == 5);
  size_t const cap{ v.capacity() };
  v.clear();
  CHECK(v.empty());
  CHECK(v.capacity() == cap);
}

TEST_CASE("pod_vector: a random run of every member matches std::vector") {
  differential<uint8_t>(1);
  differential<B3>(2);
  differential<uint16_t>(3);
  differential<uint32_t>(4);
  differential<W12>(5);
  differential<uint64_t>(6);
  differential<Init>(7);
  differential<Aligned>(8);
  differential<Big>(9);
}

TEST_CASE("pod_vector: storage holds max_align_t's alignment") {
  PodVector<Aligned> v;
  for (uint32_t i = 0; i < 40; ++i) {
    v.push_back(make<Aligned>(i));
    auto const addr{ reinterpret_cast<uintptr_t>(v.data()) };
    CHECK(addr % alignof(Aligned) == 0);
  }
}
