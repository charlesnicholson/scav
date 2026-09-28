#ifndef SCAV_LAYOUT_MEMO_H_INCLUDED
#define SCAV_LAYOUT_MEMO_H_INCLUDED

// A table from a key of words to a value of words, for a phase whose result is
// a function of inputs it can write down in full. Keys and values sit back to
// back in two arenas and an open-addressed slot names each by offset, so an
// entry costs no allocation of its own and a hit reads three places. The key is
// compared whole, so a hit returns exactly what was stored under that key.

#include <cstdint>
#include <vector>

namespace scav {

// The hash a table probes by. Only where a key starts looking is up to it; a
// hit is still the whole key compared, so any function is correct and a
// constant one makes every key collide, which is how a test reaches probing.
using MemoHash = uint64_t (*)(std::vector<uint32_t> const &key);
uint64_t memo_hash(std::vector<uint32_t> const &key);

class Memo {
 public:
  // `words` is what both arenas may hold together before the table empties
  // and starts again, which bounds it without an eviction order.
  explicit Memo(size_t words, MemoHash hash = memo_hash) : budget(words), hash_of(hash) {}

  // Whether `key` is stored, and where its value is when it is: `at` and `len`,
  // valid until the next `insert`. An empty value is found with `len` 0.
  [[nodiscard]] bool find(std::vector<uint32_t> const &key,
                          int32_t const *&at,
                          uint32_t &len);

  // Stores `value` under `key`, which must not be present.
  void insert(std::vector<uint32_t> const &key, std::vector<int32_t> const &value);

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

}  // namespace scav

#endif  // SCAV_LAYOUT_MEMO_H_INCLUDED
