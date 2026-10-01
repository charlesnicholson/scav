#ifndef SCAV_LAYOUT_MEMO_H_INCLUDED
#define SCAV_LAYOUT_MEMO_H_INCLUDED

// A table from a key of words to a value of words, for a phase whose result is
// a function of inputs it can write down in full. Keys and values sit back to
// back in two arenas and an open-addressed slot names each by offset, so an
// entry costs no allocation of its own and a hit reads three places. The key is
// compared whole, so a hit returns exactly what was stored under that key.

#include "scav/scav_layout.h"

#include <cstdint>
#include <vector>

namespace scav {

// A word no other call in this process returns; never 0.
uint32_t memo_serial();

// One word standing for `p` in a key: a `memo_serial` drawn the first time this thread
// meets `p` among the last few it interned, so one word only ever stands for one profile.
uint32_t memo_profile(scav_profile const &p);

// The hash a table probes by. A hit compares the whole key, so any function is correct;
// a constant one makes every key collide.
using MemoHash = uint64_t (*)(std::vector<uint32_t> const &key);
uint64_t memo_hash(std::vector<uint32_t> const &key);

class Memo {
 public:
  // `words` is what both arenas may hold together before the table empties
  // and starts again, which bounds it without an eviction order.
  explicit Memo(size_t words, MemoHash hash = memo_hash);
  ~Memo();
  Memo(Memo const &) = delete;
  Memo &operator=(Memo const &) = delete;

  // Whether `key` is stored, and where its value is when it is: `at` and `len`,
  // valid until the next `insert`. An empty value is found with `len` 0.
  [[nodiscard]] bool find(std::vector<uint32_t> const &key,
                          int32_t const *&at,
                          uint32_t &len);

  // Stores `value` under `key`, which must not be present.
  void insert(std::vector<uint32_t> const &key, std::vector<int32_t> const &value);

  // Drops every entry and the storage the arenas and slots took.
  void release();

  // The words the arenas and slots have claimed, whether or not in use.
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

// Held for the length of a layout. When the last open one ends, every memo in the
// process releases its storage, under the lock a starting layout waits on.
class MemoRun {
 public:
  MemoRun();
  ~MemoRun();
  MemoRun(MemoRun const &) = delete;
  MemoRun &operator=(MemoRun const &) = delete;
};

}  // namespace scav

#endif  // SCAV_LAYOUT_MEMO_H_INCLUDED
