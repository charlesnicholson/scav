#include "scav_pod_vector.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace scav {

namespace {

using Byte = unsigned char;

constexpr size_t COUNT_MAX{ UINT32_MAX };

// n elements' bytes; aborts past COUNT_MAX elements or SIZE_MAX bytes.
size_t bytes_of(size_t n, size_t elem) {
  if ((n > COUNT_MAX) || (n > (SIZE_MAX / elem))) { std::abort(); }
  return n * elem;
}

void *checked(void *p) {
  if (p == nullptr) { std::abort(); }
  return p;
}

// True when p points into [base, base + bytes).
bool within(void const *p, void const *base, size_t bytes) {
  auto const at{ reinterpret_cast<uintptr_t>(p) };
  auto const lo{ reinterpret_cast<uintptr_t>(base) };
  return (at >= lo) && (at - lo < bytes);
}

template <typename W>
W load(void const *x) {
  W w{};
  std::memcpy(&w, x, sizeof(W));
  return w;
}

template <typename W>
void fill_words(Byte *d, size_t n, W w) {
  for (size_t i = 0; i < n; ++i) { std::memcpy(d + (i * sizeof(W)), &w, sizeof(W)); }
}

// Writes n copies of the elem bytes at x, or zeros for a null x, to dst; x may lie in dst.
void fill(void *dst, size_t n, void const *x, size_t elem) {
  if (n == 0) { return; }
  auto *const d{ static_cast<Byte *>(dst) };
  if (x == nullptr) {
    std::memset(d, 0, n * elem);
    return;
  }
  switch (elem) {
    case 1: std::memset(d, *static_cast<Byte const *>(x), n); return;
    case 2: fill_words(d, n, load<uint16_t>(x)); return;
    case 4: fill_words(d, n, load<uint32_t>(x)); return;
    case 8: fill_words(d, n, load<uint64_t>(x)); return;
    default: break;
  }
  std::memmove(d, x, elem);
  size_t done{ 1 };
  while (done < n) {
    size_t const k{ (done < (n - done)) ? done : (n - done) };
    std::memcpy(d + (done * elem), d, k * elem);
    done += k;
  }
}

// Capacity for `need` elements when appending: twice the capacity, at least `need`.
size_t grown(size_t cap, size_t need) {
  if (need > COUNT_MAX) { std::abort(); }
  if (cap >= COUNT_MAX / 2) { return COUNT_MAX; }
  return ((2 * cap) > need) ? (2 * cap) : need;
}

}  // namespace

void PodVectorBase::reserve(size_t n, size_t elem) {
  if (n <= cap) { return; }
  ptr = checked(std::realloc(ptr, bytes_of(n, elem)));
  cap = static_cast<uint32_t>(n);
}

void *PodVectorBase::push_grow(void const *x, size_t elem) {
  size_t const used{ size_t{ count } * elem };
  bool const inside{ within(x, ptr, used) };
  size_t const off{ inside ? static_cast<size_t>(static_cast<Byte const *>(x) -
                                                 static_cast<Byte const *>(ptr))
                           : 0 };
  if (count == cap) { reserve(grown(cap, size_t{ count } + 1), elem); }
  void const *const src{ inside ? (static_cast<Byte const *>(ptr) + off) : x };
  void *const dst{ static_cast<Byte *>(ptr) + used };
  std::memcpy(dst, src, elem);
  ++count;
  return dst;
}

void PodVectorBase::append_fill(size_t n, void const *x, size_t elem) {
  if (n == 0) { return; }
  size_t const need{ size_t{ count } + n };
  size_t const used{ size_t{ count } * elem };
  bool const inside{ (x != nullptr) && within(x, ptr, used) };
  size_t const off{ inside ? static_cast<size_t>(static_cast<Byte const *>(x) -
                                                 static_cast<Byte const *>(ptr))
                           : 0 };
  if (need > cap) { reserve(grown(cap, need), elem); }
  void const *const src{ inside ? (static_cast<Byte const *>(ptr) + off) : x };
  fill(static_cast<Byte *>(ptr) + used, n, src, elem);
  count = static_cast<uint32_t>(need);
}

void PodVectorBase::assign_fill(size_t n, void const *x, size_t elem) {
  if (n > cap) {
    void *const fresh{ checked(std::malloc(bytes_of(n, elem))) };
    fill(fresh, n, x, elem);  // x may lie in the storage freed next
    std::free(ptr);
    ptr = fresh;
    cap = static_cast<uint32_t>(n);
  } else {
    fill(ptr, n, x, elem);
  }
  count = static_cast<uint32_t>(n);
}

void PodVectorBase::assign_copy(void const *src, size_t n, size_t elem) {
  if (n > cap) {
    void *const fresh{ checked(std::malloc(bytes_of(n, elem))) };
    std::memcpy(fresh, src, n * elem);
    std::free(ptr);
    ptr = fresh;
    cap = static_cast<uint32_t>(n);
  } else if (n != 0) {
    std::memmove(ptr, src, n * elem);
  }
  count = static_cast<uint32_t>(n);
}

void PodVectorBase::copy_from(PodVectorBase const &o, size_t elem) {
  if (&o != this) { assign_copy(o.ptr, o.count, elem); }
}

void PodVectorBase::move_from(PodVectorBase &o) {
  if (&o == this) { return; }
  if (ptr != nullptr) { std::free(ptr); }
  ptr = o.ptr;
  count = o.count;
  cap = o.cap;
  o.ptr = nullptr;
  o.count = 0;
  o.cap = 0;
}

void *PodVectorBase::insert_copy(size_t at, void const *src, size_t n, size_t elem) {
  if (n == 0) { return static_cast<Byte *>(ptr) + (at * elem); }
  size_t const need{ size_t{ count } + n };
  void *held{ nullptr };
  if (within(src, ptr, size_t{ count } * elem)) {  // a range of this vector
    held = checked(std::malloc(n * elem));
    std::memcpy(held, src, n * elem);
    src = held;
  }
  if (need > cap) { reserve(grown(cap, need), elem); }
  auto *const base{ static_cast<Byte *>(ptr) };
  if (at < count) {
    std::memmove(base + ((at + n) * elem), base + (at * elem), (count - at) * elem);
  }
  std::memcpy(base + (at * elem), src, n * elem);
  if (held != nullptr) { std::free(held); }
  count = static_cast<uint32_t>(need);
  return base + (at * elem);
}

void PodVectorBase::erase(size_t at, size_t n, size_t elem) {
  if (n == 0) { return; }
  auto *const base{ static_cast<Byte *>(ptr) };
  std::memmove(base + (at * elem), base + ((at + n) * elem), (count - at - n) * elem);
  count -= static_cast<uint32_t>(n);
}

void PodVectorBase::release(void *p) { std::free(p); }

}  // namespace scav
