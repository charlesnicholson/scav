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
