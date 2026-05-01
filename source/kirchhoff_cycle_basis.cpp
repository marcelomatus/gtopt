// SPDX-License-Identifier: BSD-3-Clause

#include <cassert>
#include <numbers>
#include <optional>
#include <queue>
#include <ranges>
#include <unordered_set>
#include <utility>

#include <gtopt/bus_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/kirchhoff_cycle_basis.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

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
      while (true) {
        const auto& p = parent_bus[cur];
        if (!p.has_value()) {
          break;
        }
        cur = *p;
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
      const auto& edge_idx_opt = parent_edge[from];
      if (!edge_idx_opt.has_value()) {
        continue;
      }
      const auto e_idx = *edge_idx_opt;
      const int sign = (edges[e_idx].bus_a == from) ? +1 : -1;
      cycle.push_back({
          .line_index = edges[e_idx].line_index,
          .sign = sign,
      });
    }

    // 3b. Walk LCA → v (reverse of v's path-to-LCA).  Each edge is
    //     traversed FROM parent TO the "to-bus".  Sign: +1 if the
    //     edge's bus_a is the parent (we walk bus_a → bus_b),
    //     −1 otherwise.  Equivalent: +1 if edge.bus_a != to-bus.
    for (const auto to_bus : v_to_lca | std::views::reverse) {
      const auto& edge_idx_opt = parent_edge[to_bus];
      if (!edge_idx_opt.has_value()) {
        continue;
      }
      const auto e_idx = *edge_idx_opt;
      const int sign = (edges[e_idx].bus_a == to_bus) ? -1 : +1;
      cycle.push_back({
          .line_index = edges[e_idx].line_index,
          .sign = sign,
      });
    }

    // 3c. Closing edge: v → u (the non-tree edge `e`).  Sign: +1 if
    //     edge.bus_a == v (we walk bus_a → bus_b), −1 otherwise.
    const int closing_sign = (edge.bus_a == v) ? +1 : -1;
    cycle.push_back({.line_index = edge.line_index, .sign = closing_sign});

    cycles.push_back(std::move(cycle));
  }

  return cycles;
}

// ── LP integration: cycle-basis KVL row assembler ────────────────────

namespace
{

/// Per-line scalars resolved at (scenario, stage) granularity.  Cached
/// once at the start of `add_kvl_rows` so the inner cycle loop can
/// stamp coefficients without re-reading schedules per block.
struct ResolvedLine
{
  const LineLP* line;  ///< owner; used for flow-col lookup
  double x_tau;  ///< τ · X / V², row coefficient on signed flow
  double phi_rad;  ///< phase-shift contribution to RHS
  bool active;  ///< false ⇒ skip in cycle building
};

}  // namespace

std::size_t add_kvl_rows(const SystemContext& sc,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         LinearProblem& lp,
                         const Collection<BusLP>& buses,
                         const Collection<LineLP>& lines)
{
  const auto& line_elements = lines.elements();
  if (line_elements.empty()) {
    return 0;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return 0;
  }

  // ── Row-scale factor ───────────────────────────────────────────────
  // PyPSA hardcodes `× 1e5` in `define_kirchhoff_voltage_constraints`
  // to push per-unit-reactance coefficients off the LP solver's
  // numerical noise floor.  gtopt instead uses the adaptive
  // `scale_theta = median(|x_τ|)` already computed by
  // `PlanningLP::auto_scale_theta` for the `node_angle` strategy:
  //
  //   row_scale = 1 / scale_theta   ≈   1 / median(|x_τ|)
  //
  // so cycle KVL coefficients land at `x_τ · row_scale` ≈ 1 for the
  // median line, matching the post-θ-col-scale magnitude of the
  // `node_angle` row exactly.  Cross-mode coefficient consistency.
  const double scale_theta_value = sc.options().scale_theta();
  const double row_scale =
      (scale_theta_value > 0.0) ? (1.0 / scale_theta_value) : 1.0;

  // ── Resolve per-line scalars + build the edge list ─────────────────
  std::vector<ResolvedLine> resolved;
  resolved.reserve(line_elements.size());

  std::vector<Edge> edges;
  edges.reserve(line_elements.size());

  for (std::size_t li = 0; li < line_elements.size(); ++li) {
    const auto& line = line_elements[li];
    ResolvedLine rl {
        .line = &line,
        .x_tau = 0.0,
        .phi_rad = 0.0,
        .active = false,
    };

    if (!line.is_active(stage) || line.is_loop()) {
      resolved.push_back(rl);
      continue;
    }

    // `param_reactance` is a member fn that already returns
    // `OptReal` (= reactance.at(stage.uid())).  No need to go through
    // `sc.stage_reactance` here — cycle_basis is only called when
    // use_kirchhoff is true, so the gate inside `stage_reactance`
    // is irrelevant.
    if (!sc.options().use_kirchhoff()) {
      resolved.push_back(rl);
      continue;
    }
    const auto x_opt = line.param_reactance(stage.uid());
    if (!x_opt || x_opt.value() == 0.0) {
      resolved.push_back(rl);
      continue;
    }
    const double X = x_opt.value();
    const double V = line.param_voltage(stage.uid()).value_or(1.0);
    const double tau = line.param_tap_ratio(stage.uid()).value_or(1.0);
    const double x_tau = tau * X / (V * V);
    if (x_tau == 0.0) {
      resolved.push_back(rl);
      continue;
    }
    const double phi_deg =
        line.param_phase_shift_deg(stage.uid()).value_or(0.0);
    const double phi_rad = phi_deg * std::numbers::pi / 180.0;

    rl.x_tau = x_tau;
    rl.phi_rad = phi_rad;
    rl.active = true;

    // Resolve bus indices via Collection's uid/name maps.  Skipped
    // (line excluded from cycle topology) if either bus is unknown.
    try {
      const auto idx_a =
          static_cast<std::size_t>(buses.element_index(line.bus_a_sid()));
      const auto idx_b =
          static_cast<std::size_t>(buses.element_index(line.bus_b_sid()));
      edges.push_back(Edge {.line_index = li, .bus_a = idx_a, .bus_b = idx_b});
    } catch (const std::out_of_range&) {
      rl.active = false;
    }

    resolved.push_back(rl);
  }

  if (edges.empty()) {
    return 0;
  }

  // ── Build fundamental cycles ───────────────────────────────────────
  const auto cycles = build_fundamental_cycles(buses.elements().size(), edges);
  if (cycles.empty()) {
    return 0;
  }

  // ── Emit one KVL row per cycle per block ───────────────────────────
  // Pre-resolve `param_reactance` accessors are not invariant per
  // block, but `x_tau` IS (per stage), so the inner loop only varies
  // the per-block flow-col references.
  static constexpr std::string_view class_name = "Kirchhoff";
  static constexpr std::string_view constraint_name = "cycle";

  std::size_t total_rows = 0;
  std::size_t cycle_idx = 0;
  for (const auto& cycle : cycles) {
    for (const auto& block : blocks) {
      const auto buid = block.uid();

      double rhs = 0.0;
      auto row =
          SparseRow {
              .class_name = class_name,
              .constraint_name = constraint_name,
              .variable_uid = Uid {static_cast<int>(cycle_idx)},
              .context =
                  make_block_context(scenario.uid(), stage.uid(), block.uid()),
          }
              .equal(0.0);
      // Rough non-zero estimate: every cycle edge contributes 1
      // (aggregator) or K (segs) cols per direction.  Reserve the
      // larger of cycle.size() and 4 to cover the typical case.
      row.reserve(std::max<std::size_t>(cycle.size() * 2, 4));

      for (const auto& ce : cycle) {
        const auto& rl = resolved[ce.line_index];
        if (!rl.active) {
          continue;  // defensive — should not happen for cycle members
        }
        const double coef_base = ce.sign * rl.x_tau * row_scale;
        rhs += ce.sign * rl.phi_rad * row_scale;

        // Stamp flow cols.  In `piecewise_direct` line-loss mode the
        // flowp / flown aggregators are absent and segments stamp
        // directly with the same per-edge coefficient.
        const auto& fpc = rl.line->flowp_cols_at(scenario, stage);
        if (auto it = fpc.find(buid); it != fpc.end()) {
          row[it->second] += coef_base;
        }
        const auto& fnc = rl.line->flown_cols_at(scenario, stage);
        if (auto it = fnc.find(buid); it != fnc.end()) {
          row[it->second] -= coef_base;
        }
        const auto& fpsegc = rl.line->flowp_seg_cols_at(scenario, stage);
        if (auto it = fpsegc.find(buid); it != fpsegc.end()) {
          for (const auto& col : it->second) {
            row[col] += coef_base;
          }
        }
        const auto& fnsegc = rl.line->flown_seg_cols_at(scenario, stage);
        if (auto it = fnsegc.find(buid); it != fnsegc.end()) {
          for (const auto& col : it->second) {
            row[col] -= coef_base;
          }
        }
      }

      // Update RHS now that all edges have been summed.
      row.equal(-rhs);
      [[maybe_unused]] const auto row_idx = lp.add_row(std::move(row));
      ++total_rows;
    }
    ++cycle_idx;
  }

  spdlog::debug(
      "kirchhoff::cycle_basis: emitted {} KVL row(s) "
      "({} cycle(s) × {} block(s), scenario={}, stage={}, row_scale={:.6g})",
      total_rows,
      cycles.size(),
      blocks.size(),
      scenario.uid(),
      stage.uid(),
      row_scale);

  return total_rows;
}

}  // namespace gtopt::kirchhoff::cycle_basis
