// Canonical text prints as itself, over the corpus and at the scale target,
// plus the property that rests on: printing changes bytes and not the model.

#include "core/core_internal.h"
#include "core/tests/test_charts.h"
#include "core/tests/test_support.h"
#include "core/tests/test_synth.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"
#include "scav_stable_sort.h"

#include "doctest.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

struct CorpusChart {
  char const *name;
  std::string_view text;
};

std::vector<CorpusChart> corpus() {
  return { { "vac", VAC }, { "tcp", TCP }, { "ota", OTA } };
}

// The model as text, in array order, with each subject's attributes sorted --
// the one thing canonical form moves and the digest reads in span order.
void append_attrs(std::string &out, Chart const &c, Span attrs) {
  std::vector<std::string> rows;
  rows.reserve(attrs.len);
  for (uint32_t i = 0; i < attrs.len; ++i) {
    if ((attrs.off + i) >= c.attrs.size()) { break; }
    Attr const &a{ c.attrs[attrs.off + i] };
    rows.push_back(std::string{ chart_attr_key(c, a.key) } + "=" +
                   std::string{ chart_string(c, a.value) });
  }
  scav_stable_sort(rows, [](std::string const &a, std::string const &b) { return a < b; });
  for (std::string const &row : rows) {
    out += " @";
    out += row;
  }
}

void field(std::string &out, std::string_view key, std::string_view value) {
  out += ' ';
  out += key;
  out += "=\"";
  out += value;
  out += '"';
}

void field(std::string &out, std::string_view key, uint32_t value) {
  out += ' ';
  out += key;
  out += '=';
  out += std::to_string(value);
}

template <typename Ids>
void field_ids(std::string &out, std::string_view key, Ids const &ids, Span span) {
  for (uint32_t i = 0; i < span.len; ++i) { field(out, key, ids[span.off + i].v); }
}

std::string summarize(Chart const &c) {
  std::string out{ "chart" };
  field(out, "name", chart_string(c, c.name));
  field(out, "label", chart_string(c, c.label));
  append_attrs(out, c, c.chart_attrs);
  out += '\n';

  for (uint32_t i = 0; i < c.states.size(); ++i) {
    State const &s{ c.states[i] };
    out += "state";
    field(out, "row", i);
    field(out, "name", chart_string(c, s.name));
    field(out, "label", chart_string(c, s.label));
    field(out, "kind", syntax_state_kind_name(s.kind));
    field(out, "parent", s.parent.v);
    field(out, "live", s.live);
    field_ids(out, "sub", c.submachine_ids, s.submachines);
    append_attrs(out, c, s.attrs);
    out += '\n';
  }

  for (uint32_t i = 0; i < c.submachines.size(); ++i) {
    Submachine const &m{ c.submachines[i] };
    out += "submachine";
    field(out, "row", i);
    field(out, "name", chart_string(c, m.name));
    field(out, "label", chart_string(c, m.label));
    field(out, "owner", m.owner.v);
    field(out, "ordinal", m.ordinal);
    field(out, "live", m.live);
    field_ids(out, "child", c.state_ids, m.children);
    append_attrs(out, c, m.attrs);
    out += '\n';
  }

  for (uint32_t i = 0; i < c.transitions.size(); ++i) {
    Transition const &t{ c.transitions[i] };
    out += "trans";
    field(out, "row", i);
    field(out, "src", t.src.v);
    field(out, "dst", t.dst.v);
    field(out, "kind", syntax_trans_kind_name(t.kind));
    field(out, "label", chart_string(c, t.label));
    field(out, "live", t.live);
    append_attrs(out, c, t.attrs);
    out += '\n';
  }

  for (Include const &inc : c.includes) {
    out += "include";
    field(out, "alias", chart_string(c, inc.alias));
    field(out, "path", chart_string(c, inc.path));
    field(out, "host", inc.host.v);
    out += '\n';
  }
  return out;
}

// One document, lowered: cross-document resolution is the loader's, and the
// printer works a file at a time.
std::string model_of(std::string_view text) {
  Parsed const r{ parse(text) };
  REQUIRE(r.ok);
  Chart c;
  std::vector<Diagnostic> diags;
  std::ignore = lower_document(c, r.pd, diags);
  return summarize(c);
}

}  // namespace

TEST_CASE("printer: every corpus chart reaches a fixed point in one pass") {
  for (CorpusChart const &chart : corpus()) {
    CAPTURE(chart.name);
    std::string const once{ print(chart.text) };
    CHECK(once == print(once));

    Parsed const again{ parse(once) };
    CHECK(again.ok);
    CHECK(again.diags.empty());
  }
}

TEST_CASE("printer: printing a corpus chart changes its bytes and not its model") {
  for (CorpusChart const &chart : corpus()) {
    CAPTURE(chart.name);
    std::string const once{ print(chart.text) };
    CHECK(once != chart.text);  // canonical form is the printer's, not the format's
    CHECK(model_of(chart.text) == model_of(once));
  }
}

TEST_CASE("printer: a corpus chart is stable at every budget the bounds admit") {
  for (CorpusChart const &chart : corpus()) {
    for (uint32_t const columns : { PRINT_COLUMNS_MIN, 33U, 60U, 90U, 400U }) {
      CAPTURE(chart.name);
      CAPTURE(columns);
      std::string const once{ print(chart.text, columns) };
      CHECK(once == print(once, columns));
      CHECK(parse(once).ok);
      CHECK(model_of(chart.text) == model_of(once));
    }
  }
}

TEST_CASE("printer: no output line exceeds the budget unless one token does") {
  for (CorpusChart const &chart : corpus()) {
    for (uint32_t const columns : { 40U, 90U }) {
      CAPTURE(chart.name);
      CAPTURE(columns);
      std::string const out{ print(chart.text, columns) };
      size_t begin{ 0 };
      while (begin < out.size()) {
        size_t const end{ out.find('\n', begin) };
        std::string_view const line{ out.data() + begin,
                                     (end == std::string::npos ? out.size() : end) -
                                         begin };
        // Over-long is legal only for an unbreakable atom: a long label or
        // name, or a comment, rather than a block that could have split.
        if ((line.size() > columns) && (line.find("//") == std::string_view::npos)) {
          CAPTURE(line);
          CHECK(line.find(", ") == std::string_view::npos);
        }
        if (end == std::string::npos) { break; }
        begin = end + 1;
      }
    }
  }
}

TEST_CASE("printer: a depth-16 chart at the scale target") {
  SynthSpec spec{ synth_default_spec() };
  spec.comment_every = 5;
  spec.min_bytes = 0;
  spec.min_roots = 16;
  SynthStats stats{};
  std::string const text{ synth_document(spec, stats) };

  Parsed const source{ parse(text) };
  REQUIRE(source.ok);
  CHECK(stats.states >= 2000);
  CHECK(stats.comments > 0);

  std::string const once{ print(text) };
  CHECK_FALSE(once.empty());

  // Byte-identical on the second pass, comments and attribute forms included.
  CHECK(once == print(once));

  Parsed const again{ parse(once) };
  CHECK(again.ok);
  CHECK(again.diags.empty());
  CHECK(again.pd.comments.size() == source.pd.comments.size());
  CHECK(model_of(text) == model_of(once));

  // Not one statement for one: every state writes `@synth { ... }` and
  // `@synth:n2`, which merge. The entries survive, and they are the model.
  CHECK(again.pd.stmts.size() < source.pd.stmts.size());
  CHECK(again.pd.attr_entries.size() == source.pd.attr_entries.size());
  CHECK(again.pd.attr_values.size() == source.pd.attr_values.size());
}

TEST_CASE("printer: the deep document the depth cap admits still prints") {
  // Two blocks per state level plus one for the chart, so 120 levels of nesting
  // is well inside DEFAULT_MAX_DEPTH and far past the design target of 16.
  std::string const text{ synth_deep_document(120) };
  Parsed const r{ parse(text) };
  REQUIRE(r.ok);
  std::string const once{ print(text) };
  CHECK(once == print(once));
  CHECK(parse(once).ok);
}
