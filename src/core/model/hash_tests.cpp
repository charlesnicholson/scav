// The structural digest, from both sides: which differences move it and which
// leave it alone.

#include "core/core_internal.h"
#include "core/tests/test_support.h"
#include "scav/scav_core.h"
#include "scav/scav_types.h"

#include "doctest.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace scav;
using namespace scav::test;

Chart lowered(std::string_view text, std::string_view name = "c.scav") {
  Chart c;
  std::vector<Diagnostic> diags;
  Parsed const p{ parse(text, name) };
  REQUIRE(p.ok);
  std::ignore = lower_document(c, p.pd, diags);
  return c;
}

constexpr std::string_view BASE{
  R"(chart c "label" {
       @k = "v",
       state A "leaf",
       state B choice,
       trans * -> A,
       trans A -> B "go" { @w = "1", },
       state N { submachine m { state X, }, submachine n { state X, }, },
     })"
};

}  // namespace

TEST_CASE("digest: an empty chart hashes without touching a null buffer") {
  Chart const empty;
  std::vector<scav_byte> bytes;
  chart_digest_bytes(empty, bytes);
  CHECK_FALSE(bytes.empty());  // the counts alone are content
  CHECK(chart_structural_hash(empty) == chart_structural_hash(Chart{}));
}

TEST_CASE("digest: the same model hashes the same every time") {
  Chart const a{ lowered(BASE) };
  Chart const b{ lowered(BASE) };
  CHECK(chart_structural_hash(a) == chart_structural_hash(b));
  std::vector<scav_byte> pa;
  std::vector<scav_byte> pb;
  chart_digest_bytes(a, pa);
  chart_digest_bytes(b, pb);
  CHECK(pa == pb);
}

TEST_CASE("digest: the document name is not part of the model") {
  // One network loaded three ways names the same file differently -- a buffer,
  // a relative path, a URL -- and must still agree.
  Chart const a{ lowered(BASE, "one.scav") };
  Chart const b{ lowered(BASE, "https://host/deep/two.scav") };
  CHECK(chart_structural_hash(a) == chart_structural_hash(b));
}

TEST_CASE("digest: reformatting the source is not a model change") {
  // Whitespace and the short keyword spellings are the printer's business.
  Chart const spaced{ lowered("chart c {\n  state A,\n  trans * -> A,\n}") };
  Chart const dense{ lowered("chart c { s A, t * -> A, }") };
  CHECK(chart_structural_hash(spaced) == chart_structural_hash(dense));
}

TEST_CASE("digest: the order two attributes were written in is not a model change") {
  // `scav fmt` sorts attributes by key bytes, so the authored order and the
  // canonical one are two producers of one model and have to agree.
  Chart const authored{ lowered(R"(chart c { @zeta = "1", @alpha = "2", })") };
  Chart const sorted{ lowered(R"(chart c { @alpha = "2", @zeta = "1", })") };
  CHECK(chart_structural_hash(authored) == chart_structural_hash(sorted));

  // Namespaced keys compose to `ns:key`, which is what the printer sorts on.
  Chart const blocked{ lowered(R"(chart c { @ns { b = "2", a = "1" }, })") };
  Chart const split{ lowered(R"(chart c { @ns:a = "1", @ns:b = "2", })") };
  CHECK(chart_structural_hash(blocked) == chart_structural_hash(split));
}

TEST_CASE("digest: one key's values keep the order they were written in") {
  // The sort is stable for exactly this: a repeated key is a list, and a list
  // that reordered itself would be a different model.
  Chart const ab{ lowered(R"(chart c { @k = "a", @k = "b", })") };
  Chart const ba{ lowered(R"(chart c { @k = "b", @k = "a", })") };
  CHECK(chart_structural_hash(ab) != chart_structural_hash(ba));
}

TEST_CASE("digest: sorting attributes does not erase one") {
  Chart const two{ lowered(R"(chart c { @a = "1", @b = "2", })") };
  Chart const one{ lowered(R"(chart c { @a = "1", })") };
  Chart const other{ lowered(R"(chart c { @a = "1", @b = "3", })") };
  CHECK(chart_structural_hash(two) != chart_structural_hash(one));
  CHECK(chart_structural_hash(two) != chart_structural_hash(other));
}

TEST_CASE("digest: a comment is not a model change") {
  Chart const bare{ lowered("chart c { state A, }") };
  Chart const noted{ lowered("chart c {\n // why\n state A, // trailing\n}") };
  CHECK(chart_structural_hash(bare) == chart_structural_hash(noted));
}

TEST_CASE("digest: every structural field moves it") {
  uint32_t const base{ chart_structural_hash(lowered(BASE)) };
  struct Case {
    char const *what;
    std::string_view text;
  };
  constexpr std::array<Case, 12> CASES{ {
      { .what = "chart name", .text = R"(chart d "label" { state A, })" },
      { .what = "chart label", .text = R"(chart c "other" { state A, })" },
      { .what = "state name", .text = R"(chart c "label" { state Z, })" },
      { .what = "state label", .text = R"(chart c "label" { state A "other", })" },
      { .what = "state kind", .text = R"(chart c "label" { state A choice, })" },
      { .what = "an added state", .text = R"(chart c "label" { state A, state B, })" },
      { .what = "transition label",
        .text = R"(chart c "label" { state A, trans A -> A "x", })" },
      { .what = "transition kind",
        .text = R"(chart c "label" { state A, trans internal A -> A "x", })" },
      { .what = "an attr key", .text = R"(chart c "label" { @j = "v", state A, })" },
      { .what = "an attr value", .text = R"(chart c "label" { @k = "w", state A, })" },
      { .what = "nesting", .text = R"(chart c "label" { state A { state B, }, })" },
      { .what = "submachine name",
        .text = R"(chart c "label" { state A { submachine q { state B, }, }, })" },
  } };
  std::vector<uint32_t> seen{ base };
  for (Case const &k : CASES) {
    CAPTURE(k.what);
    uint32_t const h{ chart_structural_hash(lowered(k.text)) };
    for (uint32_t const prior : seen) { CHECK(prior != h); }
    seen.push_back(h);
  }
}

TEST_CASE("digest: sibling order is structure") {
  Chart const ab{ lowered("chart c { state A, state B, }") };
  Chart const ba{ lowered("chart c { state B, state A, }") };
  CHECK(chart_structural_hash(ab) != chart_structural_hash(ba));
}

TEST_CASE("digest: attribute insertion order within a key is structure") {
  // A repeated key is a list, and its order is authored.
  Chart const xy{ lowered(R"(chart c { @k = ["x", "y"], state A, })") };
  Chart const yx{ lowered(R"(chart c { @k = ["y", "x"], state A, })") };
  CHECK(chart_structural_hash(xy) != chart_structural_hash(yx));
}

TEST_CASE("digest: a tombstone moves it") {
  Chart c{ lowered("chart c { state A, state B, }") };
  uint32_t const before{ chart_structural_hash(c) };
  c.states[0].live = 0;
  CHECK(chart_structural_hash(c) != before);
}

TEST_CASE("digest: the interned key id is not hashed, only its bytes") {
  // AttrKeyId is first-encounter order, so these two charts intern the same
  // keys in opposite orders and must still agree.
  Chart first;
  SubmachineId const froot{ build_chart(first, "c", {}) };
  StateId const fa{ build_state(first, froot, "A", StateKind::Normal, {}) };
  std::ignore = build_attr(first, ref(fa), "alpha", "1");
  std::ignore = build_attr(first, ref(fa), "beta", "2");

  Chart second;
  SubmachineId const sroot{ build_chart(second, "c", {}) };
  StateId const sz{ build_state(second, sroot, "Z", StateKind::Normal, {}) };
  std::ignore = build_attr(second, ref(sz), "beta", "9");  // interns `beta` as id 0
  second.states[sz.v].live = 0;                            // then take Z away again
  StateId const sa{ build_state(second, sroot, "A", StateKind::Normal, {}) };
  std::ignore = build_attr(second, ref(sa), "alpha", "1");
  std::ignore = build_attr(second, ref(sa), "beta", "2");

  CHECK(chart_attr_key_find(first, "alpha").v == 0);
  CHECK(chart_attr_key_find(second, "alpha").v == 1);  // different ids, same key

  // The second chart holds a tombstoned Z, so the models differ overall; the
  // narrower claim is that the digest carries each key's bytes.
  auto const encoded = [](std::string_view text) {
    std::string out;
    for (uint32_t i = 0; i < 4; ++i) {
      out.push_back(static_cast<char>((text.size() >> (i * 8U)) & 0xFFU));
    }
    out += text;
    return out;
  };
  auto const carries = [&](Chart const &c, std::string_view text) {
    std::vector<scav_byte> digest;
    chart_digest_bytes(c, digest);
    std::string_view const view{ reinterpret_cast<char const *>(digest.data()),
                                 digest.size() };
    return view.find(encoded(text)) != std::string_view::npos;
  };
  CHECK(carries(first, "alpha"));
  CHECK(carries(second, "alpha"));
  CHECK(carries(first, "beta"));
  CHECK(carries(second, "beta"));
}

TEST_CASE("digest: an include's authored path is hashed, its DocId spelling is not") {
  Chart c;
  SubmachineId const root{ build_chart(c, "c", {}) };
  std::ignore = build_include(c, root, "w", "wifi.scav");
  uint32_t const before{ chart_structural_hash(c) };

  Chart d;
  SubmachineId const droot{ build_chart(d, "c", {}) };
  std::ignore = build_include(d, droot, "w", "./wifi.scav");
  CHECK(chart_structural_hash(d) != before);
}

TEST_CASE("digest: a column is the extension's to hash, not the model's") {
  Chart c{ lowered("chart c { state A, }") };
  uint32_t const before{ chart_structural_hash(c) };
  ColumnId const col{
    column_register(c, "ext.marks", ElemKind::State, ValueKind::U32, 4, 4, 0)
  };
  REQUIRE(col.v != INVALID);
  CHECK(chart_structural_hash(c) == before);
}

TEST_CASE("digest: length prefixes keep adjacent strings apart") {
  // Without them `state ab` and `state a` + label `b` could serialize alike.
  Chart const one{ lowered(R"(chart c { state ab, })") };
  Chart const two{ lowered(R"(chart c { state a "b", })") };
  CHECK(chart_structural_hash(one) != chart_structural_hash(two));
}
