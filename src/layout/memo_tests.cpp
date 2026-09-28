// The memo table on its own: a hit is the value stored under the whole key,
// probing survives every key colliding, growth keeps every entry, and the
// budget empties the table rather than serving what it held.

#include "layout/memo.h"

#include "doctest.h"

#include <cstdint>
#include <vector>

namespace {

using namespace scav;

uint64_t colliding(std::vector<uint32_t> const & /*key*/) { return 7; }

// The value `find` names, copied out, so a check compares contents.
bool lookup(Memo &m, std::vector<uint32_t> const &key, std::vector<int32_t> &value) {
  int32_t const *at{ nullptr };
  uint32_t len{ 0 };
  if (!m.find(key, at, len)) { return false; }
  value.assign(at, at + len);
  return true;
}

std::vector<int32_t> value_for(uint32_t i) {
  return { static_cast<int32_t>(i),
           -static_cast<int32_t>(i),
           static_cast<int32_t>(i * 3U) };
}

}  // namespace

TEST_CASE("memo: a key never stored is not found") {
  Memo m{ 1024 };
  std::vector<int32_t> got;
  CHECK_FALSE(lookup(m, { 1, 2, 3 }, got));
  m.insert({ 1, 2, 3 }, { 9 });
  CHECK_FALSE(lookup(m, { 3, 2, 1 }, got));
}

TEST_CASE("memo: a value comes back exactly as it was stored") {
  Memo m{ 1024 };
  m.insert({ 1, 2, 3 }, { -5, 0, 7, 1 << 30 });
  m.insert({ 4 }, { 11 });
  std::vector<int32_t> got;
  REQUIRE(lookup(m, { 1, 2, 3 }, got));
  CHECK(got == std::vector<int32_t>{ -5, 0, 7, 1 << 30 });
  REQUIRE(lookup(m, { 4 }, got));
  CHECK(got == std::vector<int32_t>{ 11 });
}

TEST_CASE("memo: keys differing in one word or in length are different keys") {
  Memo m{ 1024 };
  m.insert({ 1, 2, 3 }, { 1 });
  std::vector<int32_t> got;
  CHECK_FALSE(lookup(m, { 1, 2, 4 }, got));
  CHECK_FALSE(lookup(m, { 1, 2 }, got));
  CHECK_FALSE(lookup(m, { 1, 2, 3, 0 }, got));
  CHECK_FALSE(lookup(m, { 0, 1, 2, 3 }, got));
  REQUIRE(lookup(m, { 1, 2, 3 }, got));
  CHECK(got == std::vector<int32_t>{ 1 });
}

TEST_CASE("memo: every key colliding, each is still found by its whole key") {
  // One hash for every key, so every lookup walks one probe run and only the
  // key comparison tells the entries apart, through several doublings.
  Memo m{ size_t{ 1 } << 20, colliding };
  constexpr uint32_t N{ 3000 };
  for (uint32_t i = 0; i < N; ++i) { m.insert({ i, i ^ 0x5555U }, value_for(i)); }
  bool all{ true };
  std::vector<int32_t> got;
  for (uint32_t i = 0; i < N; ++i) {
    all = all && lookup(m, { i, i ^ 0x5555U }, got) && (got == value_for(i));
  }
  CHECK(all);
  CHECK_FALSE(lookup(m, { N, N ^ 0x5555U }, got));
}

TEST_CASE("memo: growing the table keeps every entry it had") {
  Memo m{ size_t{ 1 } << 20 };
  constexpr uint32_t N{ 20000 };
  for (uint32_t i = 0; i < N; ++i) { m.insert({ i }, value_for(i)); }
  bool all{ true };
  std::vector<int32_t> got;
  for (uint32_t i = 0; i < N; ++i) {
    all = all && lookup(m, { i }, got) && (got == value_for(i));
  }
  CHECK(all);
}

TEST_CASE("memo: past its budget it empties and starts again") {
  // Each entry is one key word and three value words, so a budget of 40
  // holds ten and the eleventh empties the table first.
  Memo m{ 40 };
  for (uint32_t i = 0; i < 10; ++i) { m.insert({ i + 1 }, value_for(i)); }
  std::vector<int32_t> got;
  REQUIRE(lookup(m, { 1 }, got));
  m.insert({ 100 }, value_for(100));
  CHECK_FALSE(lookup(m, { 1 }, got));
  CHECK_FALSE(lookup(m, { 10 }, got));
  REQUIRE(lookup(m, { 100 }, got));
  CHECK(got == value_for(100));
  // Filled again after the reset, what it holds is what went in since.
  for (uint32_t i = 200; i < 205; ++i) { m.insert({ i }, value_for(i)); }
  bool all{ true };
  for (uint32_t i = 200; i < 205; ++i) {
    all = all && lookup(m, { i }, got) && (got == value_for(i));
  }
  CHECK(all);
}

TEST_CASE("memo: an empty key is never stored, and an empty value is") {
  Memo m{ 1024 };
  m.insert({}, { 1 });
  std::vector<int32_t> got;
  CHECK_FALSE(lookup(m, {}, got));
  m.insert({ 5 }, {});
  got = { 99 };
  REQUIRE(lookup(m, { 5 }, got));
  CHECK(got.empty());
}
