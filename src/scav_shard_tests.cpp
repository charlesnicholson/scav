// The counts at every boundary of the formula, and the partition held to
// covering [0, items) exactly once at every shape.

#include "scav_shard.h"

#include "scav/scav_types.h"
#include "scav_int.h"

#include "doctest.h"

#include <array>
#include <bit>
#include <cstdint>
#include <ostream>
#include <vector>

namespace {

using namespace scav;

struct Row {
  uint32_t entities;
  uint32_t want;
};

constexpr std::array<uint32_t, 9> SHARDS{ 1, 2, 3, 5, 8, 13, 16, 255, 256 };
constexpr std::array<uint32_t, 9> ITEMS{ 0, 1, 2, 3, 7, 64, 65, 1000, 4097 };

}  // namespace

TEST_CASE("shard: the count is a clamped power of two of the entity count") {
  constexpr std::array<Row, 18> ROWS{ {
      { .entities = 0U, .want = 1U },
      { .entities = 1U, .want = 1U },
      { .entities = 63U, .want = 1U },
      { .entities = 64U, .want = 1U },
      { .entities = 65U, .want = 2U },
      { .entities = 127U, .want = 2U },
      { .entities = 128U, .want = 2U },
      { .entities = 129U, .want = 4U },
      { .entities = 192U, .want = 4U },
      { .entities = 256U, .want = 4U },
      { .entities = 257U, .want = 8U },
      { .entities = 4096U, .want = 64U },
      { .entities = 8192U, .want = 128U },
      { .entities = 16383U, .want = 256U },
      { .entities = 16384U, .want = 256U },
      { .entities = 16385U, .want = 256U },
      { .entities = 1U << 20U, .want = 256U },
      { .entities = UINT32_MAX, .want = 256U },
  } };
  for (Row const &r : ROWS) {
    CAPTURE(r.entities);
    CHECK(shard_count(r.entities) == r.want);
  }
}

TEST_CASE("shard: the count only ever grows, and never past the ceiling") {
  uint32_t previous{ shard_count(0) };
  for (uint32_t entities = 0; entities <= 40000U; ++entities) {
    uint32_t const count{ shard_count(entities) };
    CHECK(count >= previous);
    CHECK(count >= 1U);
    CHECK(count <= 256U);
    CHECK(std::has_single_bit(count));
    previous = count;
  }
}

TEST_CASE("shard: the count is usable in a constant expression") {
  static_assert(shard_count(0) == 1U);
  static_assert(shard_count(64) == 1U);
  static_assert(shard_count(65) == 2U);
  static_assert(shard_count(4096) == 64U);
  static_assert(shard_count(UINT32_MAX) == 256U);
}

TEST_CASE("shard: the ranges partition [0, items) at every shape") {
  for (uint32_t shards : SHARDS) {
    for (uint32_t items : ITEMS) {
      CAPTURE(shards);
      CAPTURE(items);

      std::vector<uint32_t> covered(items, 0);
      uint32_t next{ 0 };
      uint32_t total{ 0 };
      uint32_t shortest{ UINT32_MAX };
      uint32_t longest{ 0 };
      uint32_t previous_len{ UINT32_MAX };
      bool ordered{ true };

      for (uint32_t shard = 0; shard < shards; ++shard) {
        scav_span const range{ shard_range(shard, shards, items) };
        CHECK(range.off == next);
        CHECK((range.off + range.len) <= items);
        for (uint32_t i = 0; i < range.len; ++i) { covered[range.off + i] += 1U; }
        next = range.off + range.len;
        total += range.len;
        shortest = imin(shortest, range.len);
        longest = imax(longest, range.len);
        ordered = ordered && (range.len <= previous_len);
        previous_len = range.len;
      }

      CHECK(next == items);
      CHECK(total == items);
      CHECK(longest - shortest <= 1U);
      CHECK(ordered);

      uint32_t once{ 0 };
      for (uint32_t c : covered) { once += (c == 1U) ? 1U : 0U; }
      CHECK(once == items);
    }
  }
}

TEST_CASE("shard: more shards than items leaves the tail empty") {
  for (uint32_t items : ITEMS) {
    for (uint32_t shards : SHARDS) {
      if (shards < items) { continue; }
      CAPTURE(shards);
      CAPTURE(items);
      for (uint32_t shard = 0; shard < items; ++shard) {
        CHECK(shard_range(shard, shards, items).len == 1U);
      }
      for (uint32_t shard = items; shard < shards; ++shard) {
        scav_span const range{ shard_range(shard, shards, items) };
        CHECK(range.len == 0U);
        CHECK(range.off == items);
      }
    }
  }
}

TEST_CASE("shard: the range is usable in a constant expression") {
  static_assert(shard_range(0, 1, 0).off == 0U);
  static_assert(shard_range(0, 1, 0).len == 0U);
  static_assert(shard_range(0, 3, 7).off == 0U);
  static_assert(shard_range(0, 3, 7).len == 3U);
  static_assert(shard_range(1, 3, 7).off == 3U);
  static_assert(shard_range(1, 3, 7).len == 2U);
  static_assert(shard_range(2, 3, 7).off == 5U);
  static_assert(shard_range(2, 3, 7).len == 2U);
  static_assert(shard_range(255, 256, UINT32_MAX).off == 4278190080U);
}
