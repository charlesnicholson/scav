// The router registry: names in, ids out, both languages.

#include "scav/scav_core_c.h"
#include "scav/scav_layout.h"

#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <ostream>
#include <string_view>

namespace {

using namespace scav;

scav_byte const *bytes_of(std::string_view text) {
  return reinterpret_cast<scav_byte const *>(text.data());
}

}  // namespace

TEST_CASE("router: the registry holds straight and resolves it by name") {
  REQUIRE(router_count() == 1);

  scav_byte const *name{ nullptr };
  uint32_t len{ 0 };
  REQUIRE(router_name(0, name, len));
  CHECK(std::string_view{ reinterpret_cast<char const *>(name), len } == "straight");
  CHECK(!router_name(1, name, len));

  scav_router_id id{ 99 };
  REQUIRE(router_by_name(bytes_of("straight"), 8, id));
  CHECK(id == 0);
  CHECK(!router_by_name(bytes_of("straigh"), 7, id));
  CHECK(!router_by_name(bytes_of("straightx"), 9, id));
  CHECK(!router_by_name(nullptr, 0, id));
}

TEST_CASE("router: the C surface agrees with the C++ one") {
  uint32_t count{ 0 };
  REQUIRE(scav_router_list(&count) == SCAV_OK);
  CHECK(count == router_count());

  scav_byte const *name{ nullptr };
  uint32_t len{ 0 };
  REQUIRE(scav_router_name(0, &name, &len) == SCAV_OK);
  CHECK(scav_router_name(count, &name, &len) == SCAV_E_INVALID_ARG);

  scav_router_id id{ 99 };
  REQUIRE(scav_router_by_name(name, len, &id) == SCAV_OK);
  CHECK(id == 0);
  CHECK(scav_router_by_name(bytes_of("bendy"), 5, &id) == SCAV_E_INVALID_ARG);
  CHECK(scav_router_by_name(nullptr, 0, &id) == SCAV_E_INVALID_ARG);
  CHECK(scav_router_list(nullptr) == SCAV_E_INVALID_ARG);
}
