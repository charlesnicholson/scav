#ifndef SCAV_CORE_MODEL_LOOKUP_MAP_H_INCLUDED
#define SCAV_CORE_MODEL_LOOKUP_MAP_H_INCLUDED

// PRD 6: iteration order of unordered_map varies across all three standard
// libraries, so "never iterated where order reaches output" has to be a compile
// error rather than a review comment. This exposes find / contains / at / insert
// and deliberately no begin() or end().
//
// Key lookup itself is deterministic by usage -- a hash value never escapes as a
// bucket index -- which is why the container underneath is still the right one.
//
// The type is a class with methods, which PRD 4 rules out for model data. It is
// not model data: nothing here outlives the call that built it, gets serialized,
// or gets hashed (PRD 4.1's test).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace scav {

// A transparent hash and equality for string keys, so probing with a
// string_view over bytes already in hand does not allocate a std::string per
// lookup. Interning runs once per token, so that allocation is the difference
// between linear and merely-linear-with-a-malloc.
struct StringViewHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
struct StringViewEqual {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const { return a == b; }
};

template <typename K,
          typename V,
          typename Hash = std::hash<K>,
          typename Eq = std::equal_to<K>>
class LookupMap {
 public:
  // Returns nullptr when absent, so a caller cannot accidentally name an
  // iterator and walk off it. Templated on the probe type so a transparent Hash
  // admits heterogeneous lookup; with the default Hash only K compiles.
  template <typename Q>
  [[nodiscard]] V const *find(Q const &key) const {
    auto const it = map.find(key);
    return (it == map.end()) ? nullptr : &it->second;
  }

  template <typename Q>
  [[nodiscard]] V *find(Q const &key) {
    auto const it = map.find(key);
    return (it == map.end()) ? nullptr : &it->second;
  }

  template <typename Q>
  [[nodiscard]] bool contains(Q const &key) const {
    return map.find(key) != map.end();
  }

  // True when the pair was inserted, false when the key was already present. The
  // stored value is left alone in the second case.
  bool insert(K key, V value) {
    return map.emplace(std::move(key), std::move(value)).second;
  }

  [[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(map.size()); }
  [[nodiscard]] bool empty() const { return map.empty(); }
  void clear() { map.clear(); }
  void reserve(uint32_t n) { map.reserve(n); }

 private:
  std::unordered_map<K, V, Hash, Eq> map;
};

// The string-keyed spelling, which is the only one P0 needs.
template <typename V>
using StringLookupMap = LookupMap<std::string, V, StringViewHash, StringViewEqual>;

}  // namespace scav

#endif  // SCAV_CORE_MODEL_LOOKUP_MAP_H_INCLUDED
