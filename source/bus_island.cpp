/**
 * @file      bus_island.cpp
 * @brief     Connected-component (island) detection for bus networks
 * @date      Fri Mar 28 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <limits>
#include <numeric>
#include <ranges>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>

#include <gtopt/bus.hpp>
#include <gtopt/bus_island.hpp>
#include <gtopt/line.hpp>
#include <gtopt/map_reserve.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ── DisjointSetUnion ──────────────────────────────────────────────────

DisjointSetUnion::DisjointSetUnion(std::size_t n)
    : parent_(n)
    , rank_(n, 0)
{
  std::ranges::iota(parent_, std::size_t {0});
}

auto DisjointSetUnion::find(std::size_t x) -> std::size_t
{
  while (parent_[x] != x) {
    parent_[x] = parent_[parent_[x]];  // path splitting
    x = parent_[x];
  }
  return x;
}

auto DisjointSetUnion::unite(std::size_t x, std::size_t y) -> bool
{
  x = find(x);
  y = find(y);
  if (x == y) {
    return false;
  }
  // Union by rank
  if (rank_[x] < rank_[y]) {
    std::swap(x, y);
  }
  parent_[y] = x;
  if (rank_[x] == rank_[y]) {
    ++rank_[x];
  }
  return true;
}

// ── Island detection ──────────────────────────────────────────────────

auto detect_islands_and_fix_references(Array<Bus>& buses,
                                       const Array<Line>& lines,
                                       const PlanningOptionsLP& options)
    -> std::size_t
{
  // Early-out: no Kirchhoff → no angle variables → no islands to detect
  if (buses.size() <= 1 || options.use_single_bus() || !options.use_kirchhoff())
  {
    return 0;
  }

  const auto kirchhoff_threshold = options.kirchhoff_threshold();

  // Build UID → index and Name → index mappings for bus lookup.
  // Use a hash map (not a dense vector sized by max-uid) because UIDs are
  // arbitrary user-facing identifiers and can be sparse or very large —
  // indexing a vector by UID risks huge over-allocation or OOM.
  const auto num_buses = buses.size();
  constexpr auto sentinel = std::numeric_limits<std::size_t>::max();

  std::unordered_map<Uid, std::size_t> uid_to_index;
  std::unordered_map<std::string, std::size_t> name_to_index;
  map_reserve(uid_to_index, num_buses);
  map_reserve(name_to_index, num_buses);

  for (auto&& [idx, bus] : std::views::enumerate(buses)) {
    const auto i = static_cast<std::size_t>(idx);
    uid_to_index[bus.uid] = i;
    if (!bus.name.empty()) {
      name_to_index[bus.name] = i;
    }
  }

  // Resolve a SingleId (uid or name) to a bus index
  const auto resolve = [&](const SingleId& sid) -> std::size_t
  {
    return std::visit(
        [&](const auto& v) -> std::size_t
        {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, Uid>) {
            const auto it = uid_to_index.find(v);
            return it != uid_to_index.end() ? it->second : sentinel;
          } else {
            const auto it = name_to_index.find(v);
            return it != name_to_index.end() ? it->second : sentinel;
          }
        },
        sid);
  };

  // Build union-find over Kirchhoff-eligible buses connected by lines
  DisjointSetUnion dsu(num_buses);

  for (const auto& line : lines) {
    // Skip self-loops
    if (line.bus_a == line.bus_b) {
      continue;
    }
    // Skip lines without reactance (no Kirchhoff contribution)
    if (!line.reactance.has_value()) {
      continue;
    }
    const auto idx_a = resolve(line.bus_a);
    const auto idx_b = resolve(line.bus_b);

    // Skip if either bus is not in the bus array
    if (idx_a == sentinel || idx_b == sentinel) {
      continue;
    }
    dsu.unite(idx_a, idx_b);
  }

  // Group buses by their island root
  //   island_roots: set of unique root indices
  //   For each root, check if any bus already has reference_theta
  std::set<std::size_t> island_roots;
  for (const auto i : iota_range(std::size_t {0}, num_buses)) {
    island_roots.insert(dsu.find(i));
  }

  const auto num_islands = island_roots.size();

  if (num_islands <= 1) {
    // Single island: fall back to the simple logic — set first
    // Kirchhoff-eligible bus as reference if none is already set
    const bool has_reference = std::ranges::any_of(
        buses, [](const auto& b) { return b.reference_theta.has_value(); });

    if (!has_reference) {
      const bool any_kirchhoff = std::ranges::any_of(
          buses,
          [kirchhoff_threshold](const auto& b)
          { return b.needs_kirchhoff(kirchhoff_threshold); });

      if (any_kirchhoff) {
        buses.front().reference_theta = 0.0;
        SPDLOG_DEBUG(
            "Island 0: setting bus '{}' as reference "
            "(reference_theta=0)",
            buses.front().name);
      }
    }
    return num_islands;
  }

  // Multiple islands detected — indent two spaces to match the surrounding
  // "Building LP model" section hierarchy.
  SPDLOG_INFO("  Network has {} islands", num_islands);

  // For each island, ensure at least one reference bus
  for (const auto root : island_roots) {
    // Collect bus indices in this island
    bool has_reference = false;
    std::size_t first_kirchhoff_idx = num_buses;  // sentinel

    for (const auto i : iota_range(std::size_t {0}, num_buses)) {
      if (dsu.find(i) != root) {
        continue;
      }
      if (buses[i].reference_theta.has_value()) {
        has_reference = true;
        break;
      }
      if (first_kirchhoff_idx == num_buses
          && buses[i].needs_kirchhoff(kirchhoff_threshold))
      {
        first_kirchhoff_idx = i;
      }
    }

    if (!has_reference && first_kirchhoff_idx < num_buses) {
      buses[first_kirchhoff_idx].reference_theta = 0.0;
      SPDLOG_DEBUG(
          "Island (root {}): setting bus '{}' as reference "
          "(reference_theta=0)",
          root,
          buses[first_kirchhoff_idx].name);
    }
  }

  return num_islands;
}

}  // namespace gtopt
