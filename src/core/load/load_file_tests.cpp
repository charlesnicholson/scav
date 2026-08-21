// The filesystem transport, which is the one part of core that opens a file.
// Everything writes under the build tree, so a run leaves nothing behind it.

#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

std::string scratch(std::string_view name) {
  return std::string{ SCAV_TEST_OUT_DIR } + "/core_load_file_" + std::string{ name };
}

bool put(std::string const &path, std::string_view text) {
  return write_file(path.c_str(), raw(text), text.size());
}

std::string get(std::string const &path) {
  std::vector<scav_byte> bytes;
  if (!read_file(path.c_str(), bytes)) { return "<unreadable>"; }
  return { reinterpret_cast<char const *>(bytes.data()), bytes.size() };
}

}  // namespace

TEST_CASE("load_file: bytes written come back exactly") {
  std::string const path{ scratch("roundtrip.txt") };
  std::string_view const text{ "chart c {\n  state A,\n}\n" };
  REQUIRE(put(path, text));
  CHECK(get(path) == text);
}

TEST_CASE("load_file: a write replaces rather than appends") {
  std::string const path{ scratch("replace.txt") };
  REQUIRE(put(path, "the longer original contents"));
  REQUIRE(put(path, "short"));
  CHECK(get(path) == "short");
}

TEST_CASE("load_file: an empty write leaves an empty file, not an absent one") {
  std::string const path{ scratch("empty.txt") };
  REQUIRE(put(path, "something"));
  REQUIRE(write_file(path.c_str(), nullptr, 0));
  CHECK(get(path).empty());
}

TEST_CASE("load_file: bytes with no text meaning survive both directions") {
  std::string const path{ scratch("binary.bin") };
  std::string text;
  for (uint32_t i = 0; i < 256; ++i) { text += static_cast<char>(i); }
  REQUIRE(put(path, text));
  CHECK(get(path) == text);
}

TEST_CASE("load_file: a path that cannot be opened is false, not a crash") {
  std::vector<scav_byte> bytes{ 1, 2, 3 };
  CHECK_FALSE(read_file(nullptr, bytes));
  CHECK(bytes.empty());  // cleared even on the failing path
  CHECK_FALSE(read_file(scratch("no_such_file.txt").c_str(), bytes));

  CHECK_FALSE(write_file(nullptr, raw("x"), 1));
  CHECK_FALSE(write_file(scratch("no_such_dir/x.txt").c_str(), raw("x"), 1));
  // A null buffer with a non-zero length is a caller error, not an empty write.
  CHECK_FALSE(write_file(scratch("nullbuf.txt").c_str(), nullptr, 4));
}
