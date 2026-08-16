// xxHash32 against the reference algorithm. The empty-input vectors are the
// published ones; the rest come from an independent implementation.

#include "scav_xxhash.h"

#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;

uint32_t hash(std::string_view text, uint32_t seed = 0) {
  return xxhash32(reinterpret_cast<scav_byte const *>(text.data()), text.size(), seed);
}

struct Vector {
  std::string_view input;
  uint32_t seed;
  uint32_t want;
};

}  // namespace

TEST_CASE("xxh32: the published empty-input vectors") {
  CHECK(hash("", 0) == 0x02CC'5D05U);
  CHECK(hash("", 1) == 0x0B2C'B792U);
}

TEST_CASE("xxh32: reference vectors across every length branch") {
  // Under 16 bytes takes the seed+PRIME5 path; 16 and over runs the four-lane
  // stripe loop; the tail exercises the 4-byte and 1-byte drains.
  constexpr std::array<Vector, 7> VECTORS{ {
      { .input = "a", .seed = 0U, .want = 0x550D'7456U },
      { .input = "abc", .seed = 0U, .want = 0x32D1'53FFU },
      { .input = "scav", .seed = 0U, .want = 0xDEFF'16A0U },
      { .input = "0123456789abcdef", .seed = 0U, .want = 0xC2C4'5B69U },
      { .input = "0123456789abcdef0123456789abcdefX", .seed = 0U, .want = 0xCF4A'7B96U },
      { .input = "xxhash", .seed = 12345U, .want = 0x8D86'4E2FU },
      { .input = "1234567890123456789012345678901234567890",
        .seed = 0U,
        .want = 0x765D'8C05U },
  } };
  for (Vector const &v : VECTORS) {
    CAPTURE(v.input);
    CAPTURE(v.seed);
    CHECK(hash(v.input, v.seed) == v.want);
  }
}

TEST_CASE("xxh32: a null pointer with zero length is the empty input") {
  // The digest of an empty chart takes this path: vector::data() on an empty
  // vector may be null, and dereferencing is what the length guards.
  CHECK(xxhash32(nullptr, 0, 0) == 0x02CC'5D05U);
}

TEST_CASE("xxh32: length is mixed in, so a prefix is not its parent") {
  CHECK(hash("abc") != hash("abcd"));
  CHECK(hash(std::string_view{ "abc\0", 4 }) != hash("abc"));
}

TEST_CASE("xxh32: the seed changes the digest") {
  CHECK(hash("scav", 0) != hash("scav", 1));
  CHECK(hash("0123456789abcdef", 0) != hash("0123456789abcdef", 7));
}

TEST_CASE("xxh32: a one-bit change at any position moves the digest") {
  // Avalanche, checked rather than assumed: a hash that ignored its tail would
  // pass every fixed vector above and still be useless as a golden.
  std::string base(64, 'a');
  uint32_t const want{ hash(base) };
  for (size_t i = 0; i < base.size(); ++i) {
    std::string mutated{ base };
    mutated[i] = static_cast<char>(mutated[i] ^ 1);
    CAPTURE(i);
    CHECK(hash(mutated) != want);
  }
}

TEST_CASE("xxh32: every length from 0 to 80 is distinct and stable") {
  // Covers each stripe count and every tail remainder, and catches an
  // off-by-one in the drain loops that a handful of vectors would not.
  std::vector<uint32_t> seen;
  for (uint32_t n = 0; n <= 80; ++n) {
    std::string const input(n, 'q');
    uint32_t const h{ hash(input) };
    CHECK(h == hash(input));  // pure
    for (uint32_t const prior : seen) { CHECK(prior != h); }
    seen.push_back(h);
  }
}
