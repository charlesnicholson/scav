// Lookup, insertion, and the absence of iteration -- the last one asserted at
// compile time, which is the type's whole job.

#include "scav_lookup_map.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace {

using namespace scav;

// The compile-time contract: no begin, no end, so no range-for and no
// <algorithm> can see an order.
template <typename T>
concept Iterable = requires(T t) { t.begin(); };

static_assert(!Iterable<LookupMap<uint32_t, uint32_t>>);
static_assert(!Iterable<LookupMap<std::string, uint32_t> const>);

}  // namespace

TEST_CASE("lookup_map: insert claims a key once and find sees the first value") {
  LookupMap<std::string, uint32_t> map;
  CHECK(map.empty());
  CHECK(map.insert("a", 1));
  CHECK(!map.insert("a", 2));
  CHECK(map.size() == 1);
  REQUIRE(map.find("a") != nullptr);
  CHECK(*map.find("a") == 1);
  CHECK(map.find("b") == nullptr);
  CHECK(map.contains("a"));
  CHECK(!map.contains("b"));
}

TEST_CASE("lookup_map: found values mutate in place and clear empties") {
  LookupMap<uint32_t, uint32_t> map;
  map.reserve(64);
  for (uint32_t i = 0; i < 64; ++i) { CHECK(map.insert(i, i * 3)); }
  CHECK(map.size() == 64);
  *map.find(7) = 99;
  CHECK(*map.find(7) == 99);

  LookupMap<uint32_t, uint32_t> const &view{ map };
  REQUIRE(view.find(7) != nullptr);
  CHECK(*view.find(7) == 99);

  map.clear();
  CHECK(map.empty());
  CHECK(map.find(7) == nullptr);
}
