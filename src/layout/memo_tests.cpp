// The memo table on its own.

#include "layout/memo.h"

#include "doctest.h"
#include "scav_vector.h"

#include <cstdint>

namespace {

using namespace scav;

uint64_t colliding(Vector<uint32_t> const & /*key*/) { return 7; }

// `find`, with the value copied out.
bool lookup(Memo &m, Vector<uint32_t> const &key, Vector<int32_t> &value) {
  int32_t const *at{ nullptr };
  uint32_t len{ 0 };
  if (!m.find(key, at, len)) { return false; }
  value.assign(at, at + len);
  return true;
}

Vector<int32_t> value_for(uint32_t i) {
  return { static_cast<int32_t>(i),
           -static_cast<int32_t>(i),
           static_cast<int32_t>(i * 3U) };
}

}  // namespace

TEST_CASE("memo: a key never stored is not found") {
  Memo m{ 1024 };
  Vector<int32_t> got;
  CHECK_FALSE(lookup(m, { 1, 2, 3 }, got));
  m.insert({ 1, 2, 3 }, { 9 });
  CHECK_FALSE(lookup(m, { 3, 2, 1 }, got));
}

TEST_CASE("memo: a value comes back exactly as it was stored") {
  Memo m{ 1024 };
  m.insert({ 1, 2, 3 }, { -5, 0, 7, 1 << 30 });
  m.insert({ 4 }, { 11 });
  Vector<int32_t> got;
  REQUIRE(lookup(m, { 1, 2, 3 }, got));
  CHECK(got == Vector<int32_t>{ -5, 0, 7, 1 << 30 });
  REQUIRE(lookup(m, { 4 }, got));
  CHECK(got == Vector<int32_t>{ 11 });
}

TEST_CASE("memo: keys differing in one word or in length are different keys") {
  Memo m{ 1024 };
  m.insert({ 1, 2, 3 }, { 1 });
  Vector<int32_t> got;
  CHECK_FALSE(lookup(m, { 1, 2, 4 }, got));
  CHECK_FALSE(lookup(m, { 1, 2 }, got));
  CHECK_FALSE(lookup(m, { 1, 2, 3, 0 }, got));
  CHECK_FALSE(lookup(m, { 0, 1, 2, 3 }, got));
  REQUIRE(lookup(m, { 1, 2, 3 }, got));
  CHECK(got == Vector<int32_t>{ 1 });
}

TEST_CASE("memo: every key colliding, each is still found by its whole key") {
  // One hash for every key: each lookup walks one probe run, through several doublings.
  Memo m{ size_t{ 1 } << 20, colliding };
  constexpr uint32_t N{ 3000 };
  for (uint32_t i = 0; i < N; ++i) { m.insert({ i, i ^ 0x5555U }, value_for(i)); }
  bool all{ true };
  Vector<int32_t> got;
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
  Vector<int32_t> got;
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
  Vector<int32_t> got;
  REQUIRE(lookup(m, { 1 }, got));
  m.insert({ 100 }, value_for(100));
  CHECK_FALSE(lookup(m, { 1 }, got));
  CHECK_FALSE(lookup(m, { 10 }, got));
  REQUIRE(lookup(m, { 100 }, got));
  CHECK(got == value_for(100));
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
  Vector<int32_t> got;
  CHECK_FALSE(lookup(m, {}, got));
  m.insert({ 5 }, {});
  got = { 99 };
  REQUIRE(lookup(m, { 5 }, got));
  CHECK(got.empty());
}

TEST_CASE("memo: the hash reads every word of a key") {
  // Flipping any one word, or appending one, changes the hash at every length up to 40.
  uint64_t s{ 99 };
  auto const next = [&s]() {
    s = (s * 6364136223846793005ULL) + 1442695040888963407ULL;
    return static_cast<uint32_t>(s >> 32U);
  };
  bool all{ true };
  for (uint32_t len = 1; len <= 40; ++len) {
    Vector<uint32_t> key(len);
    for (uint32_t &w : key) { w = next(); }
    uint64_t const h{ memo_hash(key) };
    for (uint32_t k = 0; k < len; ++k) {
      Vector<uint32_t> other{ key };
      other[k] ^= 1U;
      all = all && (memo_hash(other) != h);
    }
    Vector<uint32_t> longer{ key };
    longer.push_back(0);
    all = all && (memo_hash(longer) != h);
  }
  CHECK(all);
}

TEST_CASE("memo: every memo gives back its storage when the last layout ends") {
  Memo m{ 1024 };
  for (uint32_t i = 0; i < 100; ++i) { m.insert({ i + 1 }, value_for(i)); }
  Vector<int32_t> got;
  {
    MemoRun const outer;
    { MemoRun const inner; }
    // One layout still open: nothing is released under it.
    REQUIRE(lookup(m, { 1 }, got));
    CHECK(m.held() > 0);
  }
  CHECK_FALSE(lookup(m, { 1 }, got));
  CHECK(m.held() == 0);
  // Released, it fills again like a new one.
  m.insert({ 7 }, value_for(7));
  REQUIRE(lookup(m, { 7 }, got));
  CHECK(got == value_for(7));
}

TEST_CASE("memo: a serial is never repeated and an interned profile is one word for it") {
  uint32_t const a{ memo_serial() };
  uint32_t const b{ memo_serial() };
  CHECK(a != 0);
  CHECK(b != 0);
  CHECK(a != b);

  scav_profile p{};
  scav_profile q{};
  q.sweep_count = 7;
  uint32_t const pw{ memo_profile(p) };
  CHECK(pw != 0);
  CHECK(memo_profile(p) == pw);
  CHECK(memo_profile(q) != pw);
  CHECK(memo_profile(p) == pw);
  // Pushed out by eight others, `p` draws a fresh word, never one another profile holds.
  Vector<uint32_t> words;
  for (int32_t k = 0; k < 8; ++k) {
    scav_profile r{};
    r.node_sep = k + 1;
    words.push_back(memo_profile(r));
  }
  uint32_t const again{ memo_profile(p) };
  CHECK(again != pw);
  for (uint32_t const w : words) { CHECK(again != w); }
}

namespace {

uint32_t find_in(KeyIndex const &k, Vector<uint32_t> const &key) {
  return k.find(key.data(), static_cast<uint32_t>(key.size()), memo_hash(key));
}

uint32_t put_in(KeyIndex &k, Vector<uint32_t> const &key) {
  return k.insert(key.data(), static_cast<uint32_t>(key.size()), memo_hash(key));
}

}  // namespace

TEST_CASE("key index: keys are numbered in insertion order and found by their whole key") {
  KeyIndex k;
  CHECK(find_in(k, { 1, 2, 3 }) == INVALID);
  CHECK(put_in(k, { 1, 2, 3 }) == 0);
  CHECK(put_in(k, { 4 }) == 1);
  CHECK(put_in(k, {}) == 2);
  CHECK(k.size() == 3);
  CHECK(find_in(k, { 1, 2, 3 }) == 0);
  CHECK(find_in(k, { 4 }) == 1);
  CHECK(find_in(k, {}) == 2);
  CHECK(find_in(k, { 1, 2, 4 }) == INVALID);
  CHECK(find_in(k, { 1, 2 }) == INVALID);
  CHECK(find_in(k, { 1, 2, 3, 0 }) == INVALID);
  CHECK(find_in(k, { 0, 1, 2, 3 }) == INVALID);
}

TEST_CASE(
    "key index: keys sharing one hash, through several doublings, keep their numbers") {
  KeyIndex k;
  constexpr uint32_t N{ 3000 };
  bool all{ true };
  for (uint32_t i = 0; i < N; ++i) {
    Vector<uint32_t> const key{ i, i ^ 0x5555U };
    all = all && (k.insert(key.data(), 2, 7) == i);
  }
  for (uint32_t i = 0; i < N; ++i) {
    Vector<uint32_t> const key{ i, i ^ 0x5555U };
    all = all && (k.find(key.data(), 2, 7) == i);
  }
  CHECK(all);
  Vector<uint32_t> const absent{ N, N ^ 0x5555U };
  CHECK(k.find(absent.data(), 2, 7) == INVALID);
  CHECK(k.bytes() > 0);
}

TEST_CASE("key index: growing keeps every key it had") {
  KeyIndex k;
  constexpr uint32_t N{ 20000 };
  for (uint32_t i = 0; i < N; ++i) { REQUIRE(put_in(k, { i, i * 3U }) == i); }
  bool all{ true };
  for (uint32_t i = 0; i < N; ++i) { all = all && (find_in(k, { i, i * 3U }) == i); }
  CHECK(all);
}
