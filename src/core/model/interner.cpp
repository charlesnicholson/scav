#include "core/model/interner.h"

#include "core/model/bytes_view.h"
#include "core/model/sort.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

StrRef intern_bytes(Interner &in, scav_byte const *bytes, size_t len) {
  if (len == 0) { return {}; }

  std::string_view const probe{ bytes_view(bytes, len) };
  if (uint32_t const *const found = in.index.find(probe); found != nullptr) {
    return in.unique[*found];
  }

  // The pool is bounded by the document, which the parser already checked fits
  // a span, so both of these round-trip by construction.
  StrRef const ref{ str_ref(narrow_clamp<uint32_t>(in.staging.size()),
                            narrow_clamp<uint32_t>(len)) };
  in.staging.insert(in.staging.end(), bytes, bytes + len);
  in.index.insert(std::string{ probe }, narrow_clamp<uint32_t>(in.unique.size()));
  in.unique.push_back(ref);
  return ref;
}

StrRef intern_bytes(Interner &in, std::string_view text) {
  return intern_bytes(in, reinterpret_cast<scav_byte const *>(text.data()), text.size());
}

void intern_finalize(Interner const &in, StringPool &pool, StringRemap &out) {
  pool.bytes.clear();
  out.staging.clear();
  out.finalized.clear();

  uint32_t const n{ narrow_clamp<uint32_t>(in.unique.size()) };
  if (n == 0) { return; }

  std::vector<uint32_t> order(n);
  for (uint32_t i = 0; i < n; ++i) { order[i] = i; }

  scav_byte const *const staged{ in.staging.data() };
  std::vector<StrRef> const &unique{ in.unique };
  // Every entry is distinct, so byte order alone is already a total order and no
  // input-derived tie-break is needed. Stable anyway, per PRD 6.
  stable_sort_by(order, [staged, &unique](uint32_t a, uint32_t b) {
    return string_compare_bytes(staged + unique[a].off,
                                unique[a].len,
                                staged + unique[b].off,
                                unique[b].len) < 0;
  });

  pool.bytes.reserve(in.staging.size());
  // Sized rather than reserved: the remap is filled by staging index, not in
  // sorted order, so `finalized` is written out of sequence.
  out.staging.resize(n);
  out.finalized.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t const which{ order[i] };
    StrRef const ref{ unique[which] };
    out.staging[which] = ref.off;
    out.finalized[which] = narrow_clamp<uint32_t>(pool.bytes.size());
    pool.bytes.insert(pool.bytes.end(), staged + ref.off, staged + ref.off + ref.len);
  }
}

StrRef intern_remap(StringRemap const &r, StrRef ref) {
  if (ref.len == 0) { return {}; }

  // `staging` ascends with the index, because interning appends.
  uint32_t lo{ 0 };
  uint32_t hi{ narrow_clamp<uint32_t>(r.staging.size()) };
  while (lo < hi) {
    uint32_t const mid{ lo + ((hi - lo) / 2) };
    if (r.staging[mid] < ref.off) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  // An unknown offset means the caller mixed refs from two interners, which is a
  // programming error rather than input the format can produce. Degrading to the
  // empty string keeps it from silently reading another string's bytes.
  if ((lo >= r.staging.size()) || (r.staging[lo] != ref.off)) { return {}; }
  return str_ref(r.finalized[lo], ref.len);
}

std::string_view intern_staged_view(Interner const &in, StrRef ref) {
  if (ref.len == 0) { return {}; }
  return bytes_view(in.staging.data() + ref.off, ref.len);
}

}  // namespace scav
