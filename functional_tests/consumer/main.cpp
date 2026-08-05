// Compiled with the consumer's own flags -- exceptions and RTTI on -- against an
// archive built without either. The answer must be the same.

#include <scav/toy.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

int main() {
  char const *const text{ "scav" };
  auto const *const bytes{ reinterpret_cast<scav_byte const *>(text) };
  auto const len{ static_cast<uint32_t>(std::strlen(text)) };

  // Pinned, so a consumer that links but computes something else fails rather
  // than passing silently.
  constexpr uint64_t EXPECTED{ 0xf75ced18b5176da0ULL };

  uint64_t const actual{ scav_toy_checksum(bytes, len) };
  if (actual != EXPECTED) {
    std::fprintf(stderr,
                 "scav_toy_checksum(\"scav\") = %016llx, expected %016llx\n",
                 static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(EXPECTED));
    return 1;
  }

  std::printf("consumer ok: %016llx\n", static_cast<unsigned long long>(actual));
  return 0;
}
