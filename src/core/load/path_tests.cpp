// Pins the keys `path_resolve` produces for filesystem, drive-letter and URL paths.

#include "scav/scav_core.h"

#include "doctest.h"

#include <ostream>
#include <string>
#include <string_view>

namespace {

using namespace scav;

// Returns the resolved key, or "<rejected>" when `path_resolve` fails.
std::string resolved(std::string_view base, std::string_view ref) {
  std::string out{ "pre-existing content" };  // assigned, never appended to
  return path_resolve(base, ref, out) ? out : std::string{ "<rejected>" };
}

}  // namespace

TEST_CASE("path: a bare name resolves beside its base") {
  CHECK(resolved("vac.scav", "dock.scav") == "dock.scav");
  CHECK(resolved("charts/vac.scav", "dock.scav") == "charts/dock.scav");
  CHECK(resolved("a/b/c/vac.scav", "dock.scav") == "a/b/c/dock.scav");
  CHECK(resolved("", "dock.scav") == "dock.scav");
}

TEST_CASE("path: a nested name descends from the base's directory") {
  CHECK(resolved("a/vac.scav", "sub/dock.scav") == "a/sub/dock.scav");
  CHECK(resolved("a/vac.scav", "sub/deeper/dock.scav") == "a/sub/deeper/dock.scav");
}

TEST_CASE("path: `.` folds away and `//` collapses") {
  CHECK(resolved("a/b/vac.scav", "./dock.scav") == "a/b/dock.scav");
  CHECK(resolved("a/b/vac.scav", "././dock.scav") == "a/b/dock.scav");
  CHECK(resolved("a/b/vac.scav", "sub//dock.scav") == "a/b/sub/dock.scav");
  CHECK(resolved("./a/vac.scav", "dock.scav") == "a/dock.scav");
  CHECK(resolved("a//b/vac.scav", "dock.scav") == "a/b/dock.scav");
}

TEST_CASE("path: two spellings of one document give one key") {
  CHECK(resolved("x/vac.scav", "./dock.scav") == resolved("x/vac.scav", "dock.scav"));
  CHECK(resolved("x/vac.scav", "sub/../dock.scav") == resolved("x/vac.scav", "dock.scav"));
  CHECK(resolved("x/vac.scav", "./sub/./dock.scav") ==
        resolved("x/vac.scav", "sub/dock.scav"));
}

TEST_CASE("path: `..` pops a segment") {
  CHECK(resolved("a/b/vac.scav", "../dock.scav") == "a/dock.scav");
  CHECK(resolved("a/b/vac.scav", "../../dock.scav") == "dock.scav");
  CHECK(resolved("a/b/vac.scav", "../c/dock.scav") == "a/c/dock.scav");
  CHECK(resolved("a/b/c/vac.scav", "../../shared/dock.scav") == "a/shared/dock.scav");
}

TEST_CASE("path: a `..` above a relative base is kept, not dropped") {
  CHECK(resolved("a/vac.scav", "../../dock.scav") == "../dock.scav");
  CHECK(resolved("a/vac.scav", "../../../dock.scav") == "../../dock.scav");
  CHECK(resolved("vac.scav", "../dock.scav") == "../dock.scav");
  CHECK(resolved("a/vac.scav", "../../dock.scav") !=
        resolved("a/vac.scav", "../../../dock.scav"));
}

TEST_CASE("path: a `..` above an opaque root is dropped") {
  // Nothing lies above `/` or an authority, so `/../x` resolves to `/x`.
  CHECK(resolved("/a/vac.scav", "../../dock.scav") == "/dock.scav");
  CHECK(resolved("/a/vac.scav", "../dock.scav") == "/dock.scav");
  CHECK(resolved("https://h/a/vac.scav", "../../../dock.scav") == "https://h/dock.scav");
}

TEST_CASE("path: an absolute or scheme-carrying ref passes through verbatim") {
  CHECK(resolved("a/vac.scav", "/srv/dock.scav") == "/srv/dock.scav");
  CHECK(resolved("a/vac.scav", "https://h/dock.scav") == "https://h/dock.scav");
  CHECK(resolved("a/vac.scav", "/srv/./x/../dock.scav") == "/srv/./x/../dock.scav");
  CHECK(resolved("https://h/a/v.scav", "file:///x.scav") == "file:///x.scav");
}

TEST_CASE("path: a scheme base keeps its authority and resolves under it") {
  CHECK(resolved("https://h/a/vac.scav", "dock.scav") == "https://h/a/dock.scav");
  CHECK(resolved("https://h/a/vac.scav", "../dock.scav") == "https://h/dock.scav");
  CHECK(resolved("https://h/vac.scav", "sub/dock.scav") == "https://h/sub/dock.scav");
  // An authority with no path still separates from the name under it.
  CHECK(resolved("https://h", "dock.scav") == "https://h/dock.scav");
  CHECK(resolved("https://h/", "dock.scav") == "https://h/dock.scav");
}

TEST_CASE("path: `scheme://x` makes x an authority, not a document") {
  // Ordinary URL semantics: `mem://a` is host `a` with an empty path, so a
  // sibling lands under it. `mem:///a` is the empty-authority spelling.
  CHECK(resolved("mem://a.scav", "b.scav") == "mem://a.scav/b.scav");
  CHECK(resolved("mem:///a.scav", "b.scav") == "mem:///b.scav");
}

TEST_CASE("path: a drive letter is an opaque root, and stays one") {
  // Names are `/`-separated everywhere; converting a native path is the
  // caller's job at its own boundary. A drive prefix still must not be popped.
  CHECK(resolved("C:/proj/vac.scav", "dock.scav") == "C:/proj/dock.scav");
  CHECK(resolved("C:/proj/vac.scav", "../dock.scav") == "C:/dock.scav");
  CHECK(resolved("C:/proj/vac.scav", "../../../dock.scav") == "C:/dock.scav");
  // A drive-relative base resolves with no separator after the drive.
  CHECK(resolved("C:vac.scav", "dock.scav") == "C:dock.scav");
}

TEST_CASE("path: a drive letter needs its separator to resolve under it") {
  // With backslashes the drive prefix is the whole root and a sibling resolves
  // beside the drive; `load_file` converts them on Windows.
  CHECK(resolved("D:\\a\\vac.scav", "dock.scav") == "D:dock.scav");
  CHECK(resolved("D:/a/vac.scav", "dock.scav") == "D:/a/dock.scav");
}

TEST_CASE("path: a backslash is an ordinary byte") {
  // Not a separator, so it survives into the key verbatim.
  CHECK(resolved("a/vac.scav", "sub\\dock.scav") == "a/sub\\dock.scav");
}

TEST_CASE("path: a ref that names no document is rejected") {
  CHECK(resolved("a/vac.scav", "") == "<rejected>");
  CHECK(resolved("a/vac.scav", "/") == "<rejected>");
  CHECK(resolved("a/vac.scav", "sub/") == "<rejected>");
  CHECK(resolved("a/vac.scav", ".") == "<rejected>");
  CHECK(resolved("a/vac.scav", "..") == "<rejected>");
  CHECK(resolved("a/vac.scav", "sub/..") == "<rejected>");
  CHECK(resolved("a/b/vac.scav", "../..") == "<rejected>");
  CHECK(resolved("a/vac.scav", "./") == "<rejected>");
}

TEST_CASE("path: a rejected ref clears out rather than leaving it stale") {
  std::string out{ "stale" };
  CHECK_FALSE(path_resolve("a/vac.scav", "", out));
  CHECK(out.empty());
  CHECK_FALSE(path_resolve("a/vac.scav", "..", out));
  CHECK(out.empty());
}

TEST_CASE("path: resolving a resolved key against itself is a fixed point") {
  // The loader resolves a document's includes against the key it was claimed
  // under.
  std::string const key{ resolved("a/b/vac.scav", "../c/dock.scav") };
  CHECK(key == "a/c/dock.scav");
  CHECK(resolved(key, "led.scav") == "a/c/led.scav");
  CHECK(resolved(key, "../b/vac.scav") == "a/b/vac.scav");
}

TEST_CASE("path: the same inputs give the same bytes every time") {
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(resolved("a/b/vac.scav", "../x/./y/../dock.scav") == "a/x/dock.scav");
  }
}
