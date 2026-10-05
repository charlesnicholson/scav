// `scav fmt`: rewrites each file in canonical form, or reports it under `--check`.
// One file at a time; includes are not followed.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cli {

namespace {

// Appends parse diagnostics as `path:line:col: message`; spans index `bytes`.
void report(std::string &err,
            char const *path,
            std::vector<scav_byte> const &bytes,
            std::vector<Diagnostic> const &diags) {
  for (Diagnostic const &d : diags) {
    err += path;
    if ((d.src.len != 0) &&
        ((static_cast<size_t>(d.src.off) + d.src.len) <= bytes.size())) {
      LineCol const lc{ diag_line_col(bytes.data(), bytes.size(), d.src.off) };
      err += ':';
      string_append_u32(err, lc.line);
      err += ':';
      string_append_u32(err, lc.column);
    }
    err += ": ";
    err += diag_message(d.code);
    err += '\n';
  }
}

}  // namespace

int run_fmt(std::vector<char const *> const &paths, bool check_only) {
  int worst{ EXIT_CLEAN };
  for (char const *const path : paths) {
    std::vector<scav_byte> bytes;
    if (!read_file(path, bytes)) {
      write_error("cannot read", path);
      worst = EXIT_UNUSABLE;
      continue;
    }

    ParsedDocument pd;
    std::vector<Diagnostic> diags;
    bool const ok{
      parse_document(bytes.data(), bytes.size(), path, parse_default_options(), pd, diags)
    };
    std::string err;
    report(err, path, pd.src_bytes, diags);
    write_stream(err, stderr);
    if (!ok) {
      // A file that fails to parse is left unchanged.
      worst = EXIT_UNUSABLE;
      continue;
    }

    std::string canonical;
    if (!print_document(pd, print_default_options(), canonical)) {
      write_stream("scav: the print column budget is out of range\n", stderr);
      worst = EXIT_UNUSABLE;
      continue;
    }

    // Compares the raw bytes on disk, before normalization, with the canonical text.
    std::string_view const before{ reinterpret_cast<char const *>(bytes.data()),
                                   bytes.size() };
    if (before == canonical) { continue; }

    if (check_only) {
      std::string line{ path };
      line += ": not canonical\n";
      write_stream(line, stderr);
      if (worst == EXIT_CLEAN) { worst = EXIT_DIAGNOSED; }
      continue;
    }
    if (!write_file(path,
                    reinterpret_cast<scav_byte const *>(canonical.data()),
                    canonical.size())) {
      write_error("cannot write", path);
      worst = EXIT_UNUSABLE;
    }
  }
  return worst;
}

}  // namespace cli
