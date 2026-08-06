#ifndef SCAV_CORE_MODEL_LOOKUP_MAP_H_INCLUDED
#define SCAV_CORE_MODEL_LOOKUP_MAP_H_INCLUDED

// unordered_map's iteration order differs across standard libraries, so "never
// iterated" is a compile error here rather than a review comment: no begin() or
// end(). Lookup itself is fine -- a hash value never escapes as a bucket index.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace scav {

// Transparent, so probing with a string_view over bytes already in hand does not
// allocate a std::string per lookup. Interning runs once per token.
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

  [[nodiscard]] size_t size() const { return map.size(); }
  [[nodiscard]] bool empty() const { return map.empty(); }
  void clear() { map.clear(); }
  void reserve(size_t n) { map.reserve(n); }

 private:
  std::unordered_map<K, V, Hash, Eq> map;
};

// The string-keyed spelling, which is the only one P0 needs.
template <typename V>
using StringLookupMap = LookupMap<std::string, V, StringViewHash, StringViewEqual>;

}  // namespace scav

#endif  // SCAV_CORE_MODEL_LOOKUP_MAP_H_INCLUDED
