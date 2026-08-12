// Compiled against the library's testable variant, where the internal helper below
// has external linkage.

#include "scav/scav_toy.h"

#include "doctest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

// Declared here rather than in a header: a test reaches an internal function by
// naming the prototype and linking against the testable build.
uint64_t scav_toy_fold(uint64_t acc, scav_byte b);

namespace {

uint64_t checksum_of(std::vector<scav_byte> const &bytes) {
  return scav_toy_checksum(bytes.data(), static_cast<uint32_t>(bytes.size()));
}

std::vector<scav_byte> bytes_of(std::string_view text) {
  std::vector<scav_byte> out;
  out.reserve(text.size());
  for (char const c : text) { out.push_back(static_cast<scav_byte>(c)); }
  return out;
}

// Hand-rolled: locale-sensitive formatting is banned from anything whose bytes
// reach output, and a golden is exactly that.
std::string hex64(uint64_t v) {
  static constexpr std::string_view DIGITS{ "0123456789abcdef" };
  std::string out(16, '0');
  for (uint32_t i = 0; i < 16; ++i) { out[15 - i] = DIGITS[(v >> (4 * i)) & 0xFU]; }
  return out;
}

struct GoldenCase {
  char const *label;
  std::vector<scav_byte> bytes;
};

std::vector<GoldenCase> golden_cases() {
  std::vector<scav_byte> ascending;
  std::vector<scav_byte> descending;
  for (uint32_t i = 0; i < 256; ++i) {
    ascending.push_back(static_cast<scav_byte>(i));
    descending.push_back(static_cast<scav_byte>(255 - i));
  }
  return {
    { "empty", {} },
    { "nul", { 0 } },
    { "a", bytes_of("a") },
    { "foobar", bytes_of("foobar") },
    { "scav", bytes_of("scav") },
    { "all-bytes-ascending", ascending },
    { "all-bytes-descending", descending },
    { "high-bit-run", std::vector<scav_byte>(64, 0xff) },
  };
}

// Binary: a golden is compared byte for byte, so nothing may translate line
// endings on the way in.
std::string read_file(std::filesystem::path const &path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path.string());
  return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

void write_file(std::filesystem::path const &path, std::string const &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

TEST_CASE("toy: checksum matches published FNV-1a vectors") {
  // Published vectors, so a mistake cannot hide behind a golden that merely
  // agrees with itself.
  CHECK(checksum_of({}) == 0xcbf29ce484222325ULL);
  CHECK(checksum_of(bytes_of("a")) == 0xaf63dc4c8601ec8cULL);
  CHECK(checksum_of(bytes_of("foobar")) == 0x85944171f73967e8ULL);
}

TEST_CASE("toy: checksum of an empty range does not read the pointer") {
  CHECK(scav_toy_checksum(nullptr, 0) == 0xcbf29ce484222325ULL);
}

TEST_CASE("toy: checksum is order-sensitive") {
  CHECK(checksum_of(bytes_of("ab")) != checksum_of(bytes_of("ba")));
}

TEST_CASE("toy: checksum distinguishes an embedded NUL from a short string") {
  std::vector<scav_byte> const with_nul{ 'a', 0, 'b' };
  CHECK(checksum_of(with_nul) != checksum_of(bytes_of("ab")));
}

// This test existing at all is the point: it proves an internal function is
// reachable without inventing a seam.
TEST_CASE("toy: fold is one FNV-1a step") {
  constexpr uint64_t BASIS{ 0xcbf29ce484222325ULL };
  CHECK(scav_toy_fold(BASIS, 'a') == 0xaf63dc4c8601ec8cULL);
}

TEST_CASE("toy: checksum is fold applied left to right") {
  uint64_t acc{ 0xcbf29ce484222325ULL };
  for (scav_byte const b : bytes_of("scav")) { acc = scav_toy_fold(acc, b); }
  CHECK(acc == checksum_of(bytes_of("scav")));
}

TEST_CASE("toy: fold mixes the whole byte") {
  // Differ only in the high bit; both must move the accumulator.
  CHECK(scav_toy_fold(0, 0x00) != scav_toy_fold(0, 0x80));
}

TEST_CASE("toy: golden checksum table") {
  // On a mismatch the produced bytes land under out/, so the diff is one `diff`
  // away rather than a hex dump in the test log.
  std::string produced;
  for (GoldenCase const &c : golden_cases()) {
    produced += c.label;
    produced += '\t';
    produced += hex64(checksum_of(c.bytes));
    produced += '\n';
  }

  std::filesystem::path const golden{ std::filesystem::path{ SCAV_TEST_DATA_DIR } /
                                      "golden" / "toy" / "checksum.txt" };
  std::string const expected{ read_file(golden) };

  if (produced != expected) {
    std::filesystem::path const actual{ std::filesystem::path{ SCAV_TEST_OUT_DIR } /
                                        "golden" / "toy" / "checksum.txt" };
    write_file(actual, produced);
    FAIL("golden mismatch: " << golden.string() << " vs " << actual.string());
  }
  CHECK(produced == expected);
}

// Fails on purpose, and is skipped unless run by name. A harness that reports
// nothing looks exactly like one where everything passes.
TEST_CASE("toy: deliberate failure" * doctest::skip()) {
  CHECK_MESSAGE(scav_toy_checksum(nullptr, 0) == 0, "this failure is intentional");
}
