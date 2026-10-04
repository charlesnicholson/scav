// Decodes each `raw|gzip <TAB> cap <TAB> input <TAB> output` manifest line into a
// `cap`-byte buffer, writes it to output, and prints `status bytes` to stdout.

#include "scav_inflate.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> fields(std::string const &line) {
  std::vector<std::string> out(1);
  for (char const c : line) {
    if (c == '\t') {
      out.emplace_back();
    } else {
      out.back() += c;
    }
  }
  return out;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    (void)std::fputs("usage: scav_inflate_driver <manifest>\n", stderr);
    return 2;
  }
  std::ifstream manifest{ argv[1] };
  std::string line;
  while (std::getline(manifest, line)) {
    std::vector<std::string> const f{ fields(line) };
    std::vector<scav_byte> in;
    if ((f.size() != 4U) || !scav::read_file(f[2].c_str(), in)) {
      (void)std::fprintf(stderr, "bad manifest line: %s\n", line.c_str());
      return 2;
    }
    auto const cap{ static_cast<uint32_t>(std::stoul(f[1])) };
    std::vector<scav_byte> out(cap);
    uint32_t n{ 0 };
    auto const len{ static_cast<uint32_t>(in.size()) };
    scav::InflateStatus const st{
      (f[0] == "gzip") ? scav::gunzip(in.data(), len, out.data(), cap, n)
                       : scav::inflate(in.data(), len, out.data(), cap, n)
    };
    std::ofstream written{ f[3], std::ios::binary };
    written.write(reinterpret_cast<char const *>(out.data()), n);
    std::printf("%u %u\n", static_cast<uint32_t>(st), n);
  }
  return 0;
}
