// Each helper against the std::vector member it stands for, on a trivially
// copyable element and on one that owns memory, at and below capacity.

#include "scav_vec.h"

#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <utility>
#include <vector>

namespace {

using namespace scav;

using Owned = std::vector<uint32_t>;

struct Pair {
  uint32_t a;
  uint32_t b;
  bool operator==(Pair const &) const = default;
};

// size() == capacity(), so the next growth reallocates.
template <typename T>
std::vector<T> full(std::initializer_list<T> items) {
  std::vector<T> v;
  v.reserve(items.size());
  v.insert(v.end(), items.begin(), items.end());
  REQUIRE(v.size() == v.capacity());
  return v;
}

// splitmix64's finalizer, position-addressed.
uint64_t rnd(uint64_t seed, uint64_t index) {
  uint64_t x{ seed + (index * 0x9E37'79B9'7F4A'7C15ULL) };
  x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31U);
}

}  // namespace

TEST_CASE("vec: push_back below and at capacity") {
  std::vector<uint32_t> v;
  for (uint32_t i = 0; i < 100; ++i) {
    uint32_t const x{ i * 3 };
    vec_push_back(v, x);          // lvalue
    vec_push_back(v, i * 3 + 1);  // rvalue
  }
  REQUIRE(v.size() == 200);
  for (size_t i = 0; i < 100; ++i) {
    CHECK(v[2 * i] == i * 3);
    CHECK(v[(2 * i) + 1] == (i * 3) + 1);
  }

  auto p{ full<Pair>({ { .a = 1, .b = 2 } }) };
  vec_push_back(p, { .a = 3, .b = 4 });  // braced, reallocating
  CHECK(p == std::vector<Pair>{ { .a = 1, .b = 2 }, { .a = 3, .b = 4 } });
}

TEST_CASE("vec: push_back of an element of the same vector, at capacity") {
  auto v{ full<uint32_t>({ 7, 8, 9 }) };
  vec_push_back(v, v[0]);
  CHECK(v == std::vector<uint32_t>{ 7, 8, 9, 7 });

  auto o{ full<Owned>({ { 1, 2, 3 }, { 4 } }) };
  vec_push_back(o, o[0]);
  CHECK(o == std::vector<Owned>{ { 1, 2, 3 }, { 4 }, { 1, 2, 3 } });
  vec_push_back(o, std::move(o[1]));
  CHECK(o[3] == Owned{ 4 });
  CHECK(o[1].empty());
}

TEST_CASE("vec: assign a count of one value") {
  std::vector<uint32_t> v{ 1, 2, 3, 4, 5 };
  vec_assign(v, 2, 9);  // shrink
  CHECK(v == std::vector<uint32_t>{ 9, 9 });
  vec_assign(v, 4, 8);  // grow within capacity
  CHECK(v == std::vector<uint32_t>{ 8, 8, 8, 8 });
  vec_assign(v, 64, 7);  // reallocate
  CHECK(v == std::vector<uint32_t>(64, 7));
  vec_assign(v, 0, 1);
  CHECK(v.empty());

  std::vector<Owned> o{ { 1 }, { 2 } };
  vec_assign(o, 3, Owned{ 5, 6 });
  CHECK(o == std::vector<Owned>(3, Owned{ 5, 6 }));
}

TEST_CASE("vec: assign a count of an element of the same vector") {
  auto v{ full<uint32_t>({ 4, 5, 6 }) };
  vec_assign(v, 32, v[1]);  // reallocating frees the element named
  CHECK(v == std::vector<uint32_t>(32, 5));
  vec_assign(v, 2, v[31]);
  CHECK(v == std::vector<uint32_t>{ 5, 5 });

  auto o{ full<Owned>({ { 1, 2 }, { 3 } }) };
  vec_assign(o, 16, o[0]);
  CHECK(o == std::vector<Owned>(16, Owned{ 1, 2 }));
}

TEST_CASE("vec: assign a range and a list") {
  std::vector<uint32_t> const src{ 1, 2, 3, 4, 5, 6, 7, 8 };
  std::vector<uint32_t> v{ 9 };
  vec_assign(v, src.begin(), src.end());
  CHECK(v == src);
  vec_assign(v, src.begin() + 2, src.end() - 3);
  CHECK(v == std::vector<uint32_t>{ 3, 4, 5 });
  vec_assign(v, src.data(), src.data());
  CHECK(v.empty());
  vec_assign(v, { 10, 11 });
  CHECK(v == std::vector<uint32_t>{ 10, 11 });
}

TEST_CASE("vec: resize value-initializes growth and keeps the prefix") {
  std::vector<uint32_t> v{ 3, 1, 4 };
  vec_resize(v, 3);
  CHECK(v == std::vector<uint32_t>{ 3, 1, 4 });
  vec_resize(v, 6);
  CHECK(v == std::vector<uint32_t>{ 3, 1, 4, 0, 0, 0 });
  vec_resize(v, 2);
  CHECK(v == std::vector<uint32_t>{ 3, 1 });
  vec_resize(v, 0);
  CHECK(v.empty());

  std::vector<Owned> o{ { 1 } };
  vec_resize(o, 3);
  CHECK(o == std::vector<Owned>{ { 1 }, {}, {} });
}

TEST_CASE("vec: resize with a value, including one from the same vector") {
  std::vector<uint32_t> v{ 1, 2 };
  vec_resize(v, 5, 7);
  CHECK(v == std::vector<uint32_t>{ 1, 2, 7, 7, 7 });
  vec_resize(v, 1, 9);  // shrinking ignores the value
  CHECK(v == std::vector<uint32_t>{ 1 });

  auto w{ full<uint32_t>({ 6, 5 }) };
  vec_resize(w, 20, w[1]);
  CHECK(w == std::vector<uint32_t>{ 6, 5, 5, 5, 5, 5, 5, 5, 5, 5,
                                    5, 5, 5, 5, 5, 5, 5, 5, 5, 5 });

  auto o{ full<Owned>({ { 1, 2 } }) };
  vec_resize(o, 3, o[0]);
  CHECK(o == std::vector<Owned>(3, Owned{ 1, 2 }));
}

TEST_CASE("vec: reserve raises capacity and nothing else") {
  std::vector<uint32_t> v{ 1, 2, 3 };
  vec_reserve(v, 100);
  CHECK(v.capacity() >= 100);
  CHECK(v == std::vector<uint32_t>{ 1, 2, 3 });
  size_t const cap{ v.capacity() };
  vec_reserve(v, 1);
  CHECK(v.capacity() == cap);
}

TEST_CASE("vec: insert a range or a list at the front, middle and end") {
  std::vector<uint32_t> const src{ 7, 8 };
  std::vector<uint32_t> v{ 1, 2, 3 };
  auto at{ vec_insert(v, v.begin() + 1, src.begin(), src.end()) };
  CHECK(v == std::vector<uint32_t>{ 1, 7, 8, 2, 3 });
  CHECK(at == v.begin() + 1);
  at = vec_insert(v, v.begin(), src.data(), src.data() + 1);
  CHECK(v == std::vector<uint32_t>{ 7, 1, 7, 8, 2, 3 });
  CHECK(at == v.begin());
  at = vec_insert(v, v.end(), { 5, 6 });
  CHECK(v == std::vector<uint32_t>{ 7, 1, 7, 8, 2, 3, 5, 6 });
  CHECK(at == v.begin() + 6);
  at = vec_insert(v, v.begin() + 2, src.begin(), src.begin());
  CHECK(at == v.begin() + 2);
  CHECK(v.size() == 8);
}

TEST_CASE("vec: a random run of every helper matches the members") {
  std::vector<uint32_t> got;
  std::vector<uint32_t> want;
  for (uint64_t step = 0; step < 4000; ++step) {
    uint64_t const r{ rnd(3, step) };
    size_t const n{ (r >> 8U) % 40U };
    auto const x{ static_cast<uint32_t>(r >> 32U) };
    switch (r % 8U) {
      case 0:
        vec_push_back(got, x);
        want.push_back(x);
        break;
      case 1:  // an element of the same vector
        vec_push_back(got, got.empty() ? x : got[0]);
        want.push_back(want.empty() ? x : want[0]);
        break;
      case 2:
        vec_assign(got, n, x);
        want.assign(n, x);
        break;
      case 3:
        vec_resize(got, n);
        want.resize(n);
        break;
      case 4:
        vec_resize(got, n, x);
        want.resize(n, x);
        break;
      case 5:
        vec_reserve(got, n);
        want.reserve(n);
        break;
      case 6: {
        std::vector<uint32_t> const src(n % 5U, x);
        size_t const at{ want.empty() ? 0 : (n % want.size()) };
        vec_insert(got, got.begin() + static_cast<ptrdiff_t>(at), src.begin(), src.end());
        want.insert(want.begin() + static_cast<ptrdiff_t>(at), src.begin(), src.end());
        break;
      }
      default: {
        std::vector<uint32_t> const src(n, x ^ 1U);
        vec_assign(got, src.begin(), src.end());
        want.assign(src.begin(), src.end());
        break;
      }
    }
    REQUIRE_MESSAGE(got == want, "step " << step);
  }
}
