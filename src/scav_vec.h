#ifndef SCAV_VEC_H_INCLUDED
#define SCAV_VEC_H_INCLUDED

// std::vector's growth operations with each allocating path out of line, one
// instantiation per element type; the in-capacity paths stay inline.

#include <cstddef>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#  define SCAV_NOINLINE __declspec(noinline)
#else
#  define SCAV_NOINLINE [[gnu::noinline]]
#endif

namespace scav {

// x is taken by value, so it may name an element of v.
template <typename T>
// NOLINTNEXTLINE(performance-unnecessary-value-param)
SCAV_NOINLINE void vec_assign(std::vector<T> &v, size_t n, std::type_identity_t<T> x) {
  v.assign(n, x);
}

template <typename T, typename It>
  requires(!std::is_integral_v<It>)
SCAV_NOINLINE void vec_assign(std::vector<T> &v, It first, It last) {
  v.assign(first, last);
}

template <typename T>
void vec_assign(std::vector<T> &v, std::initializer_list<std::type_identity_t<T>> il) {
  vec_assign(v, il.begin(), il.end());
}

template <typename T>
SCAV_NOINLINE void vec_reserve(std::vector<T> &v, size_t n) {
  v.reserve(n);
}

template <typename T>
SCAV_NOINLINE void vec_resize_grow(std::vector<T> &v, size_t n) {
  v.resize(n);
}

template <typename T>
SCAV_NOINLINE void vec_resize_grow(std::vector<T> &v, size_t n, T const &x) {
  v.resize(n, x);
}

template <typename T>
void vec_resize(std::vector<T> &v, size_t n) {
  if (n <= v.size()) {
    v.resize(n);
  } else {
    vec_resize_grow(v, n);
  }
}

template <typename T>
void vec_resize(std::vector<T> &v, size_t n, std::type_identity_t<T> const &x) {
  if (n <= v.size()) {
    v.resize(n);
  } else {
    vec_resize_grow(v, n, x);
  }
}

template <typename T, typename... A>
SCAV_NOINLINE T &vec_emplace_back_grow(std::vector<T> &v, A &&...a) {
  return v.emplace_back(std::forward<A>(a)...);
}

// Compares as pointers, as emplace_back does, so its inlined reallocation folds away.
template <typename T>
bool vec_has_room(std::vector<T> const &v) {
  return (v.data() + v.size()) < (v.data() + v.capacity());
}

template <typename T, typename... A>
T &vec_emplace_back(std::vector<T> &v, A &&...a) {
  if (vec_has_room(v)) [[likely]] { return v.emplace_back(std::forward<A>(a)...); }
  return vec_emplace_back_grow(v, std::forward<A>(a)...);
}

template <typename T>
void vec_push_back(std::vector<T> &v, std::type_identity_t<T> const &x) {
  if (vec_has_room(v)) [[likely]] {
    v.push_back(x);
  } else {
    vec_emplace_back_grow(v, x);
  }
}

template <typename T>
void vec_push_back(std::vector<T> &v, std::type_identity_t<T> &&x) {
  if (vec_has_room(v)) [[likely]] {
    v.push_back(std::move(x));
  } else if constexpr (std::is_trivially_copyable_v<T>) {
    vec_emplace_back_grow(v, static_cast<T const &>(x));  // the copying instantiation
  } else {
    vec_emplace_back_grow(v, std::move(x));
  }
}

template <typename T, typename It>
SCAV_NOINLINE typename std::vector<T>::iterator vec_insert(
    std::vector<T> &v,
    typename std::vector<T>::const_iterator pos,
    It first,
    It last) {
  return v.insert(pos, first, last);
}

template <typename T>
typename std::vector<T>::iterator vec_insert(
    std::vector<T> &v,
    typename std::vector<T>::const_iterator pos,
    std::initializer_list<std::type_identity_t<T>> il) {
  return vec_insert(v, pos, il.begin(), il.end());
}

}  // namespace scav

#endif  // SCAV_VEC_H_INCLUDED
