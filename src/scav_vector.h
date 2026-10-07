#ifndef SCAV_VECTOR_H_INCLUDED
#define SCAV_VECTOR_H_INCLUDED

// A growable array of trivially copyable elements, std::vector's members by name. Every
// element type shares VectorBase's out-of-line growth, copy and free.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace scav {

// `count` elements of `elem` bytes each. Appending past capacity takes max(2 * capacity,
// new size); reserve, assign and copies take exactly; over UINT32_MAX elements aborts.
class VectorBase {
 public:
  VectorBase(VectorBase const &) = delete;
  VectorBase &operator=(VectorBase const &) = delete;

  [[nodiscard]] size_t size() const { return count; }
  [[nodiscard]] bool empty() const { return count == 0; }
  [[nodiscard]] size_t capacity() const { return cap; }
  void clear() { count = 0; }

 protected:
  VectorBase() = default;
  ~VectorBase() = default;

  void reserve(size_t n, size_t elem);
  void *push_grow(void const *x, size_t elem);  // appends *x; x may name an element
  void append_fill(size_t n, void const *x, size_t elem);  // null x appends zero bytes
  void assign_fill(size_t n, void const *x, size_t elem);
  void assign_copy(void const *src, size_t n, size_t elem);
  void copy_from(VectorBase const &o, size_t elem);
  void move_from(VectorBase &o);  // frees this storage and takes o's, leaving o empty
  void *insert_copy(size_t at, void const *src, size_t n, size_t elem);
  void erase(size_t at, size_t n, size_t elem);
  static void release(void *p);

  void *ptr{ nullptr };
  uint32_t count{ 0 };
  uint32_t cap{ 0 };
};

template <typename T>
class Vector : public VectorBase {
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(alignof(T) <= alignof(std::max_align_t));

 public:
  using value_type = T;
  using iterator = T *;
  using const_iterator = T const *;

  Vector() = default;
  explicit Vector(size_t n) { resize(n); }
  Vector(size_t n, T const &x) { assign(n, x); }
  Vector(T const *first, T const *last) { assign(first, last); }
  Vector(std::initializer_list<T> il) { assign(il); }
  Vector(Vector const &o) : VectorBase() { copy_from(o, sizeof(T)); }
  Vector(Vector &&o) noexcept : VectorBase() { swap(o); }
  ~Vector() { release(ptr); }

  Vector &operator=(Vector const &o) {
    copy_from(o, sizeof(T));
    return *this;
  }
  Vector &operator=(Vector &&o) noexcept {
    move_from(o);
    return *this;
  }

  [[nodiscard]] T *data() { return static_cast<T *>(ptr); }
  [[nodiscard]] T const *data() const { return static_cast<T const *>(ptr); }
  [[nodiscard]] T *begin() { return data(); }
  [[nodiscard]] T const *begin() const { return data(); }
  [[nodiscard]] T *end() { return data() + count; }
  [[nodiscard]] T const *end() const { return data() + count; }
  T &operator[](size_t i) { return data()[i]; }
  T const &operator[](size_t i) const { return data()[i]; }
  [[nodiscard]] T &front() { return data()[0]; }
  [[nodiscard]] T const &front() const { return data()[0]; }
  [[nodiscard]] T &back() { return data()[count - 1]; }
  [[nodiscard]] T const &back() const { return data()[count - 1]; }

  void push_back(T const &x) {
    if (count < cap) [[likely]] {
      data()[count] = x;
      ++count;
    } else {
      push_grow(&x, sizeof(T));
    }
  }

  template <typename... A>
  T &emplace_back(A &&...a) {
    T const x(std::forward<A>(a)...);
    push_back(x);
    return back();
  }

  void pop_back() { --count; }

  // Value-initializes the elements it adds.
  void resize(size_t n) {
    if (n <= count) {
      count = static_cast<uint32_t>(n);
    } else if constexpr (std::is_trivially_default_constructible_v<T>) {
      append_fill(n - count, nullptr, sizeof(T));
    } else {
      T const x = T();
      append_fill(n - count, &x, sizeof(T));
    }
  }

  void resize(size_t n, T const &x) {
    if (n <= count) {
      count = static_cast<uint32_t>(n);
    } else {
      append_fill(n - count, &x, sizeof(T));
    }
  }

  void reserve(size_t n) { VectorBase::reserve(n, sizeof(T)); }

  void assign(size_t n, T const &x) { assign_fill(n, &x, sizeof(T)); }
  void assign(T const *first, T const *last) {
    assign_copy(first, static_cast<size_t>(last - first), sizeof(T));
  }
  void assign(std::initializer_list<T> il) {
    assign_copy(il.begin(), il.size(), sizeof(T));
  }

  T *insert(T const *pos, T const &x) { return insert(pos, &x, &x + 1); }
  T *insert(T const *pos, T const *first, T const *last) {
    return static_cast<T *>(insert_copy(static_cast<size_t>(pos - data()),
                                        first,
                                        static_cast<size_t>(last - first),
                                        sizeof(T)));
  }
  T *insert(T const *pos, std::initializer_list<T> il) {
    return insert(pos, il.begin(), il.end());
  }

  T *erase(T const *pos) { return erase(pos, pos + 1); }
  T *erase(T const *first, T const *last) {
    auto const at{ static_cast<size_t>(first - data()) };
    VectorBase::erase(at, static_cast<size_t>(last - first), sizeof(T));
    return data() + at;
  }

  void swap(Vector &o) noexcept {
    std::swap(ptr, o.ptr);
    std::swap(count, o.count);
    std::swap(cap, o.cap);
  }

  friend bool operator==(Vector const &a, Vector const &b) {
    if (a.count != b.count) { return false; }
    for (size_t i = 0; i < a.count; ++i) {
      if (!(a[i] == b[i])) { return false; }
    }
    return true;
  }
};

}  // namespace scav

#endif  // SCAV_VECTOR_H_INCLUDED
