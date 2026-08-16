// Resolves a document name to a key by byte-wise segment folding.

#include "scav/scav_core.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace scav {

namespace {

constexpr size_t NPOS{ std::string_view::npos };

// A `:` before any `/`. Matches `https:` and also `C:`; a drive letter is not a
// scheme, but treating it as one gives the same answer.
bool has_scheme(std::string_view s) {
  size_t const colon{ s.find(':') };
  if (colon == NPOS) { return false; }
  size_t const slash{ s.find('/') };
  return (slash == NPOS) || (colon < slash);
}

// The leading run of `s` that segment folding may not reach -- a scheme with
// its authority, or a leading `/`. Everything after it is poppable by `..`.
size_t opaque_root(std::string_view s) {
  if (has_scheme(s)) {
    size_t const colon{ s.find(':') };
    if (s.substr(colon, 3) == "://") {
      size_t const after{ s.find('/', colon + 3) };
      return (after == NPOS) ? s.size() : (after + 1);
    }
    // `scheme:path`, and the drive-letter case. The following separator goes
    // with the root, or `C:/proj` would fold to `C:proj`.
    size_t const next{ colon + 1 };
    return ((next < s.size()) && (s[next] == '/')) ? (next + 1) : next;
  }
  return (!s.empty() && (s.front() == '/')) ? 1U : 0U;
}

// Splits `text` on `/` and folds `.` and `..` into `segs`. An unpoppable `..`
// survives while relative and drops under a root, folding `/../x` onto `/x`.
void fold_segments(std::string_view text,
                   bool rooted,
                   std::vector<std::string_view> &segs) {
  size_t start{ 0 };
  while (start <= text.size()) {
    size_t const slash{ text.find('/', start) };
    size_t const end{ (slash == NPOS) ? text.size() : slash };
    std::string_view const seg{ text.substr(start, end - start) };
    if (!seg.empty() && (seg != ".")) {
      bool const up{ seg == ".." };
      if (up && !segs.empty() && (segs.back() != "..")) {
        segs.pop_back();
      } else if (!up || !rooted) {
        segs.push_back(seg);
      }
    }
    if (slash == NPOS) { break; }
    start = slash + 1;
  }
}

}  // namespace

bool path_resolve(std::string_view base, std::string_view ref, std::string &out) {
  out.clear();
  // The last component must name a document; a trailing separator, `.` and
  // `..` all name a directory, and `a/..` would land on the base's own.
  if (ref.empty()) { return false; }
  size_t const cut_ref{ ref.rfind('/') };
  std::string_view const tail{ (cut_ref == NPOS) ? ref : ref.substr(cut_ref + 1) };
  if (tail.empty() || (tail == ".") || (tail == "..")) { return false; }

  // Absolute or scheme-carrying passes through uninterpreted.
  if ((ref.front() == '/') || has_scheme(ref)) {
    out.assign(ref);
    return true;
  }

  size_t const root{ opaque_root(base) };
  std::string_view const rooted{ base.substr(0, root) };
  std::string_view const rest{ base.substr(root) };
  size_t const cut{ rest.rfind('/') };
  std::string_view const dir{ (cut == NPOS) ? std::string_view{}
                                            : rest.substr(0, cut + 1) };

  // The tail check above guarantees `ref` contributes a final named segment, so
  // the result is never empty and never ends in `..`.
  std::vector<std::string_view> segs;
  fold_segments(dir, root != 0, segs);
  fold_segments(ref, root != 0, segs);

  out.assign(rooted);
  // `https://host` with no path takes the whole authority as its root, so a
  // name under it needs a separator. A bare `C:` keeps drive-relative.
  if (!out.empty() && (out.back() != '/') && (out.find("://") != NPOS)) { out += '/'; }
  for (size_t i = 0; i < segs.size(); ++i) {
    if (i != 0) { out += '/'; }
    out += segs[i];
  }
  return true;
}

}  // namespace scav
