#ifndef SCAV_LOOKUP_H_INCLUDED
#define SCAV_LOOKUP_H_INCLUDED

// Keyed lookup and insertion with no iteration, so "never iterated" is a
// compile error; bucket order and hash values cannot reach output.

#include <cstddef>
#include <unordered_map>
#include <utility>

namespace scav {

template <typename K, typename V, typename Hash = std::hash<K>>
class Lookup {
public:
  // False when the key is already present; the stored value is untouched.
  bool insert(K const &key, V value) {
    return map.emplace(key, std::move(value)).second;
  }

  // Null when absent, so presence and access are one probe.
  [[nodiscard]] V *find(K const &key) {
    auto const it{ map.find(key) };
    return (it == map.end()) ? nullptr : &it->second;
  }
  [[nodiscard]] V const *find(K const &key) const {
    auto const it{ map.find(key) };
    return (it == map.end()) ? nullptr : &it->second;
  }

  [[nodiscard]] bool contains(K const &key) const { return map.contains(key); }
  [[nodiscard]] size_t size() const { return map.size(); }
  [[nodiscard]] bool empty() const { return map.empty(); }
  void clear() { map.clear(); }
  void reserve(size_t count) { map.reserve(count); }

private:
  std::unordered_map<K, V, Hash> map;
};

}  // namespace scav

#endif  // SCAV_LOOKUP_H_INCLUDED
