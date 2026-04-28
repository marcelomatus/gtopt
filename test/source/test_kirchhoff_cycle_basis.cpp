// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the cycle-basis builder.  Pure graph algorithm, no
// SystemContext / LineLP / LP coupling — these tests exercise the
// builder in isolation so the LP-integration layer (PR 2b) can rely on
// its correctness.
//
// Cycle-count invariant: for a connected graph with |L| edges and |B|
// buses, `build_fundamental_cycles` returns exactly `|L| − |B| + 1`
// cycles; with `k` islands, `|L| − |B| + k`.  Tests below verify this
// invariant and the per-cycle sign correctness.

#include <cstddef>
#include <set>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/kirchhoff_cycle_basis.hpp>

using namespace gtopt::kirchhoff::
    cycle_basis;  // NOLINT(google-global-names-in-headers)

namespace
{

/// Sum of "signed traversal" around a cycle: for KVL to be valid, every
/// vertex must be entered exactly as many times as it is exited.  If
/// the cycle is well-formed, the multiset of (entry, exit) flips
/// matches up everywhere.  This helper checks that closure: walking
/// the edges in order with the recorded signs forms a closed loop.
[[nodiscard]] bool is_closed_loop(const Cycle& cycle,
                                  const std::vector<Edge>& all_edges)
{
  if (cycle.empty()) {
    return false;
  }
  // Determine the start bus from the first edge + sign.
  auto edge_start = [&](const CycleEdge& ce) -> std::size_t
  {
    const auto& e = all_edges[ce.line_index];
    return ce.sign == +1 ? e.bus_a : e.bus_b;
  };
  auto edge_end = [&](const CycleEdge& ce) -> std::size_t
  {
    const auto& e = all_edges[ce.line_index];
    return ce.sign == +1 ? e.bus_b : e.bus_a;
  };
  // Walk: each edge's `end` must match the next edge's `start`.
  // After the last edge, we must return to the first edge's `start`.
  const auto first_start = edge_start(cycle.front());
  for (std::size_t i = 0; i < cycle.size(); ++i) {
    const auto end_of_i = edge_end(cycle[i]);
    const auto start_of_next =
        (i + 1 < cycle.size()) ? edge_start(cycle[i + 1]) : first_start;
    if (end_of_i != start_of_next) {
      return false;
    }
  }
  return true;
}

/// Return the multiset of edge line_indices in a cycle.
[[nodiscard]] std::multiset<std::size_t> cycle_edge_set(const Cycle& cycle)
{
  std::multiset<std::size_t> s;
  for (const auto& ce : cycle) {
    s.insert(ce.line_index);
  }
  return s;
}

}  // namespace

// ── Trivial graphs ────────────────────────────────────────────────

TEST_CASE("build_fundamental_cycles - empty graph yields no cycles")
{
  const auto cycles = build_fundamental_cycles(0, {});
  CHECK(cycles.empty());

  const auto cycles_isolated = build_fundamental_cycles(5, {});
  CHECK(cycles_isolated.empty());
}

TEST_CASE("build_fundamental_cycles - single edge yields no cycle")
{
  const std::vector<Edge> edges = {{.line_index = 0, .bus_a = 0, .bus_b = 1}};
  const auto cycles = build_fundamental_cycles(2, edges);
  CHECK(cycles.empty());
}

TEST_CASE("build_fundamental_cycles - tree (3-bus chain) yields no cycle")
{
  // 0 — 1 — 2  (no cycle: 2 edges, 3 buses, |L|−|B|+1 = 0)
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 1, .bus_b = 2},
  };
  const auto cycles = build_fundamental_cycles(3, edges);
  CHECK(cycles.empty());
}

TEST_CASE("build_fundamental_cycles - self-loop is silently dropped")
{
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 0, .bus_b = 0},  // self-loop
  };
  const auto cycles = build_fundamental_cycles(2, edges);
  CHECK(cycles.empty());
}

// ── Single-cycle graphs ───────────────────────────────────────────

TEST_CASE("build_fundamental_cycles - triangle: 1 cycle of 3 edges")
{
  // 0 — 1 — 2 — 0 ; 3 edges, 3 buses → |L|−|B|+1 = 1 cycle
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 1, .bus_b = 2},
      {.line_index = 2, .bus_a = 2, .bus_b = 0},
  };
  const auto cycles = build_fundamental_cycles(3, edges);
  REQUIRE(cycles.size() == 1);
  CHECK(cycles.front().size() == 3);
  CHECK(cycle_edge_set(cycles.front()) == std::multiset<std::size_t> {0, 1, 2});
  CHECK(is_closed_loop(cycles.front(), edges));
}

TEST_CASE("build_fundamental_cycles - 4-bus square: 1 cycle of 4 edges")
{
  // 0 — 1
  // |   |
  // 3 — 2
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 1, .bus_b = 2},
      {.line_index = 2, .bus_a = 2, .bus_b = 3},
      {.line_index = 3, .bus_a = 3, .bus_b = 0},
  };
  const auto cycles = build_fundamental_cycles(4, edges);
  REQUIRE(cycles.size() == 1);
  CHECK(cycles.front().size() == 4);
  CHECK(cycle_edge_set(cycles.front())
        == std::multiset<std::size_t> {0, 1, 2, 3});
  CHECK(is_closed_loop(cycles.front(), edges));
}

TEST_CASE("build_fundamental_cycles - parallel edges between same bus pair")
{
  // 0 — 1 with two parallel edges → 1 cycle of 2 edges
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 0, .bus_b = 1},  // parallel
  };
  const auto cycles = build_fundamental_cycles(2, edges);
  REQUIRE(cycles.size() == 1);
  CHECK(cycles.front().size() == 2);
  CHECK(cycle_edge_set(cycles.front()) == std::multiset<std::size_t> {0, 1});
  CHECK(is_closed_loop(cycles.front(), edges));
}

TEST_CASE("build_fundamental_cycles - parallel with reversed orientation")
{
  // 0 → 1 and 1 → 0 (same physical pair, opposite bus_a/bus_b)
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 1, .bus_b = 0},
  };
  const auto cycles = build_fundamental_cycles(2, edges);
  REQUIRE(cycles.size() == 1);
  CHECK(is_closed_loop(cycles.front(), edges));
}

// ── Multi-cycle graphs ────────────────────────────────────────────

TEST_CASE("build_fundamental_cycles - 4-bus square + diagonal: 2 cycles")
{
  // 0 — 1
  // | \ |
  // 3 — 2
  // 5 edges, 4 buses → |L|−|B|+1 = 2 cycles
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 1, .bus_b = 2},
      {.line_index = 2, .bus_a = 2, .bus_b = 3},
      {.line_index = 3, .bus_a = 3, .bus_b = 0},
      {.line_index = 4, .bus_a = 0, .bus_b = 2},  // diagonal
  };
  const auto cycles = build_fundamental_cycles(4, edges);
  REQUIRE(cycles.size() == 2);
  for (const auto& c : cycles) {
    CHECK(is_closed_loop(c, edges));
  }
  // Each fundamental cycle uses ≥ 3 edges.
  for (const auto& c : cycles) {
    CHECK(c.size() >= 3);
  }
}

TEST_CASE("build_fundamental_cycles - pentagon + chord: 2 cycles")
{
  // 0 — 1 — 2 — 3 — 4 — 0 ; chord 1—3
  // 6 edges, 5 buses → 2 cycles
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 1, .bus_b = 2},
      {.line_index = 2, .bus_a = 2, .bus_b = 3},
      {.line_index = 3, .bus_a = 3, .bus_b = 4},
      {.line_index = 4, .bus_a = 4, .bus_b = 0},
      {.line_index = 5, .bus_a = 1, .bus_b = 3},  // chord
  };
  const auto cycles = build_fundamental_cycles(5, edges);
  REQUIRE(cycles.size() == 2);
  for (const auto& c : cycles) {
    CHECK(is_closed_loop(c, edges));
  }
}

// ── Multi-island graphs ───────────────────────────────────────────

TEST_CASE("build_fundamental_cycles - two disjoint triangles: 2 cycles")
{
  // Island A: 0—1—2—0 ; Island B: 3—4—5—3
  // |L|=6, |B|=6, k=2 islands → 6 − 6 + 2 = 2 cycles
  const std::vector<Edge> edges = {
      // Island A
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 1, .bus_b = 2},
      {.line_index = 2, .bus_a = 2, .bus_b = 0},
      // Island B
      {.line_index = 3, .bus_a = 3, .bus_b = 4},
      {.line_index = 4, .bus_a = 4, .bus_b = 5},
      {.line_index = 5, .bus_a = 5, .bus_b = 3},
  };
  const auto cycles = build_fundamental_cycles(6, edges);
  REQUIRE(cycles.size() == 2);
  for (const auto& c : cycles) {
    CHECK(c.size() == 3);
    CHECK(is_closed_loop(c, edges));
  }
  // Cycles must not mix islands: one cycle uses {0,1,2}, the other
  // uses {3,4,5}.
  std::multiset<std::size_t> cycle1 = cycle_edge_set(cycles[0]);
  std::multiset<std::size_t> cycle2 = cycle_edge_set(cycles[1]);
  const std::multiset<std::size_t> island_a {0, 1, 2};
  const std::multiset<std::size_t> island_b {3, 4, 5};
  CHECK(((cycle1 == island_a && cycle2 == island_b)
         || (cycle1 == island_b && cycle2 == island_a)));
}

TEST_CASE("build_fundamental_cycles - tree forest (multi-island, no cycles)")
{
  // Island A: 0 — 1 ; Island B: 2 — 3 — 4
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 2, .bus_b = 3},
      {.line_index = 2, .bus_a = 3, .bus_b = 4},
  };
  const auto cycles = build_fundamental_cycles(5, edges);
  CHECK(cycles.empty());
}

// ── Sign-correctness regression ───────────────────────────────────

TEST_CASE("build_fundamental_cycles - sign convention matches bus_a→bus_b")
{
  // Triangle with a deliberately reversed edge:
  //   edge 0: 0 → 1 (bus_a=0, bus_b=1)
  //   edge 1: 2 → 1 (bus_a=2, bus_b=1)   ← reversed orientation
  //   edge 2: 0 → 2 (bus_a=0, bus_b=2)
  // Walking the cycle 0 → 1 → 2 → 0:
  //   - edge 0 traversed bus_a→bus_b ⇒ sign +1
  //   - edge 1 traversed bus_b→bus_a ⇒ sign −1
  //   - edge 2 traversed bus_b→bus_a ⇒ sign −1
  // (BFS may pick a different traversal direction; the test checks
  // that the resulting signs leave the cycle closed under signed
  // edge traversal — a stronger and orientation-agnostic invariant.)
  const std::vector<Edge> edges = {
      {.line_index = 0, .bus_a = 0, .bus_b = 1},
      {.line_index = 1, .bus_a = 2, .bus_b = 1},
      {.line_index = 2, .bus_a = 0, .bus_b = 2},
  };
  const auto cycles = build_fundamental_cycles(3, edges);
  REQUIRE(cycles.size() == 1);
  CHECK(cycles.front().size() == 3);
  CHECK(is_closed_loop(cycles.front(), edges));
  // Each sign must be ±1.
  for (const auto& ce : cycles.front()) {
    CHECK((ce.sign == +1 || ce.sign == -1));
  }
}
