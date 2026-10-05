// Filesystem transport: runs the loader with `fopen` reads in 64 KiB chunks.

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

// Converts a native path to a `/`-separated key; on Windows, backslashes become `/`.
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

  // Reads until `ferror` or `feof` is set; a read error clears `out`.
  std::array<scav_byte, READ_CHUNK> buffer{};
  bool ok{ true };
  for (;;) {
    size_t const got{ std::fread(buffer.data(), 1, buffer.size(), file) };
    out.insert(out.end(), buffer.data(), buffer.data() + got);
    if (std::ferror(file) != 0) {
      ok = false;
      break;
    }
    if (std::feof(file) != 0) { break; }
  }
  std::ignore = std::fclose(file);
  if (!ok) { out.clear(); }
  return ok;
}

bool write_file(char const *path, scav_byte const *bytes, size_t len) {
  if ((path == nullptr) || ((bytes == nullptr) && (len != 0))) { return false; }
  std::FILE *const file{ std::fopen(path, "wb") };
  if (file == nullptr) { return false; }
  bool const wrote{ (len == 0) || (std::fwrite(bytes, 1, len, file) == len) };
  // Always closes; false when the write or the close fails.
  return (std::fclose(file) == 0) && wrote;
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

  // Copies the pending names each round; `load_add` invalidates the pending view.
  // Each round loads every pending document or returns.
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
