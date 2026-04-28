// SPDX-License-Identifier: BSD-3-Clause

#include <cassert>
#include <optional>
#include <queue>
#include <unordered_set>
#include <utility>

#include <gtopt/kirchhoff_cycle_basis.hpp>

namespace gtopt::kirchhoff::cycle_basis
{

namespace
{

/// Walk parent pointers from `start` up to (but not including) `stop`,
/// emitting each visited bus (except `start` itself when
/// `include_start = false`).  Used to materialise the
/// path-from-bus-to-LCA in the spanning tree.
[[nodiscard]] std::vector<std::size_t> walk_to_ancestor(
    std::size_t start,
    std::size_t stop,
    std::span<const std::optional<std::size_t>> parent_bus)
{
  std::vector<std::size_t> path;
  auto cur = start;
  while (cur != stop) {
    path.push_back(cur);
    const auto& p = parent_bus[cur];
    if (!p.has_value()) {
      // Should not happen if `stop` was found via `walk_up_collect`
      // — but guard against pathological inputs.
      break;
    }
    cur = *p;
  }
  return path;
}

}  // namespace

std::vector<Cycle> build_fundamental_cycles(std::size_t num_buses,
                                            std::span<const Edge> edges)
{
  if (num_buses == 0) {
    return {};
  }

  // 1. Build adjacency list.  For each bus, store (neighbor_bus,
  //    edge_index) pairs.  Self-loops are skipped — they cannot be
  //    part of a meaningful KVL cycle (θ_a − θ_a = 0 trivially).
  std::vector<std::vector<std::pair<std::size_t, std::size_t>>> adj(num_buses);
  for (std::size_t e = 0; e < edges.size(); ++e) {
    const auto& edge = edges[e];
    if (edge.bus_a == edge.bus_b) {
      continue;
    }
    assert(edge.bus_a < num_buses);
    assert(edge.bus_b < num_buses);
    adj[edge.bus_a].emplace_back(edge.bus_b, e);
    adj[edge.bus_b].emplace_back(edge.bus_a, e);
  }

  // 2. BFS spanning tree per island.  `parent_bus[v]` and
  //    `parent_edge[v]` are set when `v` is first discovered; they
  //    stay `nullopt` for the BFS root of each island.
  std::vector<std::optional<std::size_t>> parent_bus(num_buses);
  std::vector<std::optional<std::size_t>> parent_edge(num_buses);
  std::vector<bool> visited(num_buses, false);
  std::vector<bool> is_tree_edge(edges.size(), false);

  for (std::size_t root = 0; root < num_buses; ++root) {
    if (visited[root]) {
      continue;
    }
    visited[root] = true;
    std::queue<std::size_t> q;
    q.push(root);
    while (!q.empty()) {
      const auto u = q.front();
      q.pop();
      for (const auto& [v, e_idx] : adj[u]) {
        if (visited[v]) {
          continue;
        }
        visited[v] = true;
        parent_bus[v] = u;
        parent_edge[v] = e_idx;
        is_tree_edge[e_idx] = true;
        q.push(v);
      }
    }
  }

  // 3. For each non-tree edge, build the fundamental cycle:
  //    walk u → LCA, walk v → LCA (reversed), close with the edge.
  std::vector<Cycle> cycles;
  cycles.reserve(edges.size());  // rough upper bound

  for (std::size_t e = 0; e < edges.size(); ++e) {
    if (is_tree_edge[e]) {
      continue;
    }
    const auto& edge = edges[e];
    if (edge.bus_a == edge.bus_b) {
      continue;  // self-loop (already filtered, but defensive)
    }
    const auto u = edge.bus_a;
    const auto v = edge.bus_b;

    // Find LCA by collecting u's path-to-root, then walking v upward
    // until we hit a bus on u's path.
    std::unordered_set<std::size_t> u_ancestors;
    {
      auto cur = u;
      u_ancestors.insert(cur);
      while (parent_bus[cur].has_value()) {
        cur = *parent_bus[cur];
        u_ancestors.insert(cur);
      }
    }

    auto lca = v;
    while (!u_ancestors.contains(lca)) {
      const auto& p = parent_bus[lca];
      if (!p.has_value()) {
        // u and v are in different islands — should be impossible for
        // a non-tree edge whose endpoints are both visited by BFS.
        // Defensive: skip.
        break;
      }
      lca = *p;
    }
    if (!u_ancestors.contains(lca)) {
      continue;  // pathological — skip this edge
    }

    const auto u_to_lca = walk_to_ancestor(u, lca, parent_bus);
    const auto v_to_lca = walk_to_ancestor(v, lca, parent_bus);

    Cycle cycle;
    cycle.reserve(u_to_lca.size() + v_to_lca.size() + 1);

    // 3a. Walk u → LCA along tree edges.  Each edge is traversed
    //     FROM the "from-bus" TO its parent.  Sign: +1 if the edge's
    //     bus_a is the "from-bus" (i.e., we walk bus_a → bus_b),
    //     −1 otherwise.
    for (const auto from : u_to_lca) {
      const auto e_idx = *parent_edge[from];
      const int sign = (edges[e_idx].bus_a == from) ? +1 : -1;
      cycle.push_back({.line_index = edges[e_idx].line_index, .sign = sign});
    }

    // 3b. Walk LCA → v (reverse of v's path-to-LCA).  Each edge is
    //     traversed FROM parent TO the "to-bus".  Sign: +1 if the
    //     edge's bus_a is the parent (we walk bus_a → bus_b),
    //     −1 otherwise.  Equivalent: +1 if edge.bus_a != to-bus.
    for (auto it = v_to_lca.rbegin(); it != v_to_lca.rend(); ++it) {
      const auto to_bus = *it;
      const auto e_idx = *parent_edge[to_bus];
      const int sign = (edges[e_idx].bus_a == to_bus) ? -1 : +1;
      cycle.push_back({.line_index = edges[e_idx].line_index, .sign = sign});
    }

    // 3c. Closing edge: v → u (the non-tree edge `e`).  Sign: +1 if
    //     edge.bus_a == v (we walk bus_a → bus_b), −1 otherwise.
    const int closing_sign = (edge.bus_a == v) ? +1 : -1;
    cycle.push_back({.line_index = edge.line_index, .sign = closing_sign});

    cycles.push_back(std::move(cycle));
  }

  return cycles;
}

}  // namespace gtopt::kirchhoff::cycle_basis
