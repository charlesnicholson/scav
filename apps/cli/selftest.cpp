// `scav selftest`: lay the embedded corpus out on this toolchain, at every
// thread count in the matrix, and diff the three hashes against the committed
// goldens.

#include "cli.h"
#include "selftest_corpus.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav/scav_layout_c.h"
#include "scav/scav_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cli {

namespace {

constexpr std::array<uint32_t, 7> THREAD_COUNTS{ 1, 2, 3, 5, 8, 13, 16 };
constexpr std::array<std::string_view, 3> COLUMNS{ "inputs", "structural", "coordinate" };

// One run's hashes, in the golden's column order.
using Hashes = std::array<uint32_t, COLUMNS.size()>;

// The golden ================================================================

bool parse_hex32(std::string_view text, uint32_t &out) {
  if (text.size() != 8) { return false; }
  out = 0;
  for (char const ch : text) {
    uint32_t digit{ 0 };
    if ((ch >= '0') && (ch <= '9')) {
      digit = static_cast<uint32_t>(ch - '0');
    } else if ((ch >= 'a') && (ch <= 'f')) {
      digit = static_cast<uint32_t>(ch - 'a') + 10U;
    } else {
      return false;
    }
    out = (out << 4U) | digit;
  }
  return true;
}

// Returns the field count; only the first five are written.
uint32_t split_fields(std::string_view line, std::array<std::string_view, 5> &out) {
  uint32_t n{ 0 };
  size_t at{ 0 };
  while (at < line.size()) {
    if (line[at] == ' ') {
      ++at;
      continue;
    }
    size_t const found{ line.find(' ', at) };
    size_t const stop{ (found == std::string_view::npos) ? line.size() : found };
    if (n < out.size()) { out[n] = line.substr(at, stop - at); }
    ++n;
    at = stop;
  }
  return n;
}

// The corpus ================================================================

CorpusChart const *find_chart(std::vector<CorpusChart> const &table,
                              std::string_view name) {
  for (CorpusChart const &chart : table) {
    if (chart.name == name) { return &chart; }
  }
  return nullptr;
}

enum class Load : uint32_t { Ok, Diagnosed, Unsatisfiable };

// The root is added under its bare filename, so every path resolved against it
// is bare too and names a table key exactly.
Load load_embedded(std::vector<CorpusChart> const &table,
                   CorpusChart const &root,
                   Loader &loader,
                   Chart &chart,
                   std::vector<Diagnostic> &diags,
                   std::string &missing) {
  bool adding{ load_add(loader, root.bytes, root.len, root.name) };
  // The pending view dies on the next add, so each round copies out first.
  std::vector<std::string> wanted;
  while (adding) {
    wanted.clear();
    for (Pending const &p : load_pending(loader)) {
      wanted.emplace_back(load_pending_path(loader, p));
    }
    if (wanted.empty()) { break; }
    for (std::string const &want : wanted) {
      CorpusChart const *const doc{ find_chart(table, want) };
      if (doc == nullptr) {
        missing = want;
        return Load::Unsatisfiable;
      }
      adding = load_add(loader, doc->bytes, doc->len, want);
      if (!adding) { break; }
    }
  }
  return load_finish(loader, chart, diags) ? Load::Ok : Load::Diagnosed;
}

// Findings index the loader's buffers until a chart exists to index instead.
void append_load_diags(std::string &out,
                       Loader const &loader,
                       Chart const &chart,
                       std::vector<Diagnostic> const &diags,
                       std::string_view name) {
  for (Diagnostic const &d : diags) {
    if (chart.documents.empty()) {
      diag_append(out, loader, d, name);
    } else {
      diag_append(out, chart, d, name);
    }
  }
}

// A fresh copy per run, so no thread count reads geometry columns another
// wrote: layout_run rewrites them in place.
bool layout_hashes(Chart const &chart,
                   scav_layout_opts const &opts,
                   std::string_view name,
                   Hashes &out,
                   std::string &why) {
  Chart run{ chart };
  std::vector<scav_placed> placed;
  std::vector<Diagnostic> diags;
  if (!layout_run(run, scav_spaces{}, opts, placed, diags)) {
    for (Diagnostic const &d : diags) { diag_append(why, run, d, name); }
    return false;
  }
  out = { layout_inputs_digest(run),
          layout_structural_hash(run),
          layout_coordinate_hash(run) };
  return true;
}

// Output ====================================================================

// Diagnostics arrive one per line; a chart's verdict is one line.
void append_flat(std::string &out, std::string_view text) {
  while (!text.empty() && (text.back() == '\n')) { text.remove_suffix(1); }
  for (char const ch : text) {
    if (ch == '\n') {
      out += "; ";
    } else {
      out += ch;
    }
  }
}

// One column of a FAIL line: what this run produced, then what it was diffed
// against.
void append_column(std::string &out,
                   uint32_t column,
                   uint32_t got,
                   std::string_view source,
                   uint32_t want) {
  out += ' ';
  out += COLUMNS[column];
  out += ' ';
  string_append_hex32(out, got);
  out += " (";
  out += source;
  out += ' ';
  string_append_hex32(out, want);
  out += ')';
}

void append_hashes(std::string &out, Hashes const &h) {
  for (uint32_t const value : h) {
    out += ' ';
    string_append_hex32(out, value);
  }
}

void report(std::string &line) {
  line += '\n';
  write_stream(line, stdout);
}

std::string fail_line(std::string_view name) {
  std::string out{ "FAIL " };
  out += name;
  return out;
}

}  // namespace

int run_selftest(char const *against_path) {
  std::vector<scav_byte> bytes;
  std::string against;
  std::string_view golden{ corpus_golden() };
  if (against_path != nullptr) {
    if (!read_file(against_path, bytes)) {
      write_error("cannot read", against_path);
      return EXIT_UNUSABLE;
    }
    against.assign(bytes.begin(), bytes.end());
    golden = against;
  }

  scav_layout_opts opts{};
  if (!profile_named("readable", opts.profile)) {
    write_error("no such profile", "readable");
    return EXIT_UNUSABLE;
  }
  std::vector<CorpusChart> const table{ corpus_charts() };

  uint32_t charts{ 0 };
  uint32_t failures{ 0 };
  uint32_t line_number{ 0 };
  size_t at{ 0 };
  while (at < golden.size()) {
    size_t const found{ golden.find('\n', at) };
    size_t const stop{ (found == std::string_view::npos) ? golden.size() : found };
    std::string_view line{ golden.substr(at, stop - at) };
    at = stop + 1;
    ++line_number;
    if (!line.empty() && (line.back() == '\r')) { line.remove_suffix(1); }
    if (line.empty()) { continue; }

    std::array<std::string_view, 5> field{};
    Hashes want{};
    bool shaped{ split_fields(line, field) == 4 };
    for (uint32_t i = 0; shaped && (i < COLUMNS.size()); ++i) {
      shaped = parse_hex32(field[i + 1], want[i]);
    }
    if (!shaped) {
      std::string bad{ "FAIL golden:" };
      string_append_u32(bad, line_number);
      bad += " malformed, want: name inputs structural coordinate";
      report(bad);
      ++failures;
      continue;
    }

    ++charts;
    std::string_view const name{ field[0] };
    CorpusChart const *const root{ find_chart(table, name) };
    if (root == nullptr) {
      std::string bad{ fail_line(name) };
      bad += " no such chart in this build";
      report(bad);
      ++failures;
      continue;
    }

    Loader loader;
    Chart chart;
    std::vector<Diagnostic> diags;
    std::string missing;
    Load const status{ load_embedded(table, *root, loader, chart, diags, missing) };
    if (status == Load::Unsatisfiable) {
      write_error("no embedded document", missing);
      return EXIT_UNUSABLE;
    }
    std::string why;
    if (status == Load::Diagnosed) {
      append_load_diags(why, loader, chart, diags, name);
      std::string bad{ fail_line(name) };
      bad += ' ';
      append_flat(bad, why);
      report(bad);
      ++failures;
      continue;
    }

    std::array<Hashes, THREAD_COUNTS.size()> runs{};
    bool laid{ true };
    for (uint32_t k = 0; laid && (k < THREAD_COUNTS.size()); ++k) {
      opts.threads = THREAD_COUNTS[k];
      laid = layout_hashes(chart, opts, name, runs[k], why);
    }
    if (!laid) {
      std::string bad{ fail_line(name) };
      bad += ' ';
      append_flat(bad, why);
      report(bad);
      ++failures;
      continue;
    }

    bool failed{ false };
    if (runs[0] != want) {
      std::string bad{ fail_line(name) };
      for (uint32_t i = 0; i < COLUMNS.size(); ++i) {
        append_column(bad, i, runs[0][i], "golden", want[i]);
      }
      report(bad);
      failed = true;
      ++failures;
    }
    for (uint32_t k = 1; k < THREAD_COUNTS.size(); ++k) {
      if (runs[k] == runs[0]) { continue; }
      std::string bad{ fail_line(name) };
      bad += " threads=";
      string_append_u32(bad, THREAD_COUNTS[k]);
      for (uint32_t i = 0; i < COLUMNS.size(); ++i) {
        if (runs[k][i] == runs[0][i]) { continue; }
        append_column(bad, i, runs[k][i], "threads=1", runs[0][i]);
      }
      report(bad);
      failed = true;
      ++failures;
    }
    if (failed) { continue; }

    // Padded to `FAIL`, so the hash columns line up down the whole report.
    std::string good{ "ok   " };
    good += name;
    append_hashes(good, runs[0]);
    report(good);
  }

  std::string summary{ "selftest: " };
  string_append_u32(summary, charts);
  summary += " charts, ";
  string_append_u32(summary, static_cast<uint32_t>(THREAD_COUNTS.size()));
  summary += " thread counts, ";
  string_append_u32(summary, failures);
  summary += " failures";
  report(summary);
  return (failures == 0) ? EXIT_CLEAN : EXIT_DIAGNOSED;
}

}  // namespace cli
