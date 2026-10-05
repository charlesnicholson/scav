#ifndef SCAV_LAYOUT_MEMO_H_INCLUDED
#define SCAV_LAYOUT_MEMO_H_INCLUDED

// A hash table from word keys to word values. Keys and values sit back to back in two
// arenas, indexed by open-addressed slots; a hit compares the whole key.

#include "scav/scav_layout.h"

#include <cstdint>
#include <vector>

namespace scav {

// A process-unique word, never 0.
uint32_t memo_serial();

// Key word for `p`: a `memo_serial` cached per thread for its last 8 distinct profiles.
// A word names one profile.
uint32_t memo_profile(scav_profile const &p);

// The probe hash; any function is correct, a constant one included.
using MemoHash = uint64_t (*)(std::vector<uint32_t> const &key);
uint64_t memo_hash(std::vector<uint32_t> const &key);
uint64_t memo_hash(uint32_t const *key, size_t len);  // the same over `key[0..len)`

// An open-addressed index numbering word-string keys 0, 1, 2, ... in insertion order; keys
// sit back to back in one arena, slots at their hash's high word, and a hit compares all.
class KeyIndex {
 public:
  // The number of `key[0..len)`, or INVALID; `hash` is the hash it was stored under.
  [[nodiscard]] uint32_t find(uint32_t const *key, uint32_t len, uint64_t hash) const;

  // Numbers the absent `key[0..len)`; INVALID when the arena cannot hold it.
  uint32_t insert(uint32_t const *key, uint32_t len, uint64_t hash);

  [[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(entries.size()); }

  // Bytes the arena, entries and slots hold.
  [[nodiscard]] size_t bytes() const {
    return (keys.capacity() * sizeof(uint32_t)) + (entries.capacity() * sizeof(Entry)) +
           (slots.capacity() * sizeof(Slot));
  }

 private:
  struct Slot {
    uint32_t check;  // the hash's high word
    uint32_t entry;  // 0 for an empty slot, else the key's number plus 1
  };
  struct Entry {
    uint32_t off, len;  // -> keys
  };
  static_assert(sizeof(Slot) == 8);
  static_assert(sizeof(Entry) == 8);

 public:
  // Bytes a key holds besides its words: its entry and, at most half full, two slots.
  static constexpr size_t ENTRY_BYTES{ sizeof(Entry) + (2 * sizeof(Slot)) };

 private:
  // The slot holding the key, or the empty slot it goes in.
  [[nodiscard]] size_t slot_of(uint32_t const *key, uint32_t len, uint64_t hash) const;
  void grow();

  std::vector<uint32_t> keys;
  std::vector<Entry> entries;
  std::vector<Slot> slots;  // a power of two, at most half full
};

class Memo {
 public:
  // `words` caps both arenas together; an insert past the cap empties the table first.
  explicit Memo(size_t words, MemoHash hash = memo_hash);
  ~Memo();
  Memo(Memo const &) = delete;
  Memo &operator=(Memo const &) = delete;

  // True when `key` is stored; `at` and `len` then give its value (`len` may be 0),
  // valid until the next `insert`.
  [[nodiscard]] bool find(std::vector<uint32_t> const &key,
                          int32_t const *&at,
                          uint32_t &len);

  // Stores `value` under `key`, which must not be present.
  void insert(std::vector<uint32_t> const &key, std::vector<int32_t> const &value);

  // Drops every entry and frees the arenas and slots.
  void release();

  // Capacity of the arenas and slots, in words.
  [[nodiscard]] size_t held() const {
    return keys.capacity() + values.capacity() + (slots.capacity() * (sizeof(Slot) / 4));
  }

 private:
  struct Slot {
    uint64_t hash;
    uint32_t key_off, key_len;  // `key_len` 0 is an empty slot
    uint32_t value_off, value_len;
  };
  Slot &slot_of(uint64_t hash, std::vector<uint32_t> const &key);
  void grow();

  size_t budget;
  MemoHash hash_of;
  std::vector<uint32_t> keys;
  std::vector<int32_t> values;
  std::vector<Slot> slots;
  uint32_t used{ 0 };
};

// Held for the length of a layout. When the last open run ends, every memo in the
// process releases its storage, under the lock `MemoRun()` takes.
class MemoRun {
 public:
  MemoRun();
  ~MemoRun();
  MemoRun(MemoRun const &) = delete;
  MemoRun &operator=(MemoRun const &) = delete;
};

}  // namespace scav

#endif  // SCAV_LAYOUT_MEMO_H_INCLUDED
