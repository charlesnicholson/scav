#include "layout/memo.h"

#include "scav_int.h"
#include "scav_thread.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace scav {

// Four independent lanes over two words at a time, so their multiplies overlap; then
// the lanes and the tail fold into one.
uint64_t memo_hash(std::vector<uint32_t> const &key) {
  constexpr uint64_t K{ UINT64_C(0x9E37'79B9'7F4A'7C15) };
  auto const mix = [](uint64_t h, uint64_t w) {
    h = (h ^ w) * K;
    return h ^ (h >> 29U);
  };
  std::array<uint64_t, 4> lane{ 1, 2, 3, 4 };
  size_t const n{ key.size() };
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

// Every memo alive, and how many layouts are open. Never destroyed: a pool
// thread's memos outlive static destruction and deregister as they go.
struct Registry {
  Mutex lock;
  std::vector<Memo *> memos;
  uint32_t open{ 0 };
};

Registry &registry() {
  static Registry *const INSTANCE{ new Registry };
  return *INSTANCE;
}

}  // namespace

Memo::Memo(size_t words, MemoHash hash) : budget(words), hash_of(hash) {
  Registry &r{ registry() };
  ScopedLock const held{ r.lock };
  r.memos.push_back(this);
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

// The slot holding `key`, or the empty one it would go in; the table is never
// more than half full, so the probe ends.
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

// Doubles the slots and re-seats every entry by its stored hash.
void Memo::grow() {
  std::vector<Slot> old;
  old.swap(slots);
  slots.assign(imax(old.size() * 2, size_t{ 1024 }), Slot{});
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

void Memo::insert(std::vector<uint32_t> const &key, std::vector<int32_t> const &value) {
  if (key.empty()) { return; }
  if ((keys.size() + key.size() + values.size() + value.size()) > budget) {
    keys.clear();
    values.clear();
    slots.assign(slots.size(), Slot{});
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
  keys.insert(keys.end(), key.begin(), key.end());
  values.insert(values.end(), value.begin(), value.end());
  ++used;
}

}  // namespace scav
