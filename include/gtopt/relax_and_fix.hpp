/**
 * @file      relax_and_fix.hpp
 * @brief     Relax-and-fix rolling-window integrality for monolithic UC MIPs
 * @date      2026-07-14
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * Relax-and-fix is the classical rolling-window MIP heuristic for unit
 * commitment (Wolsey, "Integer Programming", §12.5; Pochet & Wolsey 2006):
 * the model stays WHOLE — every constraint row, including the reservoir /
 * storage intertemporal coupling, is in the LP for every solve — but the
 * commitment binaries are integer only inside a rolling time window.
 *
 *   window k solve:   [ fixed 0/1 | integer (overlap + window k) | relaxed ]
 *                       past          active                        future
 *
 * After window k's MIP, the active binaries that leave the window (all but
 * the trailing `overlap_hours`) are FIXED via bounds to their solved 0/1
 * values, and the next window's binaries become integer.  The FINAL window's
 * solve — every past window pinned, the last window integer — is a genuine
 * MIP solve whose incumbent is the returned solution, so the backend ends in
 * the exact state a plain MIP solve leaves it in (integrality flags intact,
 * incumbent published) and the normal output / dual-recovery / dump paths
 * apply unchanged.
 *
 * Complexity lever: a weekly UC with a 24 h window solves ~7 MIPs with ~7×
 * fewer FREE binaries each — much smaller branch-and-bound trees.  The
 * result is integer-feasible and its objective is an UPPER bound on the true
 * MIP optimum (relax-and-fix is a heuristic; the relaxed future makes each
 * window's view of what follows optimistic but never infeasible).
 *
 * Failure containment: if any window MIP fails (infeasible after a bad early
 * fix, solver error), the orchestrator restores every pinned bound and every
 * integrality flag and falls back to the plain full MIP — relax-and-fix can
 * lose time, never a solution.
 *
 * Backend support: cplex / scip / highs / mindopt drive the in-place rolling
 * loop.  CBC's OsiCbc backend cannot re-solve a mutated MIP in place, so it is
 * gated to a single plain full-MIP solve (correct, just not accelerated).
 */

#pragma once

#include <expected>
#include <span>
#include <unordered_map>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/domain_rules.hpp>
#include <gtopt/error.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

class LinearInterface;

/// One window-partitionable integer column: its RAW LP index plus the
/// chronological start hour of the (first) block its commitment period
/// covers — the key the rolling window partitions on.
struct RelaxAndFixCol
{
  int col {};
  double start_hour {};
};

/// Windowing parameters (hours).  `window_hours <= 0` disables the rolling
/// loop (the solve degrades to the plain MIP `li.resolve`).
struct RelaxAndFixWindowOptions
{
  double window_hours {};
  double overlap_hours {};
};

/// Per-run summary (logging / tests).
struct RelaxAndFixReport
{
  /// Solver status of the FINAL solve (the plain-MIP status when the rolling
  /// loop was skipped or fell back).
  int status {};
  /// Number of window MIPs solved (1 = plain MIP, no rolling).
  int windows {1};
  /// Windowed integer columns partitioned across the rolling windows.
  int window_cols {0};
  /// A window MIP failed and the solve fell back to the plain full MIP.
  bool fallback {false};
};

/// Solve `li` (built once, never rebuilt) by relax-and-fix rolling-window
/// integrality over the `cols` partition; plain `li.resolve(solve_opts)`
/// when `cols` is empty, `opts.window_hours <= 0`, or a single window spans
/// the whole horizon.  Duplicate column entries are tolerated (a commitment
/// period column may be listed once per member block) and are assigned the
/// EARLIEST listed start hour.
[[nodiscard]] std::expected<RelaxAndFixReport, Error> solve_relax_and_fix(
    LinearInterface& li,
    const SolverOptions& solve_opts,
    std::span<const RelaxAndFixCol> cols,
    const RelaxAndFixWindowOptions& opts);

/// Build the window-partition input from the per-(scenario, commitment)
/// status-column run infos and the chronological per-block start hours
/// (block uid → cumulative hours before the block).  Columns whose block uid
/// is missing from `block_start_hours` are skipped (they stay plain integer
/// through every window solve).
[[nodiscard]] std::vector<RelaxAndFixCol> make_relax_and_fix_cols(
    std::span<const CommitmentRunInfo> commitments,
    const std::unordered_map<Uid, double>& block_start_hours);

}  // namespace gtopt
