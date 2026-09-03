// Coordinate assignment against hand-written layered graphs: the type-1
// marking on its own, one pass on its own with numbers worked by hand, then
// the properties the balanced result has to keep.

#include "layout/coords.h"

#include "doctest.h"

#include <cstdint>
#include <vector>

namespace scav {

// The two steps `coords.cpp` brackets with SCAV_INTERNAL, declared here
// rather than in a header so the shipping build keeps them internal.
std::vector<uint8_t> coords_mark_type1(CoordGraph const &g);
std::vector<int64_t> coords_one_pass(CoordGraph const &g,
                                     std::vector<uint8_t> const &mark,
                                     bool upward,
                                     bool rightward);

}  // namespace scav

namespace {

using namespace scav;

constexpr int32_t EXT{ 100 };
constexpr int32_t SEP{ 20 };

// Uniform extents, so a hand-worked expectation is one separation value.
CoordGraph uniform(uint32_t nodes,
                   std::vector<std::vector<uint32_t>> layers,
                   std::vector<CoordGraph::Edge> edges) {
  return { .extent = std::vector<int32_t>(nodes, EXT),
           .layers = std::move(layers),
           .edges = std::move(edges),
           .sep = SEP };
}

// The graph both type-1 tests use: a real edge crossing an inner segment
// between the same two layers. 0 -> {1, 2}, the chain 1 -> 4 -> 5 is inner at
// its first hop, and 2 -> 3 is the real segment that crosses it.
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
  CHECK(c[1] == 0);  // in no layer, so it never took part
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

  // Without the mark, the crossing segment wins the alignment and the chain
  // bends by two separations instead.
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

TEST_CASE("coords: adjacent nodes keep their separation, mixed extents") {
  // A fan that forces every layer to hold several nodes at once, with extents
  // chosen odd so the halving in the separation formula is exercised.
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
  // The chain 1 -> 3 -> 5 is inner throughout and passes a node far wider
  // than itself, which is the shape that would collide if balancing averaged
  // the four passes per node without preserving the constraint.
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
