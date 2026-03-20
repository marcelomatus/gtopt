/**
 * @file      benders_cut.hpp
 * @brief     Modular Benders cut construction and handling
 * @date      2026-03-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module extracts the cut-creation logic from the SDDP solver into
 * standalone, testable functions and a class-based interface.
 *
 * ## Free functions
 *
 * - `build_benders_cut()`        – standard optimality cut from reduced costs
 * - `relax_fixed_state_variable()` – elastic-filter column relaxation
 * - `build_feasibility_cut()`    – clone → relax → solve → extract cut
 * - `build_multi_cuts()`         – per-slack bound-constraint cuts
 * - `average_benders_cut()`      – unweighted average of several cuts
 * - `weighted_average_benders_cut()` – probability-weighted average
 *
 * ## BendersCut class
 *
 * `BendersCut` wraps the free functions as member functions and adds:
 * - An optional `AdaptiveWorkPool` for LP solve/resolve operations.
 *   When a pool is provided, the elastic-filter LP solve is submitted to
 *   the pool; otherwise it is performed synchronously.
 * - An infeasible-cut counter: every successful elastic-filter solve
 *   (i.e. every LP infeasibility event handled by the filter) is counted.
 *   The counter can be queried for monitoring-API integration.
 *
 * The SDDP solver (`sddp_solver.hpp`) re-exports the free-function symbols
 * so that existing code that includes `sddp_solver.hpp` continues to compile
 * without changes.
 */

#pragma once

#include <atomic>
#include <span>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_row.hpp>

// Forward declaration to avoid including the heavy work_pool.hpp header.
// Consumers that need the full AdaptiveWorkPool definition (e.g. to call
// make_solver_work_pool) should include <gtopt/work_pool.hpp> directly.
namespace gtopt
{
class AdaptiveWorkPool;
}

namespace gtopt
{

// ─── State variable linkage ─────────────────────────────────────────────────

/**
 * @brief Describes one state-variable linkage between consecutive phases
 *
 * A state variable has a *source* column in phase t (e.g. efin, capainst)
 * and a *dependent* column in phase t+1 (e.g. eini, capainst_ini).  The
 * source column's physical bounds are captured at initialisation time so
 * the elastic filter can relax to the correct domain.
 */
struct StateVarLink
{
  ColIndex source_col {};  ///< Source column in source phase's LP
  ColIndex dependent_col {};  ///< Dependent column in target phase's LP
  PhaseIndex source_phase {};  ///< Phase where the source column lives
  PhaseIndex target_phase {};  ///< Phase where the dependent column lives
  double trial_value {0.0};  ///< Trial value from the last forward pass
  double source_low {0.0};  ///< Physical lower bound of source column
  double source_upp {0.0};  ///< Physical upper bound of source column
};

// ─── Elastic relaxation result ──────────────────────────────────────────────

/// Result of relaxing one state-variable column via the elastic filter.
/// Contains the relaxation status and the indices of the penalised slack
/// columns added to the LP.
struct RelaxedVarInfo
{
  bool relaxed {false};  ///< True if the column was relaxed (was fixed)
  /// Overshoot slack col (sup > 0 → solution < trial)
  ColIndex sup_col {unknown_index};
  /// Undershoot slack col (sdn > 0 → solution > trial)
  ColIndex sdn_col {unknown_index};

  /// Implicit bool conversion: true iff the column was relaxed.
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  operator bool() const noexcept { return relaxed; }
};

// ─── Optimality cut ─────────────────────────────────────────────────────────

/// Propagate trial values: fix dependent columns to source-column solution
void propagate_trial_values(std::span<StateVarLink> links,
                            std::span<const double> source_solution,
                            LinearInterface& target_li) noexcept;

/// Build a Benders optimality cut from reduced costs of dependent columns.
///
///   α_{t-1} ≥ z_t + Σ_i rc_i · (x_{t-1,i} − v̂_i)
///
/// Returns the cut as a SparseRow ready to add to the source phase.
[[nodiscard]] auto build_benders_cut(ColIndex alpha_col,
                                     std::span<const StateVarLink> links,
                                     std::span<const double> reduced_costs,
                                     double objective_value,
                                     std::string_view name) -> SparseRow;

// ─── Elastic filter ─────────────────────────────────────────────────────────

/// Relax a single fixed state-variable column to its physical source bounds,
/// adding penalised slack variables.
/// Returns a RelaxedVarInfo with the relaxation status and slack column
/// indices.  Converts to bool (true iff relaxed) for backward compatibility.
[[nodiscard]] RelaxedVarInfo relax_fixed_state_variable(
    LinearInterface& li,
    const StateVarLink& link,
    PhaseIndex phase,
    double penalty);

/// Result of the elastic-filter clone–solve step.
/// Contains the solved LP clone and per-link slack column information.
struct ElasticSolveResult
{
  LinearInterface clone;  ///< Solved elastic clone
  /// One RelaxedVarInfo per outgoing link (same order as @p links)
  std::vector<RelaxedVarInfo> link_infos {};
};

/// Clone the LP, apply elastic relaxation on fixed state-variable columns,
/// and solve the clone.  The original LP is never modified.
///
/// @param li       The LP to clone (not modified)
/// @param links    Outgoing state-variable links from the previous phase
/// @param penalty  Elastic penalty coefficient for slack variables
/// @param opts     Solver options for the clone solve
/// @return Solved elastic clone and per-link slack info, or nullopt if
///         no columns were fixed or the clone solve failed.
[[nodiscard]] auto elastic_filter_solve(
    const LinearInterface& li,
    std::span<const StateVarLink> links,
    double penalty,
    const SolverOptions& opts,
    std::span<const double> forward_col_sol = {},
    std::span<const double> forward_row_dual = {})
    -> std::optional<ElasticSolveResult>;

/// Build a Benders feasibility cut from a solved elastic clone.
///
/// This wraps the common pattern: clone → relax → solve → extract cut.
/// The function calls `elastic_filter_solve()` internally.
///
/// @param li          The LP to clone (not modified)
/// @param alpha_col   α column in the source phase's LP
/// @param links       Outgoing state-variable links from the source phase
/// @param penalty     Elastic penalty coefficient
/// @param opts        Solver options
/// @param name        Name for the resulting cut row
/// @return A feasibility cut (SparseRow) and the ElasticSolveResult,
///         or nullopt if the elastic solve fails.
struct FeasibilityCutResult
{
  SparseRow cut {};  ///< The feasibility Benders cut
  ElasticSolveResult elastic;  ///< Clone + slack info (for multi-cut)
};

[[nodiscard]] auto build_feasibility_cut(const LinearInterface& li,
                                         ColIndex alpha_col,
                                         std::span<const StateVarLink> links,
                                         double penalty,
                                         const SolverOptions& opts,
                                         std::string_view name)
    -> std::optional<FeasibilityCutResult>;

/// Build per-slack bound-constraint cuts from a solved elastic clone.
///
/// For each outgoing link whose slack was activated (non-zero) in the
/// elastic-clone solution, this function generates one or two bound-cut
/// rows on the source column:
///   - sup > ε  ⟹  source_col ≤ dep_val   (upper-bound cut)
///   - sdn > ε  ⟹  source_col ≥ dep_val   (lower-bound cut)
///
/// @param elastic  The solved elastic clone and per-link slack info
/// @param links    Outgoing state-variable links (same order as elastic)
/// @param name_prefix  Prefix for generated cut names
/// @param slack_tol  Minimum slack magnitude to consider "active"
/// @return Vector of bound-constraint cuts (may be empty)
[[nodiscard]] auto build_multi_cuts(const ElasticSolveResult& elastic,
                                    std::span<const StateVarLink> links,
                                    std::string_view name_prefix,
                                    double slack_tol = 1e-6)
    -> std::vector<SparseRow>;

// ─── Cut averaging ──────────────────────────────────────────────────────────

/// Compute an average cut from a collection of cuts (for Expected sharing)
[[nodiscard]] auto average_benders_cut(const std::vector<SparseRow>& cuts,
                                       std::string_view name) -> SparseRow;

/// Compute a probability-weighted average cut from a collection of cuts.
///
/// Each cut is weighted by the corresponding element in @p weights.
/// The weights are normalised internally so they need not sum to 1.
/// If all weights are zero the function returns an empty SparseRow.
///
/// @param cuts    Collection of Benders optimality cuts (SparseRow)
/// @param weights Per-cut probability weights (must be same size as cuts)
/// @param name    Name for the resulting averaged cut row
[[nodiscard]] auto weighted_average_benders_cut(
    const std::vector<SparseRow>& cuts,
    const std::vector<double>& weights,
    std::string_view name) -> SparseRow;

/// Accumulate (sum) all cuts into a single combined cut.
///
/// When LP subproblem objectives already include probability factors,
/// the correct "expected cut" is the sum of all individual cuts rather
/// than a weighted average.  Each cut's coefficients and RHS are assumed
/// to be pre-weighted by the scenario probability.
///
/// The resulting cut has:
///   lowb = Σ_i cuts[i].lowb
///   coefficients = Σ_i cuts[i].coefficients  (for each column)
///   uppb = DblMax (unchanged)
///
/// @param cuts  Collection of Benders optimality cuts (SparseRow)
/// @param name  Name for the resulting accumulated cut row
[[nodiscard]] auto accumulate_benders_cuts(const std::vector<SparseRow>& cuts,
                                           std::string_view name) -> SparseRow;

// ─── BendersCut class ────────────────────────────────────────────────────────

/**
 * @class BendersCut
 * @brief Class-based interface for Benders cut construction with work-pool
 *        support and infeasibility monitoring.
 *
 * Wraps the free functions declared above as member functions and adds:
 * - An optional `AdaptiveWorkPool` used for LP solve/resolve operations.
 *   When a pool is provided, the elastic-filter clone LP solve is submitted
 *   to the pool (rather than run synchronously on the calling thread).
 *   This allows the solver's work pool to track and schedule elastic-filter
 *   solves alongside other LP subproblems.
 * - An infeasible-cut counter: every successful elastic-filter solve
 *   (i.e. every LP infeasibility event handled by the filter) increments
 *   the counter.  The counter can be reset per-iteration and queried for
 *   monitoring-API integration (e.g. logged to the SDDP JSON status file).
 *
 * ## Usage
 *
 * ```cpp
 * // In SDDPSolver::solve():
 * auto pool = make_solver_work_pool();
 * m_benders_cut_.set_pool(pool.get());
 *
 * // In elastic_solve():
 * auto result = m_benders_cut_.elastic_filter_solve(li, links, penalty, opts);
 *
 * // After each iteration:
 * ir.infeasible_cuts_added = m_benders_cut_.infeasible_cut_count();
 * m_benders_cut_.reset_infeasible_cut_count();
 * ```
 */
class BendersCut
{
public:
  /// Construct with an optional work pool for LP solve/resolve operations.
  /// If @p pool is nullptr, LP solves are performed synchronously on the
  /// calling thread (same behaviour as the standalone free functions).
  explicit BendersCut(AdaptiveWorkPool* pool = nullptr) noexcept
      : m_pool_(pool)
  {
  }

  BendersCut(const BendersCut&) = delete;
  BendersCut& operator=(const BendersCut&) = delete;
  BendersCut(BendersCut&&) = delete;
  BendersCut& operator=(BendersCut&&) = delete;
  ~BendersCut() = default;

  /// Update the work pool used for LP solves.
  /// Must be called from a single thread (e.g. before starting the parallel
  /// solve loop).  Not safe to call concurrently with elastic_filter_solve().
  void set_pool(AdaptiveWorkPool* pool) noexcept { m_pool_ = pool; }

  /// Access the current work pool (may be nullptr).
  [[nodiscard]] AdaptiveWorkPool* pool() const noexcept { return m_pool_; }

  /// Number of successful elastic-filter solves since construction (or last
  /// reset).  Each such solve corresponds to an LP infeasibility event; in
  /// the backward pass these become feasibility cuts.
  [[nodiscard]] int infeasible_cut_count() const noexcept
  {
    return m_infeasible_cut_count_.load(std::memory_order_relaxed);
  }

  /// Reset the infeasible-cut counter (typically called at the start of each
  /// SDDP iteration to obtain per-iteration counts).
  void reset_infeasible_cut_count() noexcept
  {
    m_infeasible_cut_count_.store(0, std::memory_order_relaxed);
  }

  /// Clone @p li, apply elastic relaxation on fixed state-variable columns,
  /// and solve the clone.  When a work pool is set, the LP solve is submitted
  /// to the pool (allowing the pool's scheduling and monitoring to observe it);
  /// otherwise the solve is performed synchronously.
  ///
  /// Increments the infeasible-cut counter on each successful solve.
  ///
  /// @return Solved elastic clone and per-link slack info, or nullopt if no
  ///         columns were fixed or the clone solve failed.
  [[nodiscard]] auto elastic_filter_solve(
      const LinearInterface& li,
      std::span<const StateVarLink> links,
      double penalty,
      const SolverOptions& opts,
      std::span<const double> forward_col_sol = {},
      std::span<const double> forward_row_dual = {})
      -> std::optional<ElasticSolveResult>;

  /// Build a Benders feasibility cut using this object's elastic_filter_solve.
  /// Equivalent to the free function `build_feasibility_cut()` but uses the
  /// work pool (if set) for the internal LP solve.
  ///
  /// @return A feasibility cut and the ElasticSolveResult, or nullopt if the
  ///         elastic solve fails.
  [[nodiscard]] auto build_feasibility_cut(const LinearInterface& li,
                                           ColIndex alpha_col,
                                           std::span<const StateVarLink> links,
                                           double penalty,
                                           const SolverOptions& opts,
                                           std::string_view name)
      -> std::optional<FeasibilityCutResult>;

private:
  AdaptiveWorkPool* m_pool_ {nullptr};
  std::atomic<int> m_infeasible_cut_count_ {0};
};

}  // namespace gtopt
