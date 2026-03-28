/**
 * @file      bus_island.hpp
 * @brief     Connected-component (island) detection for bus networks
 * @date      Fri Mar 28 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a lightweight Union-Find (Disjoint Set Union) algorithm to
 * identify electrically connected islands in a bus network.  When
 * Kirchhoff (DC power-flow) constraints are enabled each island needs
 * its own reference bus with a fixed voltage angle; otherwise the LP
 * angles in that island are indeterminate and dual values (LMPs) become
 * unreliable.
 *
 * The detection runs at LP-build time over the static network topology
 * (all buses and lines) and automatically assigns `reference_theta = 0`
 * to one bus per island that does not already have a user-specified
 * reference.
 */

#pragma once

#include <cstddef>
#include <vector>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

// Forward declarations — only pointers/references are used in the API.
struct Bus;
struct Line;
class PlanningOptionsLP;

/**
 * @brief Disjoint Set Union (Union-Find) with path compression and union
 *        by rank.
 *
 * Each element is identified by a contiguous index in `[0, n)`.
 * All operations are amortised O(α(n)) ≈ O(1).
 */
class DisjointSetUnion
{
public:
  /// Construct a DSU with @p n singleton sets.
  explicit DisjointSetUnion(std::size_t n);

  /// Find the representative (root) of the set containing @p x.
  [[nodiscard]] auto find(std::size_t x) -> std::size_t;

  /// Merge the sets containing @p x and @p y.
  /// @return `true` if the two sets were different (and have been merged).
  auto unite(std::size_t x, std::size_t y) -> bool;

  /// Number of elements.
  [[nodiscard]] auto size() const noexcept -> std::size_t
  {
    return parent_.size();
  }

private:
  std::vector<std::size_t> parent_;
  std::vector<std::size_t> rank_;
};

/**
 * @brief Detect bus islands and assign a reference bus per island.
 *
 * Builds a Union-Find over buses connected by lines that have a
 * non-null reactance (i.e. lines participating in Kirchhoff constraints).
 * For each resulting connected component (island) that lacks a
 * user-specified `reference_theta`, the first bus in that island is
 * assigned `reference_theta = 0`.
 *
 * Self-loop lines (`bus_a == bus_b`) are silently ignored.
 *
 * When Kirchhoff is disabled or only one bus exists, this function is a
 * no-op and returns 0.
 *
 * @param buses   Mutable bus array — reference_theta may be set.
 * @param lines   Transmission lines (read-only).
 * @param options Resolved planning options.
 * @return Number of islands detected (0 when Kirchhoff is disabled).
 */
auto detect_islands_and_fix_references(Array<Bus>& buses,
                                       const Array<Line>& lines,
                                       const PlanningOptionsLP& options)
    -> std::size_t;

}  // namespace gtopt
