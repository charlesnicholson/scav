#ifndef SCAV_CORE_TESTS_CORPUS_H_INCLUDED
#define SCAV_CORE_TESTS_CORPUS_H_INCLUDED

// Header-only and test-only.

#include "scav/scav_core.h"
#include "scav/scav_layout.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace scav { void layout_test_no_search(bool on); }  // namespace scav

namespace scav::test {

// Every chart in test_data/charts/gauntlet, by file name; test_gauntlet.py checks it
// against the directory.
inline constexpr std::array<char const *, 54> GAUNTLET{
  "above.scav",     "between.scav",   "bypass.scav",  "carried.scav",   "chain.scav",
  "corner.scav",    "crossing.scav",  "crowd.scav",   "detour.scav",    "diagonal.scav",
  "divider.scav",   "enclosing.scav", "entered.scav", "fanin.scav",     "fanout.scav",
  "fanwide.scav",   "flank.scav",     "folded.scav",  "fork.scav",      "headed.scav",
  "header.scav",    "inloop.scav",    "inside.scav",  "inward.scav",    "kicked.scav",
  "lane.scav",      "level.scav",     "long.scav",    "loop.scav",      "marks.scav",
  "mixed.scav",     "mutual.scav",    "offset.scav",  "outside.scav",   "ported.scav",
  "pulled.scav",    "rebound.scav",   "reentry.scav", "regions.scav",   "resumed.scav",
  "ring.scav",      "room.scav",      "rooms.scav",   "roundtrip.scav", "seated.scav",
  "separator.scav", "side.scav",      "stacked.scav", "stretch.scav",   "through.scav",
  "tight.scav",     "transit.scav",   "under.scav",   "unfolded.scav"
};

// Whether the corpus is brew, dock, estop, kiln and led: the fast tier, not full or
// exhaustive.
inline bool corpus_light() {
  char const *const tier{ std::getenv("SCAV_TEST_TIER") };
  return (tier == nullptr) || ((std::string_view{ tier } != "full") &&
                               (std::string_view{ tier } != "exhaustive"));
}

// Whether the light corpus drops this chart; a gauntlet chart is never dropped.
inline bool corpus_skipped(std::string_view chart) {
  bool const light{ corpus_light() };
  bool const corpus{ chart.ends_with(".scav") &&
                     (chart.find('/') == std::string_view::npos) };
  return light && corpus && (chart != "brew.scav") && (chart != "dock.scav") &&
         (chart != "estop.scav") && (chart != "kiln.scav") && (chart != "led.scav");
}

// Whether the light tier skips a whole search of `chart`: brew, kiln and every chart it
// drops.
inline bool search_skipped(std::string_view chart) {
  return corpus_skipped(chart) ||
         (corpus_light() && ((chart == "brew.scav") || (chart == "kiln.scav")));
}

// `golden` less every line naming a skipped chart as one of its words.
inline std::string corpus_golden(std::string_view golden) {
  std::string out;
  while (!golden.empty()) {
    size_t const eol{ golden.find('\n') };
    std::string_view const line{
      golden.substr(0, (eol == std::string_view::npos) ? golden.size() : eol + 1)
    };
    golden.remove_prefix(line.size());
    bool skipped{ false };
    for (std::string_view rest{ line }; !rest.empty() && !skipped;) {
      size_t const end{ rest.find_first_of(" \n") };
      skipped = corpus_skipped(rest.substr(0, end));
      rest.remove_prefix((end == std::string_view::npos) ? rest.size() : end + 1);
    }
    if (!skipped) { out += line; }
  }
  return out;
}

// corpus_pins.txt: per chart and scale, the flags `scav dump --layout` prints after
// "rests on", at `readable`. A line at no text carries `--no-text`.
inline std::string corpus_pins_file() {
  std::vector<scav_byte> bytes;
  if (!read_file(SCAV_TEST_DATA_DIR "/golden/layout/corpus_pins.txt", bytes)) {
    return {};
  }
  return { reinterpret_cast<char const *>(bytes.data()), bytes.size() };
}

// The lines of `golden` at one scale.
inline std::string corpus_pins_at(std::string_view golden, bool text) {
  std::string out;
  while (!golden.empty()) {
    size_t const eol{ golden.find('\n') };
    std::string_view const line{
      golden.substr(0, (eol == std::string_view::npos) ? golden.size() : eol + 1)
    };
    golden.remove_prefix(line.size());
    if ((line.find(" --no-text ") == std::string_view::npos) == text) { out += line; }
  }
  return out;
}

// `chart`'s committed line at one scale, or empty.
inline std::string corpus_pins_of(std::string_view chart, bool text) {
  std::string const lines{ corpus_pins_at(corpus_pins_file(), text) };
  std::string const key{ std::string{ chart } + ' ' };
  for (size_t at = 0; at < lines.size();) {
    size_t const eol{ lines.find('\n', at) };
    size_t const end{ (eol == std::string::npos) ? lines.size() : eol + 1 };
    if (lines.compare(at, key.size(), key) == 0) { return lines.substr(at, end - at); }
    at = end;
  }
  return {};
}

// Whether the exhaustive tier runs.
inline bool corpus_exhaustive() {
  char const *const tier{ std::getenv("SCAV_TEST_TIER") };
  return (tier != nullptr) && (std::string_view{ tier } == "exhaustive");
}

// Whether a search-reaches-pins case searches `chart`: never mill, bottler only in the
// exhaustive tier.
inline bool corpus_searched(std::string_view chart) {
  return !corpus_skipped(chart) && (chart != "mill.scav") &&
         (corpus_exhaustive() || (chart != "bottler.scav"));
}

// `chart`'s line as `scav dump --layout` would print what a run took.
inline std::string corpus_pins_line(std::string_view chart,
                                    uint32_t row,
                                    SearchPins const &pins,
                                    bool text) {
  std::string out{ chart };
  out += text ? " --portfolio-row " : " --no-text --portfolio-row ";
  out += std::to_string(row);
  auto const flag = [&out](char const *name, std::initializer_list<uint32_t> fields) {
    out += ' ';
    out += name;
    char sep{ ' ' };
    for (uint32_t const f : fields) {
      out += sep;
      out += std::to_string(f);
      sep = ':';
    }
  };
  for (RankPin const &r : pins.ranks) { flag("--rank", { r.state.v, r.rank }); }
  for (ChainCut const &k : pins.cuts) { flag("--cut", { k.trans.v, k.leg }); }
  for (ReversePin const &r : pins.reverses) { flag("--reverse", { r.trans.v, r.leg }); }
  for (EndPin const &e : pins.ends) { flag("--end", { e.trans.v, e.leg, e.end, e.face }); }
  for (OrientPin const &o : pins.orients) { flag("--orient", { o.frame.v }); }
  for (FoldPin const &f : pins.folds) {
    if (f.layer != 0) {
      flag("--fold", { f.frame.v, f.mode, f.layer });
    } else {
      flag("--fold", { f.frame.v, f.mode });
    }
  }
  for (LoopPin const &l : pins.loops) { flag("--loop", { l.state.v, l.face, l.end }); }
  out += '\n';
  return out;
}

// corpus_pins.txt with its lines at one scale replaced by `actual`, no-text lines first.
inline void corpus_pins_write(std::string const &actual, bool text) {
  std::string const golden{ corpus_pins_file() };
  std::string const out{ text ? (corpus_pins_at(golden, false) + actual)
                              : (actual + corpus_pins_at(golden, true)) };
  write_file(SCAV_TEST_OUT_DIR "/corpus_pins.txt",
             reinterpret_cast<scav_byte const *>(out.data()),
             out.size());
}

// `flags` as `scav dump --layout` reads them into a row and pins; false on a flag it
// does not take.
inline bool corpus_pins_read(std::string_view flags, uint32_t &row, SearchPins &pins) {
  std::vector<std::string_view> words;
  while (!flags.empty()) {
    size_t const end{ flags.find_first_of(" \n") };
    if (end != 0) { words.push_back(flags.substr(0, end)); }
    flags.remove_prefix((end == std::string_view::npos) ? flags.size() : end + 1);
  }
  for (size_t i = 0; i < words.size(); ++i) {
    std::string_view const name{ words[i] };
    if (name == "--no-text") { continue; }
    if ((i + 1) >= words.size()) { return false; }
    std::string_view value{ words[++i] };
    std::array<uint32_t, 4> f{};  // a field the value omits reads zero
    for (uint32_t &field : f) {
      if (value.empty()) { break; }
      size_t const colon{ value.find(':') };
      std::string_view const head{ value.substr(0, colon) };
      if (std::from_chars(head.data(), head.data() + head.size(), field).ec !=
          std::errc{}) {
        return false;
      }
      value.remove_prefix((colon == std::string_view::npos) ? value.size() : colon + 1);
    }
    if (name == "--portfolio-row") {
      row = f[0];
    } else if (name == "--rank") {
      pins.ranks.push_back({ .state = StateId{ f[0] }, .rank = f[1] });
    } else if (name == "--cut") {
      pins.cuts.push_back({ .trans = TransId{ f[0] }, .leg = f[1] });
    } else if (name == "--reverse") {
      pins.reverses.push_back({ .trans = TransId{ f[0] }, .leg = f[1] });
    } else if (name == "--end") {
      pins.ends.push_back(
          { .trans = TransId{ f[0] }, .leg = f[1], .end = f[2], .face = f[3] });
    } else if (name == "--orient") {
      pins.orients.push_back({ .frame = SubmachineId{ f[0] } });
    } else if (name == "--fold") {
      pins.folds.push_back({ .frame = SubmachineId{ f[0] }, .mode = f[1], .layer = f[2] });
    } else if (name == "--loop") {
      pins.loops.push_back({ .state = StateId{ f[0] }, .face = f[1], .end = f[2] });
    } else {
      return false;
    }
  }
  return true;
}

// `layout_run` from `chart`'s committed row and pins with the move budget at zero; no
// space requests is the no-text line. False when the golden has no line for it.
inline bool corpus_layout(Chart &c,
                          std::string_view chart,
                          scav_spaces const &s,
                          scav_layout_opts const &o,
                          std::vector<scav_placed> &placed,
                          std::vector<Diagnostic> &diags,
                          uint32_t *inflations = nullptr) {
  std::string key{ '\n' };
  key += chart;
  key += (s.n_box_state != 0) ? " --portfolio-row " : " --no-text --portfolio-row ";
  std::string const golden{ '\n' + corpus_pins_file() };
  size_t const at{ golden.find(key) };
  if (at == std::string::npos) { return false; }
  size_t const from{ at + 1 + chart.size() };
  std::string_view const flags{
    std::string_view{ golden }.substr(from, golden.find('\n', from) - from)
  };
  uint32_t row{ INVALID };
  SearchPins pins;
  if (!corpus_pins_read(flags, row, pins) || (row == INVALID)) { return false; }
  layout_test_no_search(true);
  bool const laid{
    layout_run(c, s, o, placed, diags, inflations, nullptr, row, nullptr, nullptr, &pins)
  };
  layout_test_no_search(false);
  return laid;
}

}  // namespace scav::test

#endif
