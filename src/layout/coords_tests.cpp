// Coordinate assignment tests on hand-written layered graphs: type-1 marking,
// one pass worked by hand, and properties of the balanced result.

#include "layout/coords.h"

#include "doctest.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace scav {

// Test-only declarations of the SCAV_INTERNAL functions in `coords.cpp`.
std::vector<uint8_t> coords_mark_type1(CoordGraph const &g);
std::vector<int64_t> coords_one_pass(CoordGraph const &g,
                                     std::vector<uint8_t> const &mark,
                                     bool upward,
                                     bool rightward);
std::vector<int32_t> coords_place(CoordGraph const &g);

}  // namespace scav

namespace {

using namespace scav;

std::vector<int32_t> cross_coordinates(CoordGraph const &g) {
  std::vector<int32_t> out;
  scav::cross_coordinates(g, out);
  return out;
}

constexpr int32_t EXT{ 100 };
constexpr int32_t SEP{ 20 };

// Every node `EXT` wide, separated by `SEP`.
CoordGraph uniform(uint32_t nodes,
                   std::vector<std::vector<uint32_t>> layers,
                   std::vector<CoordGraph::Edge> edges) {
  return { .extent = std::vector<int32_t>(nodes, EXT),
           .layers = std::move(layers),
           .edges = std::move(edges),
           .sep = SEP };
}

// Shared by the type-1 tests: 0 -> {1, 2}; the chain 1 -> 4 -> 5 is inner at
// its first hop, and the real segment 2 -> 3 crosses it.
CoordGraph crossing_graph() {
  return uniform(6,
                 { { 0 }, { 1, 2 }, { 3, 4 }, { 5 } },
                 { { .from = 0, .to = 1, .inner = 0 },
                   { .from = 0, .to = 2, .inner = 0 },
                   { .from = 1, .to = 4, .inner = 1 },
                   { .from = 2, .to = 3, .inner = 0 },
                   { .from = 4, .to = 5, .inner = 0 } });
}

int32_t leading(CoordGraph const &g, std::vector<int32_t> const &c, uint32_t node) {
  return c[node] - (g.extent[node] / 2);
}

int32_t trailing(CoordGraph const &g, std::vector<int32_t> const &c, uint32_t node) {
  return c[node] + (g.extent[node] / 2);
}

}  // namespace

TEST_CASE("coords: an empty graph and an unplaced node") {
  CHECK(cross_coordinates({}).empty());
  CoordGraph g{ uniform(2, { { 0 } }, {}) };
  std::vector<int32_t> const c{ cross_coordinates(g) };
  REQUIRE(c.size() == 2);
  CHECK(leading(g, c, 0) == 0);
  CHECK(c[1] == 0);  // in no layer, left at zero
}

TEST_CASE("coords: one node sits at the origin") {
  CoordGraph const g{ uniform(1, { { 0 } }, {}) };
  std::vector<int32_t> const c{ cross_coordinates(g) };
  REQUIRE(c.size() == 1);
  CHECK(c[0] == EXT / 2);
}

TEST_CASE("coords: one pass over two nodes and a shared successor, by hand") {
  // L0 = [0, 1], L1 = [2], edge 0 -> 2. Block {0, 2} lands at zero and 1 is
  // pushed past it by one separation: ceil((100+100)/2) + 20.
  CoordGraph const g{
    uniform(3, { { 0, 1 }, { 2 } }, { { .from = 0, .to = 2, .inner = 0 } })
  };
  std::vector<uint8_t> const mark(g.edges.size(), 0);
  std::vector<int64_t> const x{ coords_one_pass(g, mark, false, false) };
  REQUIRE(x.size() == 3);
  CHECK(x[0] == 0);
  CHECK(x[1] == 120);
  CHECK(x[2] == 0);  // aligned with its median predecessor
}

TEST_CASE("coords: type-1 marks the real segment, not the inner one") {
  CoordGraph const g{ crossing_graph() };
  std::vector<uint8_t> const mark{ coords_mark_type1(g) };
  REQUIRE(mark.size() == 5);
  CHECK(mark[2] == 0);  // 1 -> 4, the inner segment
  CHECK(mark[3] == 1);  // 2 -> 3, the real segment crossing it
  CHECK(mark[0] == 0);
  CHECK(mark[1] == 0);
  CHECK(mark[4] == 0);
}

TEST_CASE("coords: marking is what keeps the inner segment straight") {
  CoordGraph const g{ crossing_graph() };
  std::vector<int64_t> const marked{
    coords_one_pass(g, coords_mark_type1(g), false, false)
  };
  CHECK(marked[1] == marked[4]);

  // Unmarked, the crossing segment takes the alignment and the chain bends by two
  // separations.
  std::vector<uint8_t> const none(g.edges.size(), 0);
  std::vector<int64_t> const unmarked{ coords_one_pass(g, none, false, false) };
  CHECK(unmarked[1] != unmarked[4]);
}

TEST_CASE("coords: a chain through three layers comes out straight") {
  CoordGraph const g{ uniform(
      3,
      { { 0 }, { 1 }, { 2 } },
      { { .from = 0, .to = 1, .inner = 0 }, { .from = 1, .to = 2, .inner = 0 } }) };
  std::vector<int32_t> const c{ cross_coordinates(g) };
  CHECK(c[0] == c[1]);
  CHECK(c[1] == c[2]);
}

TEST_CASE("coords: an edge met off its ends' centres aligns where it meets them") {
  // Edge 0 -> 1 meets its ends at offsets 10 and -230 and aligns those points.
  // `2` follows `1` in its layer and keeps its separation from `1`.
  CoordGraph const g{ uniform(
      3,
      { { 0 }, { 1, 2 } },
      { { .from = 0, .to = 1, .inner = 0, .from_at = 10, .to_at = -230 } }) };
  std::vector<uint8_t> const mark(g.edges.size(), 0);
  for (uint32_t k = 0; k < 4; ++k) {
    CAPTURE(k);
    std::vector<int64_t> const x{ coords_one_pass(g, mark, (k & 2U) != 0, (k & 1U) != 0) };
    CHECK(x[0] + 10 == x[1] - 230);
    CHECK(x[2] - x[1] >= EXT + SEP);
  }
  std::vector<int32_t> const c{ cross_coordinates(g) };
  CHECK(c[0] + 10 == c[1] - 230);
  CHECK(leading(g, c, 2) - trailing(g, c, 1) >= SEP);
}

TEST_CASE("coords: a weak edge anchors its lower end only where nothing else can") {
  // `2` aligns with strong `1` over weak `0`. In `blocked`, `3`'s strong median `0` is
  // taken by `2`, so `3` aligns with its weak one, `1`.
  CoordGraph const g{ uniform(4,
                              { { 0, 1 }, { 2, 3 } },
                              { { .from = 0, .to = 2, .inner = 0, .weak = 1 },
                                { .from = 1, .to = 2, .inner = 0 },
                                { .from = 1, .to = 3, .inner = 0, .weak = 1 } }) };
  std::vector<uint8_t> const mark(g.edges.size(), 0);
  std::vector<int64_t> const x{ coords_one_pass(g, mark, false, false) };
  CHECK(x[2] == x[1]);
  CHECK(x[2] != x[0]);

  CoordGraph const blocked{ uniform(4,
                                    { { 0, 1 }, { 2, 3 } },
                                    { { .from = 0, .to = 2, .inner = 0 },
                                      { .from = 0, .to = 3, .inner = 0 },
                                      { .from = 1, .to = 3, .inner = 0, .weak = 1 } }) };
  std::vector<int64_t> const y{ coords_one_pass(blocked, mark, false, false) };
  CHECK(y[2] == y[0]);
  CHECK(y[3] == y[1]);
}

TEST_CASE("coords: adjacent nodes keep their separation, mixed extents") {
  // Several nodes per layer, with odd and even extents.
  CoordGraph g;
  g.extent = { 41, 100, 7, 260, 33, 99, 15, 400 };
  g.layers = { { 0, 1, 2 }, { 3, 4 }, { 5, 6, 7 } };
  g.edges = { { .from = 0, .to = 4, .inner = 0 }, { .from = 1, .to = 3, .inner = 0 },
              { .from = 2, .to = 3, .inner = 0 }, { .from = 3, .to = 7, .inner = 0 },
              { .from = 4, .to = 5, .inner = 0 }, { .from = 4, .to = 6, .inner = 0 } };
  g.sep = 13;

  std::vector<int32_t> const c{ cross_coordinates(g) };
  for (std::vector<uint32_t> const &lay : g.layers) {
    for (uint32_t k = 1; k < lay.size(); ++k) {
      CHECK((leading(g, c, lay[k]) - trailing(g, c, lay[k - 1])) >= g.sep);
    }
  }
  int32_t least{ INT32_MAX };
  for (std::vector<uint32_t> const &lay : g.layers) {
    for (uint32_t const node : lay) {
      least = (leading(g, c, node) < least) ? leading(g, c, node) : least;
    }
  }
  CHECK(least == 0);
}

TEST_CASE("coords: separation survives a graph with a long chain and a wide node") {
  // The chain 1 -> 3 -> 5 is inner throughout and sits beside node 2, 900 wide.
  CoordGraph g;
  g.extent = { 60, 8, 900, 8, 60, 8, 60 };
  g.layers = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6 } };
  g.edges = { { .from = 0, .to = 2, .inner = 0 }, { .from = 1, .to = 3, .inner = 1 },
              { .from = 2, .to = 4, .inner = 0 }, { .from = 3, .to = 5, .inner = 1 },
              { .from = 4, .to = 6, .inner = 0 }, { .from = 5, .to = 6, .inner = 0 } };
  g.sep = 10;

  std::vector<int32_t> const c{ cross_coordinates(g) };
  for (std::vector<uint32_t> const &lay : g.layers) {
    for (uint32_t k = 1; k < lay.size(); ++k) {
      CHECK((leading(g, c, lay[k]) - trailing(g, c, lay[k - 1])) >= g.sep);
    }
  }
}

TEST_CASE("coords: all four passes agree on separation on their own") {
  CoordGraph g{ uniform(6,
                        { { 0, 1 }, { 2, 3 }, { 4, 5 } },
                        { { .from = 0, .to = 3, .inner = 0 },
                          { .from = 1, .to = 2, .inner = 0 },
                          { .from = 2, .to = 5, .inner = 0 },
                          { .from = 3, .to = 4, .inner = 0 } }) };
  std::vector<uint8_t> const mark{ coords_mark_type1(g) };
  for (uint32_t k = 0; k < 4; ++k) {
    std::vector<int64_t> const x{ coords_one_pass(g, mark, (k & 2U) != 0, (k & 1U) != 0) };
    for (std::vector<uint32_t> const &lay : g.layers) {
      for (uint32_t i = 1; i < lay.size(); ++i) {
        int64_t const gap{ (x[lay[i]] - (g.extent[lay[i]] / 2)) -
                           (x[lay[i - 1]] + (g.extent[lay[i - 1]] / 2)) };
        CHECK(gap >= g.sep);
      }
    }
  }
}

TEST_CASE("coords: two runs over one graph agree") {
  CoordGraph const g{ crossing_graph() };
  CHECK(cross_coordinates(g) == cross_coordinates(g));
}

namespace {

struct Lcg {
  uint64_t s;
  uint32_t next(uint32_t n) {
    s = (s * 6364136223846793005ULL) + 1442695040888963407ULL;
    return static_cast<uint32_t>((s >> 33U) % n);
  }
};

CoordGraph random_graph(Lcg &r) {
  CoordGraph g;
  g.sep = 10 + static_cast<int32_t>(r.next(30));
  uint32_t const layers{ 2 + r.next(4) };
  for (uint32_t l = 0; l < layers; ++l) {
    std::vector<uint32_t> lay;
    uint32_t const width{ 1 + r.next(4) };
    for (uint32_t k = 0; k < width; ++k) {
      lay.push_back(static_cast<uint32_t>(g.extent.size()));
      g.extent.push_back(20 + static_cast<int32_t>(r.next(180)));
    }
    if (l > 0) {
      std::vector<uint32_t> const &up{ g.layers.back() };
      for (uint32_t const node : lay) {
        uint32_t const fan{ 1 + r.next(2) };
        for (uint32_t f = 0; f < fan; ++f) {
          g.edges.push_back({ .from = up[r.next(static_cast<uint32_t>(up.size()))],
                              .to = node,
                              .inner = r.next(4) == 0 ? 1U : 0U,
                              .from_at = static_cast<int32_t>(r.next(61)) - 30,
                              .to_at = static_cast<int32_t>(r.next(61)) - 30,
                              .weak = r.next(5) == 0 ? 1U : 0U });
        }
      }
    }
    g.layers.push_back(std::move(lay));
  }
  return g;
}

CoordGraph mutated(CoordGraph g, uint32_t kind, Lcg &r) {
  auto const edge = [&]() -> CoordGraph::Edge & {
    return g.edges[r.next(static_cast<uint32_t>(g.edges.size()))];
  };
  switch (kind) {
    case 0: g.sep += 7; break;
    case 1: g.extent[r.next(static_cast<uint32_t>(g.extent.size()))] += 40; break;
    case 2: {
      std::vector<uint32_t> &lay{
        g.layers[r.next(static_cast<uint32_t>(g.layers.size()))]
      };
      if (lay.size() > 1) { std::swap(lay[0], lay[lay.size() - 1]); }
      break;
    }
    case 3: edge().inner ^= 1U; break;
    case 4: edge().from_at += 25; break;
    case 5: edge().to_at -= 25; break;
    case 6: edge().weak ^= 1U; break;
    default: g.edges.pop_back(); break;
  }
  return g;
}

}  // namespace

TEST_CASE("coords: a remembered placement is the placement of that graph") {
  // Each graph is placed once to fill the memo, then each field is changed in turn and
  // placed both through the memo and afresh.
  Lcg r{ 12345 };
  constexpr uint32_t KINDS{ 8 };
  std::vector<uint32_t> moved(KINDS, 0);
  bool agree{ true };
  for (uint32_t trial = 0; trial < 400; ++trial) {
    CoordGraph const g{ random_graph(r) };
    if (g.edges.empty()) { continue; }
    std::vector<int32_t> const first{ cross_coordinates(g) };
    agree = agree && (first == coords_place(g));
    for (uint32_t kind = 0; kind < KINDS; ++kind) {
      CoordGraph const h{ mutated(g, kind, r) };
      std::vector<int32_t> const got{ cross_coordinates(h) };
      std::vector<int32_t> const want{ coords_place(h) };
      agree = agree && (got == want);
      if ((h.extent.size() == g.extent.size()) && (want != first)) { ++moved[kind]; }
    }
    agree = agree && (cross_coordinates(g) == first);
  }
  CHECK(agree);
  // Every kind of change moves some placement.
  for (uint32_t kind = 0; kind < KINDS; ++kind) {
    CAPTURE(kind);
    CHECK(moved[kind] > 0);
  }
}
