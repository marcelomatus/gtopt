/**
 * @file      sddp_solver.hpp
 * @brief     SDDP (Stochastic Dual Dynamic Programming) solver for multi-phase
 *            planning problems with state variable coupling
 * @date      2026-03-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements a forward/backward iterative decomposition similar to the PLP
 * SDDP methodology.  Each gtopt-phase corresponds to a PLP-stage and is
 * solved as an independent LP subproblem.  State variables (reservoir
 * volumes, capacity expansion variables, and future irrigation rights)
 * link consecutive phases:
 *
 *   efin[t] → eini[t+1]   (reservoir volume)
 *   capainst[t] → capainst_ini[t+1]  (installed capacity)
 *
 * The solver uses the existing `SimulationLP::state_variables()` map to
 * discover all state-variable linkages generically, without hard-coding
 * any specific component type.
 *
 * **Forward pass** – phases are solved in order; state variable values
 *   propagate from source columns in phase t to dependent columns in
 *   phase t+1.
 *
 * **Backward pass** – starting from the last phase, optimality (Benders)
 *   cuts are generated from the reduced costs of the dependent state
 *   variables and added to the previous phase's LP.  An elastic filter
 *   ensures feasibility when the trial point from the forward pass would
 *   otherwise make the downstream LP infeasible.
 *
 * The solver iterates until the gap between the upper bound (forward-pass
 * cost) and the lower bound (with future-cost approximation) falls below
 * a configurable tolerance, or a maximum iteration count is reached.
 */

#pragma once

#include <expected>
#include <functional>
#include <span>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/error.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

// ─── Configuration ──────────────────────────────────────────────────────────

/// Configuration options for the SDDP iterative solver
struct SDDPOptions
{
  int max_iterations {100};  ///< Maximum forward/backward iterations
  double convergence_tol {1e-4};  ///< Relative gap tolerance for convergence
  double elastic_penalty {1e6};  ///< Penalty for elastic slack variables
  double alpha_min {0.0};  ///< Lower bound for future cost variable α ($)
  double alpha_max {1e12};  ///< Upper bound for future cost variable α ($)
};

// ─── Iteration result ───────────────────────────────────────────────────────

/// Result of a single SDDP iteration (forward + backward pass)
struct SDDPIterationResult
{
  int iteration {};  ///< Iteration number (1-based)
  double lower_bound {};  ///< Lower bound (phase 0 objective including α)
  double upper_bound {};  ///< Upper bound (sum of actual phase costs)
  double gap {};  ///< Relative gap: (UB − LB) / max(1, |UB|)
  bool converged {};  ///< True if gap < convergence tolerance
  int cuts_added {};  ///< Number of Benders cuts added this iteration
  bool feasibility_issue {};  ///< True if elastic filter was activated
};

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

// ─── Per-phase tracking ─────────────────────────────────────────────────────

/// Per-phase SDDP state: α variable, outgoing links, forward-pass cost
struct PhaseStateInfo
{
  ColIndex alpha_col {unknown_index};  ///< α column (unknown for last)
  std::vector<StateVarLink> outgoing_links {};  ///< Links TO the next phase
  double forward_objective {0.0};  ///< Opex from last forward pass
};

// ─── Free-function building blocks ──────────────────────────────────────────
// These are the modular algorithmic primitives used by SDDPSolver.

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
                                     const std::string& name) -> SparseRow;

/// Relax a single fixed state-variable column to its physical source bounds,
/// adding penalised slack variables.  Returns true if the column was relaxed.
[[nodiscard]] bool relax_fixed_state_variable(LinearInterface& li,
                                              const StateVarLink& link,
                                              PhaseIndex phase,
                                              double penalty);

// ─── SDDPSolver ─────────────────────────────────────────────────────────────

/**
 * @class SDDPSolver
 * @brief Iterative SDDP solver for multi-phase power system planning
 *
 * Wraps a `PlanningLP` and adds Benders decomposition on top of the
 * per-phase LP subproblems.  Handles reservoir volumes, capacity
 * expansion variables, and future state-variable types generically.
 *
 * @code
 * PlanningLP planning_lp(planning);
 * SDDPSolver sddp(planning_lp, SDDPOptions{.max_iterations = 10});
 * auto results = sddp.solve();
 * @endcode
 */
class SDDPSolver
{
public:
  explicit SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts = {}) noexcept;

  /// Run the SDDP iterative solve
  [[nodiscard]] auto solve(const SolverOptions& lp_opts = {})
      -> std::expected<std::vector<SDDPIterationResult>, Error>;

  /// Per-phase state information (valid after at least one iteration)
  [[nodiscard]] constexpr auto& phase_states() const noexcept
  {
    return m_phase_states_;
  }

  /// SDDP options
  [[nodiscard]] constexpr auto& options() const noexcept { return m_options_; }

private:
  void initialize_alpha_variables(SceneIndex scene);
  void collect_state_variable_links(SceneIndex scene);

  [[nodiscard]] auto forward_pass(SceneIndex scene, const SolverOptions& opts)
      -> std::expected<double, Error>;

  [[nodiscard]] auto backward_pass(SceneIndex scene, const SolverOptions& opts)
      -> std::expected<int, Error>;

  [[nodiscard]] bool apply_elastic_filter(SceneIndex scene,
                                          PhaseIndex phase,
                                          const SolverOptions& opts);

  // Accessor for the wrapped PlanningLP reference (avoids raw reference member)
  [[nodiscard]] PlanningLP& planning_lp() noexcept
  {
    return m_planning_lp_.get();
  }
  [[nodiscard]] const PlanningLP& planning_lp() const noexcept
  {
    return m_planning_lp_.get();
  }

  std::reference_wrapper<PlanningLP> m_planning_lp_;
  SDDPOptions m_options_;
  StrongIndexVector<PhaseIndex, PhaseStateInfo> m_phase_states_;
  bool m_initialized_ {false};
};

}  // namespace gtopt
