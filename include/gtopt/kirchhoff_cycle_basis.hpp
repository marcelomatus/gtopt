/**
 * @file      kirchhoff_cycle_basis.hpp
 * @brief     Fundamental-cycle builder for the loop-flow KVL formulation
 * @date      2026-04-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Pure-graph utilities for the cycle-basis (loop-flow) Kirchhoff
 * Voltage Law formulation.  No LP / SystemContext / LineLP coupling
 * — this module knows about edges and buses only.  The PR 2b LP
 * integration layer (`kirchhoff::cycle_basis::add_kvl_rows`, system-
 * level dispatch in `system_lp.cpp`) consumes the output of
 * `build_fundamental_cycles` and stamps the per-cycle KVL rows.
 *
 * Algorithm: BFS spanning tree per connected component (island).
 * Each non-tree (co-tree) edge closes exactly one fundamental cycle —
 * the unique simple cycle obtained by adding that edge to the tree.
 * For a connected graph with `|B|` buses and `|L|` edges, this yields
 * `|L| − |B| + 1` cycles; with `k` islands, `|L| − |B| + k` cycles.
 *
 * The sign of each edge in a cycle is determined by traversal
 * orientation: `+1` if the cycle traversal direction matches the
 * edge's `bus_a → bus_b` orientation, `−1` otherwise.  This matches
 * PyPSA's `find_cycles` convention
 * (`pypsa/network/power_flow.py:find_cycles`).
 *
 * Parallel edges (two or more edges between the same pair of buses)
 * are handled correctly: BFS picks one as a tree edge; each remaining
 * parallel edge becomes a co-tree edge whose fundamental cycle is the
 * 2-edge loop formed with the tree edge.
 *
 * Reference: Hörsch, Hofmann, Schlachtberger, Brown,
 *            "PyPSA: Python for Power System Analysis",
 *            J. Open Research Software 6(1), 2018.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

// BusLP / LineLP must be complete here so the `add_kvl_rows`
// signature can instantiate `Collection<BusLP>` and
// `Collection<LineLP>` (Collection's template constraints require
// the type to be complete).
#include <gtopt/bus_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/strong_index_vector.hpp>

namespace gtopt
{
class LinearProblem;
class ScenarioLP;
class StageLP;
class SystemContext;
}  // namespace gtopt

namespace gtopt::kirchhoff::cycle_basis
{

/// One undirected edge in the bus-line topology graph.  `bus_a` /
/// `bus_b` are 0-based bus indices; `line_index` is the caller's
/// opaque handle for the edge (typically an index into the active-line
/// array, used later to recover per-edge LP columns and metadata).
struct Edge
{
  std::size_t line_index;
  std::size_t bus_a;
  std::size_t bus_b;
};

/// One edge of one fundamental cycle: the original edge index plus the
/// traversal sign `+1` (bus_a → bus_b) or `−1` (bus_b → bus_a).
struct CycleEdge
{
  std::size_t line_index;
  int sign;  ///< +1 or −1
};

/// A fundamental cycle: an ordered list of edges in traversal order.
/// The first and last edges close the loop; the order matters for
/// sign-consistent KVL row assembly.
using Cycle = std::vector<CycleEdge>;

/// Build the fundamental cycle basis for an undirected multigraph.
///
/// @param num_buses  number of buses (vertices); bus indices are
///                   `[0, num_buses)`.
/// @param edges      input edges; bus indices must be `< num_buses`.
///                   Self-loops (`bus_a == bus_b`) are silently
///                   skipped — they do not contribute to KVL.
///
/// @return One fundamental cycle per non-tree edge.  For a connected
///         graph with `|L|` edges and `|B|` buses, `|L| − |B| + 1`
///         cycles; with `k` islands, `|L| − |B| + k`.  Empty when the
///         graph is a forest (no cycles).
[[nodiscard]] std::vector<Cycle> build_fundamental_cycles(
    std::size_t num_buses, std::span<const Edge> edges);

/// Per-edge OTS hookup used by `add_kvl_rows` when the cycle contains a
/// switchable line.  `u_col` is the binary status column (or its [0,1]
/// LP relaxation) introduced by `LineCommitmentLP::add_to_lp`.  The
/// caller is responsible for choosing a sufficiently loose per-line
/// `kvl_big_m` so the per-cycle bound `M_C` covers the worst-case |Δθ|
/// contribution of the cycle.  See the formulation document
/// (`docs/formulation/mathematical-formulation.md` §OTS) for the
/// derivation `M_C = 2·θ_max · |C| · row_scale + Σ |φ_e| · row_scale`.
struct SwitchableEdge
{
  ColIndex u_col;
};

/// Caller-supplied lookup that maps `(line_index, BlockUid)` → optional
/// switchable-edge info.  Returns `std::nullopt` when the line is not
/// a switching candidate at this block (the default for every line
/// when no `LineCommitment` rows exist).  Used by `add_kvl_rows` to
/// decide whether a cycle KVL row needs the big-M disjunctive form
/// (any switchable line in the cycle ⇒ disjunctive) or the standard
/// equality form (no switchable lines in the cycle ⇒ equality, the
/// pre-OTS behaviour).
using SwitchableLineLookup = std::function<std::optional<SwitchableEdge>(
    std::size_t line_index, BlockUid buid)>;

/// System-level KVL row assembler for the loop-flow formulation.
///
/// Called once per (scenario, stage) AFTER every `LineLP::add_to_lp`
/// has finished creating its flow vars.  Builds the cycle basis from
/// the active-line topology, then emits one KVL row per cycle per
/// block in the form
///
///   Σ_{l ∈ C} ε_l · x_τ_l · row_scale · (f_p_l − f_n_l)
///       =  Σ_{l ∈ C} ε_l · φ_l · row_scale
///
/// where `row_scale = 1 / scale_theta` (adaptive, mirrors PyPSA's
/// hardcoded `× 1e5`).  Using the same `scale_theta` value as the
/// `node_angle` strategy gives cross-mode coefficient consistency
/// before LP-layer row-max equilibration.
///
/// Skips lines that are inactive at this stage, are self-loops
/// (`bus_a == bus_b`), or have zero / missing `x_τ` (DC / HVDC).
///
/// When `switchable_lookup` is provided and at least one line in a
/// cycle is switchable at the current block, the equality row is
/// replaced by the OTS disjunctive form:
///
///   −Σ_{l ∈ C∩sw} M_C · (1 − u_l)
///       ≤  LHS − RHS_eq
///       ≤  +Σ_{l ∈ C∩sw} M_C · (1 − u_l)
///
/// with `M_C = 2·θ_max · |C| · row_scale + Σ_{e ∈ C} |φ_e| · row_scale`
/// (Fisher-style loose bound; iterative tightening is a follow-up).
/// At `u_l = 1 ∀ l ∈ C∩sw` the two inequalities collapse to the
/// original equality; at any `u_l = 0` the cycle slacks just enough to
/// let that branch open without distorting the surviving cycle.
///
/// @return The number of cycle KVL rows added (counts both halves of a
///         disjunctive pair as 2).
std::size_t add_kvl_rows(const SystemContext& sc,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         LinearProblem& lp,
                         const Collection<BusLP>& buses,
                         const Collection<LineLP>& lines,
                         const SwitchableLineLookup& switchable_lookup = {});

}  // namespace gtopt::kirchhoff::cycle_basis
