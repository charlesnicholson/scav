// The filesystem transport: the loader primitives with `fopen` in the fetch
// slot. Reads in chunks, which works on a pipe and avoids `ftell`'s `long`.

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

namespace scav {

namespace {

constexpr size_t READ_CHUNK{ size_t{ 64 } * 1024U };

// Document names are `/`-separated on every transport, so the one entry point
// taking a native path converts it. A backslash is a legal filename byte off
// Windows, where this would corrupt a name rather than fix it.
std::string native_to_key(char const *path) {
  std::string out{ (path == nullptr) ? "" : path };
#ifdef _WIN32
  for (char &ch : out) {
    if (ch == '\\') { ch = '/'; }
  }
#endif
  return out;
}

}  // namespace

bool read_file(char const *path, std::vector<scav_byte> &out) {
  out.clear();
  if (path == nullptr) { return false; }
  std::FILE *const file{ std::fopen(path, "rb") };
  if (file == nullptr) { return false; }

  // ferror is checked at each read rather than after the loop: a stream's
  // position is indeterminate once an operation has failed, so nothing may
  // touch it again.
  std::array<scav_byte, READ_CHUNK> buffer{};
  bool ok{ true };
  for (;;) {
    size_t const got{ std::fread(buffer.data(), 1, buffer.size(), file) };
    out.insert(out.end(), buffer.data(), buffer.data() + got);
    if (std::ferror(file) != 0) {
      ok = false;
      break;
    }
    if (got < buffer.size()) { break; }
  }
  std::ignore = std::fclose(file);
  if (!ok) { out.clear(); }
  return ok;
}

bool load_file(char const *path,
               Loader &loader,
               Chart &out,
               std::vector<Diagnostic> &diags,
               std::string &failed_path) {
  failed_path.clear();
  std::vector<scav_byte> bytes;

  if (!read_file(path, bytes)) {
    failed_path.assign((path == nullptr) ? "" : path);
    return false;
  }
  std::string const root{ native_to_key(path) };
  if (!load_add(loader, bytes.data(), bytes.size(), root)) {
    return load_finish(loader, out, diags);
  }

  // The pending view dies on the next add, so each round copies out first.
  // Every round marks a document arrived or returns, bounding the loop.
  std::vector<std::string> wanted;
  for (;;) {
    wanted.clear();
    for (Pending const &p : load_pending(loader)) {
      wanted.emplace_back(load_pending_path(loader, p));
    }
    if (wanted.empty()) { break; }

    for (std::string const &want : wanted) {
      if (!read_file(want.c_str(), bytes)) {
        failed_path = want;
        return false;
      }
      if (!load_add(loader, bytes.data(), bytes.size(), want)) {
        return load_finish(loader, out, diags);
      }
    }
  }
  return load_finish(loader, out, diags);
}

}  // namespace scav
