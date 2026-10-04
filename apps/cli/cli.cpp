// Code the CLI verbs share: output streams, layout flag parsing, and the load prologue.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <algorithm>
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

// True when all of `arg` is two colon-separated ordinals, read into `a` and `b`.
bool ordinal_pair(std::string_view arg, uint32_t &a, uint32_t &b) {
  size_t const colon{ arg.find(':') };
  if ((colon == std::string_view::npos) || (colon == 0) || (colon + 1 == arg.size())) {
    return false;
  }
  std::string_view const head{ arg.substr(0, colon) };
  std::string_view const tail{ arg.substr(colon + 1) };
  std::from_chars_result const x{
    std::from_chars(head.data(), head.data() + head.size(), a)
  };
  std::from_chars_result const y{
    std::from_chars(tail.data(), tail.data() + tail.size(), b)
  };
  return (x.ec == std::errc{}) && (x.ptr == (arg.data() + colon)) &&
         (y.ec == std::errc{}) && (y.ptr == (arg.data() + arg.size()));
}

// `N` ordinals separated by colons, the whole of `arg`.
template <size_t N>
bool ordinal_fields(std::string_view arg, std::array<uint32_t, N> &field) {
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
  return true;
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
  if (flag == "--orient") {
    std::from_chars_result const got{
      std::from_chars(value.data(), value.data() + value.size(), a)
    };
    if ((got.ec != std::errc{}) || (got.ptr != (value.data() + value.size()))) {
      return false;
    }
    out.pins.orients.push_back({ .frame = SubmachineId{ a } });
    return true;
  }
  if ((flag == "--face") || (flag == "--side")) {
    std::array<uint32_t, 4> field{};
    if (!ordinal_fields(value, field) || (field[2] > 1) || (field[3] > 3)) {
      return false;
    }
    if (flag == "--face") {
      out.pins.faces.push_back({ .trans = TransId{ field[0] },
                                 .leg = field[1],
                                 .end = field[2],
                                 .face = field[3] });
    } else {
      out.pins.sides.push_back({ .trans = TransId{ field[0] },
                                 .leg = field[1],
                                 .end = field[2],
                                 .side = field[3] });
    }
    return true;
  }
  if ((flag == "--fold") && (std::ranges::count(value, ':') == 2)) {
    std::array<uint32_t, 3> field{};
    if (!ordinal_fields(value, field) || (field[1] > FOLD_NEVER)) { return false; }
    out.pins.folds.push_back(
        { .frame = SubmachineId{ field[0] }, .mode = field[1], .layer = field[2] });
    return true;
  }
  if (!ordinal_pair(value, a, b)) { return false; }
  if (flag == "--fold") {
    if (b > FOLD_NEVER) { return false; }
    out.pins.folds.push_back({ .frame = SubmachineId{ a }, .mode = b });
  } else if (flag == "--rank") {
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
  if ((arg == "--no-search") || (arg == "--no-text")) {
    bool &flag{ (arg == "--no-search") ? out.no_search : out.no_text };
    if (flag) { return ArgRead::Malformed; }
    flag = true;
    out.given = true;
    return ArgRead::Taken;
  }
  if ((arg != "--profile") && (arg != "--portfolio-row") && (arg != "--rank") &&
      (arg != "--cut") && (arg != "--reverse") && (arg != "--face") &&
      (arg != "--orient") && (arg != "--side") && (arg != "--fold")) {
    return ArgRead::NotOurs;
  }
  if ((i + 1) >= argc) { return ArgRead::Malformed; }
  ++i;
  out.given = true;
  if (arg == "--profile") {
    out.profile = argv[i];
    return ArgRead::Taken;
  }
  return read_value(arg, argv[i], out) ? ArgRead::Taken : ArgRead::Malformed;
}

void append_layout_args(std::string &out,
                        LayoutArgs const &args,
                        uint32_t row,
                        SearchPins const &pins) {
  auto const pair = [&out](char const *flag, uint32_t a, uint32_t b) {
    out += ' ';
    out += flag;
    out += ' ';
    string_append_u32(out, a);
    out += ':';
    string_append_u32(out, b);
  };
  if (std::string_view{ args.profile } != "readable") {
    out += "--profile ";
    out += args.profile;
    out += ' ';
  }
  if (args.no_text) { out += "--no-text "; }
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
  for (OrientPin const &o : pins.orients) {
    out += " --orient ";
    string_append_u32(out, o.frame.v);
  }
  for (SidePin const &sp : pins.sides) {
    pair("--side", sp.trans.v, sp.leg);
    out += ':';
    string_append_u32(out, sp.end);
    out += ':';
    string_append_u32(out, sp.side);
  }
  for (FoldPin const &f : pins.folds) {
    pair("--fold", f.frame.v, f.mode);
    if (f.layer != 0) {
      out += ':';
      string_append_u32(out, f.layer);
    }
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
  // With no chart, diagnostics render against the loader's buffers.
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
