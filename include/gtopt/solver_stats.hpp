/**
 * @file      solver_stats.hpp
 * @brief     Aggregated solver-activity counters for end-of-run reporting.
 * @date      2026-04-15
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * One `SolverStats` lives inside every `LinearInterface`.  Counters are
 * single-writer (the thread that owns the LP), so plain integers suffice —
 * no atomics.  After all solves have joined, `gtopt_lp_runner` aggregates
 * across all `(scene, phase)` LPs via `operator+=` and prints a single
 * summary to the info stream.
 */

#pragma once

#include <algorithm>
#include <cstddef>

namespace gtopt
{

/// Per-LP counters tracking backend activity over the lifetime of the LP.
struct SolverStats
{
  /// Number of times `LinearInterface::load_flat()` invoked the plugin's
  /// `load_problem()`.  Exactly 1 per `(scene, phase)` when
  /// `low_memory_mode == off`; typically `iterations × phases` when a
  /// low-memory mode is active (backend rebuilt before every solve).
  std::size_t load_problem_calls {0};

  /// Top-level `initial_solve()` invocations (excludes fallback retries).
  std::size_t initial_solve_calls {0};
  /// Top-level `resolve()` invocations (excludes fallback retries and
  /// the lazy-crossover re-solve performed by `ensure_duals()`).
  std::size_t resolve_calls {0};
  /// Internal retries inside the algorithm-fallback loop (non-optimal
  /// primary attempts that triggered a secondary algorithm).
  std::size_t fallback_solves {0};
  /// Lazy-crossover re-solves triggered by `ensure_duals()` after a
  /// barrier-without-crossover solve.
  std::size_t crossover_solves {0};

  /// Solves that returned non-optimal even after the fallback cycle.
  std::size_t infeasible_count {0};
  /// Subset of `infeasible_count` classified as primal-infeasible.
  std::size_t primal_infeasible {0};
  /// Subset of `infeasible_count` classified as dual-infeasible.
  std::size_t dual_infeasible {0};

  /// Wall-clock seconds spent inside `m_backend_->initial_solve()` /
  /// `resolve()` (accumulated across every attempt, including fallbacks
  /// and crossover).  Excludes LP build and output time.
  double total_solve_time_s {0.0};
  /// Deterministic solver work (backend `SolveEffort::ticks`, e.g. CPLEX
  /// deterministic ticks) accumulated across every `initial_solve()` /
  /// `resolve()` on this LP.  When the backend reports native ticks this is
  /// load-independent — unlike `total_solve_time_s` it does not vary with
  /// machine contention — so it is the right measure for comparing
  /// algorithmic effort (e.g. feasibility-cut modes).  IMPORTANT: backends
  /// without a native tick count (CLP/CBC) fall back to wall seconds here,
  /// which are NOT load-independent; interpret this as deterministic ticks
  /// only when the active backend is known to report them (CPLEX/HiGHS).
  double total_solve_ticks {0.0};
  /// Largest kappa observed across every solve on this LP.  Stays at
  /// `-1.0` when the backend never reports a valid condition number.
  double max_kappa {-1.0};

  /// Sum of column counts observed at each top-level solve (one sample
  /// per `initial_solve()` / `resolve()`).  Divide by `total_solve_calls()`
  /// to recover the average LP size.  Tracking the sum instead of the
  /// average makes cross-LP aggregation a simple `+=`.
  std::size_t total_ncols {0};
  /// Sum of row counts observed at each top-level solve.
  std::size_t total_nrows {0};

  // ── SDDP backward-step timers ─────────────────────────────────────────
  //
  // Populated by `SDDPMethod::backward_pass_single_phase` (one sample per
  // cut it installs on this LP).  All six `bwd_*_s` fields are wall-clock
  // seconds accumulated across every step that landed on this LP; sum
  // them across all `(scene, phase)` LPs to break down the global
  // backward-pass wall time into its stages, or diff snapshots between
  // iterations to see whether individual stages grow as cuts accumulate.
  //
  // The instrumentation is always on — each step contributes six
  // `chrono::steady_clock::now()` pairs, which is negligible next to the
  // LP resolve it wraps (O(µs) vs O(s)).

  /// Number of backward-pass cut-installation steps that landed on this LP.
  std::size_t bwd_step_count {0};
  /// Time spent in `SystemLP::ensure_lp_built()` before the cut is added
  /// (snapshot/compress reload; 0 under `low_memory_mode=off`).
  double bwd_lp_rebuild_s {0.0};
  /// Time spent constructing the cut row (`build_benders_cut`,
  /// `rescale_benders_cut`, `filter_cut_coefficients`).  Pure CPU on the
  /// calling thread.
  double bwd_cut_build_s {0.0};
  /// Time spent in `LinearInterface::add_row(cut)` — this is the single-row
  /// CPLEX `addRow` (or equivalent) and the row-name bookkeeping that
  /// accompanies it.
  double bwd_add_row_s {0.0};
  /// Time spent pushing the new `StoredCut` onto the per-scene vector
  /// (`SDDPCutManager::store_cut`).
  double bwd_store_cut_s {0.0};
  /// Time spent in the post-cut `LinearInterface::resolve()` (simplex
  /// warm-start after adding the new row).  Distinct from the existing
  /// `total_solve_time_s`, which covers every `initial_solve`/`resolve`
  /// invocation on the LP, so the two overlap intentionally; diffing
  /// this in isolation shows the per-iteration backward-resolve cost.
  double bwd_resolve_s {0.0};
  /// Time spent in `SDDPMethod::update_max_kappa` — dominated by the
  /// backend `get_kappa()` call (CPLEX `CPXgetdblquality(CPX_KAPPA)`,
  /// HiGHS basis-condition query).
  double bwd_kappa_s {0.0};

  // ── Convergence indicators ───────────────────────────────────────────
  //
  // Sums of LP slack-variable values, accumulated across every top-level
  // `resolve()` on this LP whose caller invokes
  // `SystemLP::accumulate_convergence_indicators(scene, phase)`
  // (currently the SDDP forward pass).  Diff two snapshots via
  // `operator-=` to obtain the values produced by the most recent
  // iteration — that delta is the actual convergence signal and should
  // drop toward zero as the SDDP cuts mature.  All four are scenario-
  // probability-weighted and (for the time-extensive demand/flow
  // families) block-duration-weighted, so values from different scenes,
  // phases or LPs combine via plain `+=`.

  /// Σ over (scenario, stage, block) of
  /// `prob × duration × Demand.fail`, units MWh.  Reflects load shed
  /// volume the LP could not avoid this iteration.
  double unserved_demand {0.0};
  /// Σ over (scenario, stage, block, right) of
  /// `prob × duration × FlowRight.fail × 3600/1e6`, units hm³.  Reflects
  /// irrigation/agreement flow that the LP could not deliver.
  double unserved_flow {0.0};
  /// Σ over (scenario, stage, reservoir) of
  /// `prob × Storage.soft_emin`, units hm³.  Reflects volume below the
  /// per-stage soft minimum (`soft_emin` configured on Reservoir/Battery).
  double soft_emin_deficit {0.0};
  /// Σ over (scenario, last-stage-of-last-phase, reservoir) of
  /// `prob × Storage.efin_slack`, units hm³.  Reflects the terminal-
  /// volume shortfall below `efin` when `efin_cost` is configured.
  double efin_deficit {0.0};

  [[nodiscard]] constexpr std::size_t total_solve_calls() const noexcept
  {
    return initial_solve_calls + resolve_calls;
  }

  [[nodiscard]] constexpr std::size_t total_backend_solves() const noexcept
  {
    return total_solve_calls() + fallback_solves + crossover_solves;
  }

  constexpr SolverStats& operator+=(const SolverStats& rhs) noexcept
  {
    load_problem_calls += rhs.load_problem_calls;
    initial_solve_calls += rhs.initial_solve_calls;
    resolve_calls += rhs.resolve_calls;
    fallback_solves += rhs.fallback_solves;
    crossover_solves += rhs.crossover_solves;
    infeasible_count += rhs.infeasible_count;
    primal_infeasible += rhs.primal_infeasible;
    dual_infeasible += rhs.dual_infeasible;
    total_solve_time_s += rhs.total_solve_time_s;
    total_solve_ticks += rhs.total_solve_ticks;
    max_kappa = std::max(max_kappa, rhs.max_kappa);
    total_ncols += rhs.total_ncols;
    total_nrows += rhs.total_nrows;
    bwd_step_count += rhs.bwd_step_count;
    bwd_lp_rebuild_s += rhs.bwd_lp_rebuild_s;
    bwd_cut_build_s += rhs.bwd_cut_build_s;
    bwd_add_row_s += rhs.bwd_add_row_s;
    bwd_store_cut_s += rhs.bwd_store_cut_s;
    bwd_resolve_s += rhs.bwd_resolve_s;
    bwd_kappa_s += rhs.bwd_kappa_s;
    unserved_demand += rhs.unserved_demand;
    unserved_flow += rhs.unserved_flow;
    soft_emin_deficit += rhs.soft_emin_deficit;
    efin_deficit += rhs.efin_deficit;
    return *this;
  }

  /// Subtract a snapshot of counters — used to obtain per-iteration
  /// deltas from two consecutive snapshots of the same aggregated
  /// `SolverStats`.  `max_kappa` is copied from @p rhs (not differenced)
  /// because "max seen so far" has no meaningful subtraction; the delta
  /// snapshot holds the *post-iteration* max, which monotonically grows.
  constexpr SolverStats& operator-=(const SolverStats& rhs) noexcept
  {
    load_problem_calls -= rhs.load_problem_calls;
    initial_solve_calls -= rhs.initial_solve_calls;
    resolve_calls -= rhs.resolve_calls;
    fallback_solves -= rhs.fallback_solves;
    crossover_solves -= rhs.crossover_solves;
    infeasible_count -= rhs.infeasible_count;
    primal_infeasible -= rhs.primal_infeasible;
    dual_infeasible -= rhs.dual_infeasible;
    total_solve_time_s -= rhs.total_solve_time_s;
    total_solve_ticks -= rhs.total_solve_ticks;
    // max_kappa deliberately not subtracted (see above).
    total_ncols -= rhs.total_ncols;
    total_nrows -= rhs.total_nrows;
    bwd_step_count -= rhs.bwd_step_count;
    bwd_lp_rebuild_s -= rhs.bwd_lp_rebuild_s;
    bwd_cut_build_s -= rhs.bwd_cut_build_s;
    bwd_add_row_s -= rhs.bwd_add_row_s;
    bwd_store_cut_s -= rhs.bwd_store_cut_s;
    bwd_resolve_s -= rhs.bwd_resolve_s;
    bwd_kappa_s -= rhs.bwd_kappa_s;
    unserved_demand -= rhs.unserved_demand;
    unserved_flow -= rhs.unserved_flow;
    soft_emin_deficit -= rhs.soft_emin_deficit;
    efin_deficit -= rhs.efin_deficit;
    return *this;
  }

  [[nodiscard]] constexpr double avg_ncols() const noexcept
  {
    const auto n = total_solve_calls();
    return n == 0 ? 0.0
                  : static_cast<double>(total_ncols) / static_cast<double>(n);
  }

  [[nodiscard]] constexpr double avg_nrows() const noexcept
  {
    const auto n = total_solve_calls();
    return n == 0 ? 0.0
                  : static_cast<double>(total_nrows) / static_cast<double>(n);
  }

  [[nodiscard]] friend constexpr SolverStats operator+(
      SolverStats lhs, const SolverStats& rhs) noexcept
  {
    lhs += rhs;
    return lhs;
  }

  constexpr void reset() noexcept { *this = {}; }
};

}  // namespace gtopt
