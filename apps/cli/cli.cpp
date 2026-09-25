// Output streams and exit codes, which are a process's to choose. Everything
// below them -- loading, validating, rendering a diagnostic -- is core's.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace cli {

void write_stream(std::string const &text, std::FILE *to) {
  std::ignore = std::fwrite(text.data(), 1, text.size(), to);
}

void write_error(std::string_view what, std::string_view path) {
  std::string err{ "scav: " };
  err += what;
  err += " '";
  err += path;
  err += "'\n";
  write_stream(err, stderr);
}

namespace {

// Two ordinals separated by a colon, the whole of `text`.
bool ordinal_pair(std::string_view arg, uint32_t &a, uint32_t &b) {
  size_t const colon{ arg.find(':') };
  if ((colon == std::string_view::npos) || (colon == 0) || (colon + 1 == arg.size())) {
    return false;
  }
  std::from_chars_result const x{ std::from_chars(arg.data(), arg.data() + colon, a) };
  std::from_chars_result const y{
    std::from_chars(arg.data() + colon + 1, arg.data() + arg.size(), b)
  };
  return (x.ec == std::errc{}) && (x.ptr == (arg.data() + colon)) &&
         (y.ec == std::errc{}) && (y.ptr == (arg.data() + arg.size()));
}

bool face_fields(std::string_view arg, std::array<uint32_t, 4> &field) {
  for (uint32_t i = 0; i < field.size(); ++i) {
    size_t const at{ arg.find(':') };
    std::string_view const head{ (i + 1 == field.size()) ? arg : arg.substr(0, at) };
    if (head.empty() || ((i + 1 < field.size()) && (at == std::string_view::npos))) {
      return false;
    }
    std::from_chars_result const got{
      std::from_chars(head.data(), head.data() + head.size(), field[i])
    };
    if ((got.ec != std::errc{}) || (got.ptr != (head.data() + head.size()))) {
      return false;
    }
    if (i + 1 < field.size()) { arg.remove_prefix(at + 1); }
  }
  return (field[2] <= 1) && (field[3] <= 3);
}

// `value` is the flag's argument, parsed into `out`; false if it is malformed.
bool read_value(std::string_view flag, std::string_view value, LayoutArgs &out) {
  uint32_t a{ 0 };
  uint32_t b{ 0 };
  if (flag == "--portfolio-row") {
    std::from_chars_result const got{
      std::from_chars(value.data(), value.data() + value.size(), a)
    };
    if ((got.ec != std::errc{}) || (got.ptr != (value.data() + value.size())) ||
        (a >= LAYOUT_SEARCH_ROWS) || (out.row != INVALID)) {
      return false;
    }
    out.row = a;
    return true;
  }
  if (flag == "--face") {
    std::array<uint32_t, 4> field{};
    if (!face_fields(value, field)) { return false; }
    out.pins.faces.push_back({ .trans = TransId{ field[0] },
                               .leg = field[1],
                               .end = field[2],
                               .face = field[3] });
    return true;
  }
  if (!ordinal_pair(value, a, b)) { return false; }
  if (flag == "--rank") {
    out.pins.ranks.push_back({ .state = StateId{ a }, .rank = b });
  } else if (flag == "--cut") {
    out.pins.cuts.push_back({ .trans = TransId{ a }, .leg = b });
  } else {
    out.pins.reverses.push_back({ .trans = TransId{ a }, .leg = b });
  }
  return true;
}

}  // namespace

ArgRead read_layout_arg(int argc, char **argv, int &i, LayoutArgs &out) {
  std::string_view const arg{ argv[i] };
  if (arg == "--no-search") {
    if (out.no_search) { return ArgRead::Malformed; }
    out.no_search = true;
    out.given = true;
    return ArgRead::Taken;
  }
  if ((arg != "--portfolio-row") && (arg != "--rank") && (arg != "--cut") &&
      (arg != "--reverse") && (arg != "--face")) {
    return ArgRead::NotOurs;
  }
  // The increment is its own statement: clang-tidy's
  // bugprone-inc-dec-in-conditions is right that `++i` inside a compound
  // condition depends on an evaluation order a reader has to reconstruct.
  if ((i + 1) >= argc) { return ArgRead::Malformed; }
  ++i;
  out.given = true;
  return read_value(arg, argv[i], out) ? ArgRead::Taken : ArgRead::Malformed;
}

void append_layout_args(std::string &out, uint32_t row, SearchPins const &pins) {
  auto const pair = [&out](char const *flag, uint32_t a, uint32_t b) {
    out += ' ';
    out += flag;
    out += ' ';
    string_append_u32(out, a);
    out += ':';
    string_append_u32(out, b);
  };
  out += "--portfolio-row ";
  string_append_u32(out, row);
  for (RankPin const &r : pins.ranks) { pair("--rank", r.state.v, r.rank); }
  for (ChainCut const &k : pins.cuts) { pair("--cut", k.trans.v, k.leg); }
  for (ReversePin const &r : pins.reverses) { pair("--reverse", r.trans.v, r.leg); }
  for (FacePin const &f : pins.faces) {
    pair("--face", f.trans.v, f.leg);
    out += ':';
    string_append_u32(out, f.end);
    out += ':';
    string_append_u32(out, f.face);
  }
}

void load_and_report(char const *path, bool validate, Loaded &out) {
  std::string failed;
  bool const loaded{ load_file(path, out.loader, out.chart, out.diags, failed) };
  if (!failed.empty()) {
    write_error("cannot read", failed);
    out.code = EXIT_UNUSABLE;
    return;
  }

  std::string err;
  // A load that never reached a chart leaves nothing to print, and its findings
  // index the loader's buffers rather than a chart's.
  if (out.chart.documents.empty()) {
    for (Diagnostic const &d : out.diags) { diag_append(err, out.loader, d, path); }
    write_stream(err, stderr);
    out.code = EXIT_UNUSABLE;
    return;
  }

  bool clean{ loaded };
  if (validate) { clean = validate_chart(out.chart, out.diags) && clean; }
  for (Diagnostic const &d : out.diags) { diag_append(err, out.chart, d, path); }
  write_stream(err, stderr);
  out.code = clean ? EXIT_CLEAN : EXIT_DIAGNOSED;
}

}  // namespace cli
