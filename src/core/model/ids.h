#ifndef SCAV_CORE_MODEL_IDS_H_INCLUDED
#define SCAV_CORE_MODEL_IDS_H_INCLUDED

// Strong id types and the two span shapes every column indexes through. P0 owns
// only the front-end slice of PRD 7 -- documents, statements and the string
// pool -- so the entity ids land with the model spine in P1.

#include <cstdint>

namespace scav {

// PRD 7 spells this `kInvalid`; the pinned naming rules make a constant
// UPPER_CASE, and a rule that is mechanically enforced wins over one that is
// not.
constexpr uint32_t INVALID{ 0xFFFF'FFFFU };

struct DocId {
  uint32_t v;
};
struct StmtId {
  uint32_t v;
};

constexpr bool operator==(DocId a, DocId b) { return a.v == b.v; }
constexpr bool operator!=(DocId a, DocId b) { return a.v != b.v; }
constexpr bool operator==(StmtId a, StmtId b) { return a.v == b.v; }
constexpr bool operator!=(StmtId a, StmtId b) { return a.v != b.v; }

// Into StringPool::bytes.
struct StrRef {
  uint32_t off, len;
};

// Into a side array.
struct Span {
  uint32_t off, len;
};

constexpr bool operator==(StrRef a, StrRef b) {
  return (a.off == b.off) && (a.len == b.len);
}
constexpr bool operator!=(StrRef a, StrRef b) { return !(a == b); }
constexpr bool operator==(Span a, Span b) { return (a.off == b.off) && (a.len == b.len); }
constexpr bool operator!=(Span a, Span b) { return !(a == b); }

// Named constructors, because these two are built more than anything else in the
// front end and a designated-initializer list at every site reads worse than the
// call does. An empty span is `{}`.
constexpr StrRef str_ref(uint32_t off, uint32_t len) { return { .off = off, .len = len }; }
constexpr Span make_span(uint32_t off, uint32_t len) { return { .off = off, .len = len }; }

}  // namespace scav

#endif  // SCAV_CORE_MODEL_IDS_H_INCLUDED
