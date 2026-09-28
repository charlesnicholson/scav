#include "layout/memo.h"

#include "scav_int.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace scav {

uint64_t memo_hash(std::vector<uint32_t> const &key) {
  uint64_t h{ 0 };
  for (uint32_t const w : key) {
    h = (h ^ w) * UINT64_C(0x9E37'79B9'7F4A'7C15);
    h ^= h >> 29U;
  }
  return h;
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
