// The shipped profiles load and validate; every bound rejects; print_columns
// agrees with the printer's default.

#include "scav/scav_core.h"
#include "scav/scav_core_c.h"
#include "scav/scav_layout.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <ostream>

namespace {

using namespace scav;

Profile named(char const *name) {
  Profile p{};
  REQUIRE(profile_named(name, p));
  return p;
}

}  // namespace

TEST_CASE("profile: both shipped profiles load and pass their own validation") {
  for (char const *name : { "compact", "readable" }) {
    CAPTURE(name);
    Profile const p{ named(name) };
    CHECK(profile_validate(p));
    CHECK(p.profile_version == 1);
  }
  CHECK(named("compact").profile_id != named("readable").profile_id);
}

TEST_CASE("profile: an unknown name is refused and writes nothing") {
  Profile p{};
  p.pad = 77;
  CHECK(!profile_named("ornate", p));
  CHECK(!profile_named(nullptr, p));
  CHECK(p.pad == 77);
}

TEST_CASE("profile: print_columns matches the printer's default") {
  // The one field the printer reads; a shipped profile drifting from the
  // printer's own default would print differently through layout than fmt.
  CHECK(named("compact").print_columns == static_cast<int32_t>(DEFAULT_PRINT_COLUMNS));
  CHECK(named("readable").print_columns == static_cast<int32_t>(DEFAULT_PRINT_COLUMNS));
}

TEST_CASE("profile: every bound rejects out of range") {
  struct Poke {
    char const *what;
    int32_t Profile::*field;
    int32_t bad_low, bad_high;
  };
  // One below the floor and one above the ceiling per scalar field.
  constexpr int32_t I32_MAX{ 0x7FFF'FFFF };
  std::array const pokes{
    Poke{ "profile_id", &Profile::profile_id, -1, I32_MAX },
    Poke{ "profile_version", &Profile::profile_version, 0, I32_MAX },
    Poke{ "pad", &Profile::pad, -1, SPACE_MAX + 1 },
    Poke{ "font_size_grid", &Profile::font_size_grid, 0, SPACE_MAX + 1 },
    Poke{ "line_height_k_num", &Profile::line_height_k_num, 0, 1025 },
    Poke{ "line_height_k_den", &Profile::line_height_k_den, 0, 1025 },
    Poke{ "dar_num", &Profile::dar_num, 0, 1025 },
    Poke{ "dar_den", &Profile::dar_den, 0, 1025 },
    Poke{ "trybox", &Profile::trybox, -1, 2 },
    Poke{ "sm_tiebreak", &Profile::sm_tiebreak, -1, 2 },
    Poke{ "w_bends", &Profile::w_bends, -1, 1025 },
    Poke{ "w_area", &Profile::w_area, -1, 1025 },
    Poke{ "portfolio_k", &Profile::portfolio_k, 0, 65 },
    Poke{ "sweep_count", &Profile::sweep_count, -1, 1025 },
    Poke{ "congestion_iterations", &Profile::congestion_iterations, -1, 1025 },
    Poke{ "ripup_cap", &Profile::ripup_cap, -1, 1025 },
    Poke{ "spacing_inflation_cap", &Profile::spacing_inflation_cap, -1, 1025 },
    Poke{ "spacing_inflation_increment", &Profile::spacing_inflation_increment, -1,
      SPACE_MAX + 1 },
    Poke{ "print_columns", &Profile::print_columns,
      static_cast<int32_t>(PRINT_COLUMNS_MIN) - 1,
      static_cast<int32_t>(PRINT_COLUMNS_MAX) + 1 },
  };
  for (Poke const &poke : pokes) {
    CAPTURE(poke.what);
    Profile low{ named("readable") };
    low.*poke.field = poke.bad_low;
    CHECK(!profile_validate(low));
    Profile high{ named("readable") };
    high.*poke.field = poke.bad_high;
    // I32_MAX marks an unbounded ceiling: the value must still validate.
    CHECK(profile_validate(high) == (poke.bad_high == I32_MAX));
  }

  Profile kinds{ named("readable") };
  kinds.kind_min_w[5] = -1;
  CHECK(!profile_validate(kinds));
  kinds = named("readable");
  kinds.kind_min_h[8] = SPACE_MAX + 1;
  CHECK(!profile_validate(kinds));
}

TEST_CASE("profile: the C surface round-trips named, validate, and null args") {
  scav_profile p{};
  REQUIRE(scav_profile_named("compact", &p) == SCAV_OK);
  CHECK(scav_profile_validate(&p) == SCAV_OK);
  p.dar_num = 0;
  CHECK(scav_profile_validate(&p) == SCAV_E_INVALID_ARG);
  CHECK(scav_profile_named("ornate", &p) == SCAV_E_INVALID_ARG);
  CHECK(scav_profile_named(nullptr, &p) == SCAV_E_INVALID_ARG);
  CHECK(scav_profile_named("compact", nullptr) == SCAV_E_INVALID_ARG);
  CHECK(scav_profile_validate(nullptr) == SCAV_E_INVALID_ARG);
}
