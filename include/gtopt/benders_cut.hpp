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
 * The SDDP solver (`sddp_method.hpp`) re-exports the free-function symbols
 * so that existing code that includes `sddp_method.hpp` continues to compile
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
#include <gtopt/lp_context.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/state_variable.hpp>

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
  PhaseIndex source_phase_index {};  ///< Phase where the source column lives
  PhaseIndex target_phase_index {};  ///< Phase where the dependent column lives
  double trial_value {0.0};  ///< Trial value from the last forward pass
  double source_low {0.0};  ///< Physical lower bound of source column
  double source_upp {0.0};  ///< Physical upper bound of source column
  /// Variable scale factor: physical = LP × var_scale.
  /// Used by the elastic filter to scale the penalty per LP unit.
  double var_scale {1.0};
  /// Per-variable state cost for elastic penalty [$/physical_unit / scale_obj].
  /// When > 0, overrides the global penalty for this link.
  /// Pre-divided by scale_objective during link construction for consistency
  /// with the global penalty (which is also pre-divided by scale_objective).
  double scost {0.0};
  /// Back-pointer to the StateVariable this link represents.
  ///
  /// The StateVariable lives in `SimulationLP::m_global_variable_map_`
  /// (a `flat_map` populated once at LP build time, never reshuffled),
  /// so its address is stable for the lifetime of the solver run — the
  /// same lifetime that already couples source and dependent LP columns.
  ///
  /// Used by the SDDP solve passes to read/write runtime values
  /// (col_sol, reduced_cost) directly on the state variable, avoiding
  /// full per-LP `std::vector<double>` snapshots.  Nullable for test
  /// fixtures that exercise cut builders in isolation.
  const StateVariable* state_var {nullptr};
  /// Identity of the state-variable element, for diagnostic logs.
  /// Captured once at link collection time from the simulation registry
  /// Key.  The string_views reference the same stable storage as the
  /// map keys, so they outlive the link for the whole solver lifetime.
  std::string_view class_name {};  ///< e.g. "Reservoir", "Battery"
  std::string_view col_name {};  ///< e.g. "efin", "sini"
  Uid uid {unknown_uid};  ///< Element UID
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
/// via column bounds (lo == hi).  This is the standard SDDP bound-fixing
/// formulation: the reduced cost of the fixed column at optimum is the
/// shadow price of the implicit bound, which becomes the cut coefficient.
void propagate_trial_values(std::span<StateVarLink> links,
                            std::span<const double> source_solution,
                            LinearInterface& target_li) noexcept;

/// Propagate trial values using the source StateVariable's cached
/// col_sol() instead of a full-LP solution span.  This is the only
/// overload used by production SDDP code — the older span-based overload
/// above is kept for tests that drive propagation directly.
void propagate_trial_values(std::span<StateVarLink> links,
                            LinearInterface& target_li) noexcept;

/// Build a Benders optimality cut from reduced costs of dependent columns.
///
///   scale_alpha · α_lp ≥ z_t + Σ_i rc_i · (x_{t-1,i} − v̂_i)
///
/// @param alpha_col       Column index of the α (future-cost) variable.
/// @param links           State-variable linkage descriptors.
/// @param reduced_costs   Reduced costs of dependent columns from the LP solve.
/// @param objective_value Optimal objective value of the sub-problem.
/// @param scale_alpha     Scale divisor for α (PLP varphi scale; default 1.0).
/// @param cut_coeff_eps   Threshold below which state-var coefficients are
///                        dropped (default 0.0 = keep all).
/// @param scale_objective Objective scaling factor (default 1.0).  The cut
///                        equation is in $; the RHS and state-variable
///                        coefficients are divided by `scale_objective` to
///                        match the LP objective's $/scale_objective scaling
///                        and keep cut row coefficients well-conditioned.
///                        The α column (which is dimensionless within the
///                        cut) is left untouched.
/// Returns the cut as a SparseRow ready to add to the source phase.
/// Callers set structured metadata (.class_name, .constraint_name, .context)
/// on the returned row.
[[nodiscard]] auto build_benders_cut(ColIndex alpha_col,
                                     std::span<const StateVarLink> links,
                                     std::span<const double> reduced_costs,
                                     double objective_value,
                                     double scale_alpha = 1.0,
                                     double cut_coeff_eps = 0.0,
                                     double scale_objective = 1.0) -> SparseRow;

/// Overload that reads reduced costs directly from each link's
/// `state_var->reduced_cost()` instead of indexing into a full
/// `std::span<const double>`.  Prefer this overload in SDDP production
/// code: it lets the forward pass elide the per-phase `forward_col_cost`
/// snapshot (now removed entirely — see docs/methods/sddp.md for the
/// rationale behind dropping the row-dual formulation).
[[nodiscard]] auto build_benders_cut(ColIndex alpha_col,
                                     std::span<const StateVarLink> links,
                                     double objective_value,
                                     double scale_alpha = 1.0,
                                     double cut_coeff_eps = 0.0,
                                     double scale_objective = 1.0) -> SparseRow;

/// Remove state-variable coefficients whose absolute value is below @p eps.
///
/// Unlike the eps filtering in build_benders_cut() (which skips tiny
/// reduced costs before the RHS adjustment), this function filters
/// the final cut coefficients and adjusts the RHS accordingly.
/// Intended for post-rescale cleanup where previously significant
/// coefficients may have become negligible.
///
/// @param row           The cut row to filter in-place
/// @param alpha_col     α column index (never filtered)
/// @param eps           Absolute tolerance (coefficients with |value| < eps
///                      are removed)
void filter_cut_coefficients(SparseRow& row, ColIndex alpha_col, double eps);

/// Rescale a Benders cut row when the largest state-variable coefficient
/// exceeds @p cut_coeff_max.
///
/// All terms (coefficients, α weight, and RHS) are divided uniformly by
/// `max_coeff / cut_coeff_max`, preserving the constraint's feasible set.
/// The α column at @p alpha_col is included in the scaling.
///
/// @param row           The cut row to rescale in-place
/// @param alpha_col     α column index (to identify it for logging)
/// @param cut_coeff_max Maximum allowed absolute coefficient value (> 0)
/// @return true if the row was rescaled, false if no rescaling was needed
bool rescale_benders_cut(SparseRow& row,
                         ColIndex alpha_col,
                         double cut_coeff_max);

/// Physical-space Benders optimality cut builder.
///
///   α_phys ≥ z_t_phys + Σ_i rc_phys_i · (x_{t-1,i}_phys − v̂_i_phys)
///
/// Intended for use with `LinearInterface::add_equilibrated_row`, which
/// folds `col_scales` and the per-row equilibration internally.  All
/// inputs are in physical space:
///
/// @param alpha_col              α column index in the source phase's LP.
/// @param links                  State-variable linkage descriptors (same
///                               struct the LP-space overload consumes).
///                               `link.dependent_col` selects the target
///                               entry in @p reduced_costs_physical;
///                               `link.source_col` selects the matching
///                               entry in @p trial_values_physical.
/// @param reduced_costs_physical Physical reduced costs of the target
///                               LP's dependent columns — read from
///                               `target_li.get_col_cost()` (which
///                               applies `LP × scale_objective /
///                               col_scale`).
/// @param trial_values_physical  Physical trial values for each link's
///                               source column — read from
///                               `source_li.get_col_sol()[link.source_col]`
///                               (= `LP × col_scale_source`).  Indexed
///                               in parallel with `links`, so entry `i`
///                               corresponds to `links[i]`.
/// @param objective_value_physical Target subproblem optimum in $,
///                               i.e. `target_li.get_obj_value_physical()`.
/// @param cut_coeff_eps          Drop state-var coefficients below this
///                               absolute threshold (default 0 =
///                               keep all).
///
/// Returns a SparseRow with:
///   * row[alpha_col] = 1.0
///   * row[source_col] = -rc_phys
///   * row.lowb = obj_phys − Σ rc_phys · v̂_phys
///   * row.uppb = DblMax, row.scale = 1.0
///
/// The caller is expected to pass this row to
/// `LinearInterface::add_equilibrated_row`, which applies the LP's
/// column scales and per-row row-max equilibration to produce the
/// final LP-space row.
[[nodiscard]] auto build_benders_cut_physical(
    ColIndex alpha_col,
    std::span<const StateVarLink> links,
    std::span<const double> reduced_costs_physical,
    std::span<const double> trial_values_physical,
    double objective_value_physical,
    double cut_coeff_eps = 0.0) -> SparseRow;

/// Physical-space Benders cut builder that reads reduced cost and trial
/// value from each link's back-pointer state variable instead of taking
/// flat spans.  The SDDP backward pass uses this overload: the forward
/// pass already mirrors the target LP's `col_sol` / `reduced_cost` onto
/// every `StateVariable` via `capture_state_variable_values`, so the
/// builder can read the live values directly without re-taking the full
/// per-LP snapshots.
///
///   rc_phys  = state_var->reduced_cost_physical(scale_objective)
///   v̂_phys  = state_var->col_sol_physical()
///   lowb    = objective_value_physical − Σ rc_phys · v̂_phys
///   row[src_col] = −rc_phys,  row[alpha_col] = 1.0
///
/// The returned row carries no `already_lp_space` flag: `add_row` on an
/// equilibrated LP will fold `col_scales` and run per-row row-max
/// equilibration, so the resulting row is well-conditioned without any
/// post-hoc `rescale_benders_cut` pass.
///
/// @param alpha_col               α column in the source phase's LP.
/// @param links                   State-variable linkage descriptors.
///                                Links with a null `state_var` contribute
///                                zero (rc_phys = v̂_phys = 0), matching
///                                the test-fixture convention.
/// @param objective_value_physical Target subproblem optimum in $,
///                                i.e. `target_li.get_obj_value_physical()`.
/// @param scale_objective         Global objective scale (required so
///                                `reduced_cost_physical()` can descale
///                                the LP reduced cost to $/phys_unit).
/// @param cut_coeff_eps           Drop state-var coefficients below this
///                                absolute threshold (default 0 = keep all).
[[nodiscard]] auto build_benders_cut_physical(
    ColIndex alpha_col,
    std::span<const StateVarLink> links,
    double objective_value_physical,
    double scale_objective,
    double cut_coeff_eps = 0.0) -> SparseRow;

// ─── Elastic filter ─────────────────────────────────────────────────────────

/// Relax a single fixed state-variable column to its physical source bounds,
/// adding penalised slack variables.
/// Returns a RelaxedVarInfo with the relaxation status and slack column
/// indices.  Converts to bool (true iff relaxed) for backward compatibility.
[[nodiscard]] RelaxedVarInfo relax_fixed_state_variable(
    LinearInterface& li,
    const StateVarLink& link,
    PhaseIndex phase_index,
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
/// @param li                The LP to clone (not modified)
/// @param links             Outgoing state-variable links from the previous
/// phase
/// @param penalty           Elastic penalty coefficient for slack variables
/// @param opts              Solver options for the clone solve
/// @return Solved elastic clone and per-link slack info, or nullopt if
///         no columns were fixed or the clone solve failed.
[[nodiscard]] auto elastic_filter_solve(const LinearInterface& li,
                                        std::span<const StateVarLink> links,
                                        double penalty,
                                        const SolverOptions& opts)
    -> std::optional<ElasticSolveResult>;

/// Chinneck-style elastic IIS filter.
///
/// Single-pass deletion-filter approximation of Chinneck's elastic algorithm
/// (2008, *Feasibility and Infeasibility in Optimization*, §3.5).
///
/// Procedure:
/// 1. Clone the LP and relax every fixed state-variable bound with two
///    penalised slack variables (same as `elastic_filter_solve()`).
/// 2. Solve the relaxed clone with @p penalty.  If it is itself infeasible
///    or some other solver error occurs, return nullopt.
/// 3. Inspect the slack values:
///      - links whose `sup` and `sdn` are both ≤ @p slack_tol are NOT
///        contributing to the infeasibility ("non-essential")
///      - links with at least one slack > @p slack_tol form the IIS
///        candidate set
/// 4. Re-fix the non-essential links to their original `trial_value`
///    (drop their slack columns out of consideration by zeroing the
///    upper bound on `sup`/`sdn`) and re-solve.  If the re-solve stays
///    feasible at the same penalty cost, the IIS is confirmed.  If it
///    becomes infeasible (one of the supposedly non-essential links was
///    actually essential due to penalty competition), undo the re-fix
///    and fall back to the conservative full-elastic IIS.
///
/// The returned `ElasticSolveResult` carries `link_infos` whose `sup_col`
/// / `sdn_col` are reset to `unknown_index` for non-essential links, so
/// downstream `build_multi_cuts()` skips them and emits cuts only on the
/// IIS subset.
///
/// @param li        The LP to clone (not modified)
/// @param links     Outgoing state-variable links from the previous phase
/// @param penalty   Elastic penalty coefficient (smaller than for
///                  `elastic_filter_solve` is fine — IIS uses *which*
///                  slack is non-zero, not by how much it dominates cost)
/// @param opts      Solver options
/// @param slack_tol Tolerance for considering a slack "active" (default 1e-6)
/// @return Solved IIS-filtered clone and link-infos, or nullopt on failure.
[[nodiscard]] auto chinneck_filter_solve(const LinearInterface& li,
                                         std::span<const StateVarLink> links,
                                         double penalty,
                                         const SolverOptions& opts,
                                         double slack_tol = 1e-6)
    -> std::optional<ElasticSolveResult>;

/// @brief Result structure for feasibility cut building
struct FeasibilityCutResult
{
  SparseRow cut {};  ///< The feasibility Benders cut
  ElasticSolveResult elastic;  ///< Clone + slack info (for multi-cut)
};

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
/// @return A feasibility cut (SparseRow) and the ElasticSolveResult,
///         or nullopt if the elastic solve fails.
[[nodiscard]] auto build_feasibility_cut(const LinearInterface& li,
                                         ColIndex alpha_col,
                                         std::span<const StateVarLink> links,
                                         double penalty,
                                         const SolverOptions& opts,
                                         double scale_alpha = 1.0,
                                         double scale_objective = 1.0)
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
/// @param context  LP context for the generated cut rows
/// @param slack_tol  Minimum slack magnitude to consider "active"
/// @return Vector of bound-constraint cuts (may be empty)
[[nodiscard]] auto build_multi_cuts(const ElasticSolveResult& elastic,
                                    std::span<const StateVarLink> links,
                                    const LpContext& context = {},
                                    double slack_tol = 1e-6)
    -> std::vector<SparseRow>;

// ─── Cut averaging ──────────────────────────────────────────────────────────

/// Compute an average cut from a collection of cuts (for Expected sharing)
[[nodiscard]] auto average_benders_cut(const std::vector<SparseRow>& cuts)
    -> SparseRow;

/// Compute a probability-weighted average cut from a collection of cuts.
///
/// Each cut is weighted by the corresponding element in @p weights.
/// The weights are normalised internally so they need not sum to 1.
/// If all weights are zero the function returns an empty SparseRow.
///
/// @param cuts    Collection of Benders optimality cuts (SparseRow)
/// @param weights Per-cut probability weights (must be same size as cuts)
[[nodiscard]] auto weighted_average_benders_cut(
    const std::vector<SparseRow>& cuts, const std::vector<double>& weights)
    -> SparseRow;

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
[[nodiscard]] auto accumulate_benders_cuts(const std::vector<SparseRow>& cuts)
    -> SparseRow;

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
 * // In SDDPMethod::solve():
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
  [[nodiscard]] auto elastic_filter_solve(const LinearInterface& li,
                                          std::span<const StateVarLink> links,
                                          double penalty,
                                          const SolverOptions& opts)
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
                                           double scale_alpha = 1.0,
                                           double scale_objective = 1.0)
      -> std::optional<FeasibilityCutResult>;

private:
  AdaptiveWorkPool* m_pool_ {nullptr};
  std::atomic<int> m_infeasible_cut_count_ {0};
};

}  // namespace gtopt
