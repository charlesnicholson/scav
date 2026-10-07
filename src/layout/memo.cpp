#include "layout/memo.h"

#include "scav_int.h"
#include "scav_thread.h"
#include "scav_vec.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace scav {

// Mixes eight-word blocks into four lanes, two words per lane, then folds the lanes and
// the tail into one hash.
uint64_t memo_hash(std::vector<uint32_t> const &key) {
  return memo_hash(key.data(), key.size());
}

uint64_t memo_hash(uint32_t const *key, size_t len) {
  constexpr uint64_t K{ UINT64_C(0x9E37'79B9'7F4A'7C15) };
  auto const mix = [](uint64_t h, uint64_t w) {
    h = (h ^ w) * K;
    return h ^ (h >> 29U);
  };
  std::array<uint64_t, 4> lane{ 1, 2, 3, 4 };
  size_t const n{ len };
  size_t i{ 0 };
  for (; (i + 8) <= n; i += 8) {
    for (size_t l = 0; l < lane.size(); ++l) {
      uint64_t const w{ (uint64_t{ key[i + (2 * l)] } << 32U) | key[i + (2 * l) + 1] };
      lane[l] = mix(lane[l], w);
    }
  }
  uint64_t h{ n };
  for (uint64_t const l : lane) { h = mix(h, l); }
  for (; i < n; ++i) { h = mix(h, key[i]); }
  return h;
}

namespace {

struct Registry {
  Mutex lock;
  std::vector<Memo *> memos;
  uint32_t open{ 0 };  // live `MemoRun`s
  uint32_t serial{ 0 };
};

// Never destroyed; memos on pool threads deregister after static destruction.
Registry &registry() {
  static Registry *const INSTANCE{ new Registry };
  return *INSTANCE;
}

}  // namespace

Memo::Memo(size_t words, MemoHash hash) : budget(words), hash_of(hash) {
  Registry &r{ registry() };
  ScopedLock const held{ r.lock };
  vec_push_back(r.memos, this);
}

Memo::~Memo() {
  Registry &r{ registry() };
  ScopedLock const held{ r.lock };
  for (size_t k = 0; k < r.memos.size(); ++k) {
    if (r.memos[k] == this) {
      r.memos[k] = r.memos.back();
      r.memos.pop_back();
      break;
    }
  }
}

void Memo::release() {
  std::vector<uint32_t>{}.swap(keys);
  std::vector<int32_t>{}.swap(values);
  std::vector<Slot>{}.swap(slots);
  used = 0;
}

uint32_t memo_serial() {
  Registry &r{ registry() };
  ScopedLock const held{ r.lock };
  if (++r.serial == 0) { ++r.serial; }
  return r.serial;
}

uint32_t memo_profile(scav_profile const &p) {
  struct Entry {
    scav_profile p;
    uint32_t word;
  };
  thread_local std::array<Entry, 8> seen{};
  thread_local uint32_t next{ 0 };
  for (Entry const &e : seen) {
    if ((e.word != 0) && (std::memcmp(&e.p, &p, sizeof(scav_profile)) == 0)) {
      return e.word;
    }
  }
  Entry &e{ seen[next] };
  next = (next + 1) % seen.size();
  e = { .p = p, .word = memo_serial() };
  return e.word;
}

MemoRun::MemoRun() {
  Registry &r{ registry() };
  ScopedLock const held{ r.lock };
  ++r.open;
}

MemoRun::~MemoRun() {
  Registry &r{ registry() };
  ScopedLock const held{ r.lock };
  if (--r.open != 0) { return; }
  for (Memo *const m : r.memos) { m->release(); }
}

// The slot holding `key`, or the empty slot it goes in; the table is at most half full.
Memo::Slot &Memo::slot_of(uint64_t hash, std::vector<uint32_t> const &key) {
  size_t const mask{ slots.size() - 1 };
  for (size_t at = static_cast<size_t>(hash) & mask;; at = (at + 1) & mask) {
    Slot &slot{ slots[at] };
    if (slot.key_len == 0) { return slot; }
    if ((slot.hash == hash) && (slot.key_len == key.size()) &&
        (std::memcmp(keys.data() + slot.key_off,
                     key.data(),
                     key.size() * sizeof(uint32_t)) == 0)) {
      return slot;
    }
  }
}

// Doubles the slots (at least 1024) and reinserts every entry by its stored hash.
void Memo::grow() {
  std::vector<Slot> old;
  old.swap(slots);
  vec_assign(slots, imax(old.size() * 2, size_t{ 1024 }), Slot{});
  size_t const mask{ slots.size() - 1 };
  for (Slot const &slot : old) {
    if (slot.key_len == 0) { continue; }
    size_t at{ static_cast<size_t>(slot.hash) & mask };
    while (slots[at].key_len != 0) { at = (at + 1) & mask; }
    slots[at] = slot;
  }
}

bool Memo::find(std::vector<uint32_t> const &key, int32_t const *&at, uint32_t &len) {
  if (slots.empty() || key.empty()) { return false; }
  Slot const &slot{ slot_of(hash_of(key), key) };
  if (slot.key_len == 0) { return false; }
  at = values.data() + slot.value_off;
  len = slot.value_len;
  return true;
}

KeyIndex::KeyIndex(KeyIndex &&) noexcept = default;
KeyIndex &KeyIndex::operator=(KeyIndex &&) noexcept = default;
KeyIndex::~KeyIndex() = default;
static_assert(std::is_nothrow_move_constructible_v<KeyIndex> &&
              std::is_nothrow_move_assignable_v<KeyIndex>);

size_t KeyIndex::slot_of(uint32_t const *key, uint32_t len, uint64_t hash) const {
  size_t const mask{ slots.size() - 1 };
  uint32_t const check{ static_cast<uint32_t>(hash >> 32U) };
  for (size_t at = check & mask;; at = (at + 1) & mask) {
    Slot const &slot{ slots[at] };
    if (slot.entry == 0) { return at; }
    if (slot.check != check) { continue; }
    Entry const &e{ entries[slot.entry - 1] };
    if ((e.len == len) &&
        ((len == 0) ||
         (std::memcmp(keys.data() + e.off, key, size_t{ len } * sizeof(uint32_t)) == 0))) {
      return at;
    }
  }
}

uint32_t KeyIndex::find(uint32_t const *key, uint32_t len, uint64_t hash) const {
  if (slots.empty()) { return INVALID; }
  uint32_t const entry{ slots[slot_of(key, len, hash)].entry };
  return (entry == 0) ? INVALID : (entry - 1);
}

// Doubles the slots (at least 64) and reinserts every slot at its check word.
void KeyIndex::grow() {
  std::vector<Slot> old;
  old.swap(slots);
  vec_assign(slots, imax(old.size() * 2, size_t{ 64 }), Slot{});
  size_t const mask{ slots.size() - 1 };
  for (Slot const &slot : old) {
    if (slot.entry == 0) { continue; }
    size_t at{ slot.check & mask };
    while (slots[at].entry != 0) { at = (at + 1) & mask; }
    slots[at] = slot;
  }
}

uint32_t KeyIndex::insert(uint32_t const *key, uint32_t len, uint64_t hash) {
  constexpr size_t LIMIT{ size_t{ UINT32_MAX } - 1 };
  if (((keys.size() + len) > LIMIT) || (entries.size() >= LIMIT)) { return INVALID; }
  if (((entries.size() + 1) * 2) > slots.size()) { grow(); }
  uint32_t const n{ static_cast<uint32_t>(entries.size()) };
  slots[slot_of(key, len, hash)] = { .check = static_cast<uint32_t>(hash >> 32U),
                                     .entry = n + 1 };
  vec_push_back(entries, { .off = static_cast<uint32_t>(keys.size()), .len = len });
  vec_insert(keys, keys.end(), key, key + len);
  return n;
}

void Memo::insert(std::vector<uint32_t> const &key, std::vector<int32_t> const &value) {
  if (key.empty()) { return; }
  if ((keys.size() + key.size() + values.size() + value.size()) > budget) {
    keys.clear();
    values.clear();
    vec_assign(slots, slots.size(), Slot{});
    used = 0;
  }
  if (((size_t{ used } + 1) * 2) > slots.size()) { grow(); }
  uint64_t const hash{ hash_of(key) };
  Slot &slot{ slot_of(hash, key) };
  slot = { .hash = hash,
           .key_off = static_cast<uint32_t>(keys.size()),
           .key_len = static_cast<uint32_t>(key.size()),
           .value_off = static_cast<uint32_t>(values.size()),
           .value_len = static_cast<uint32_t>(value.size()) };
  vec_insert(keys, keys.end(), key.begin(), key.end());
  vec_insert(values, values.end(), value.begin(), value.end());
  ++used;
}

}  // namespace scav
