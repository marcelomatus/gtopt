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
#include <string>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/error.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

/**
 * @brief Configuration options for the SDDP iterative solver
 */
struct SDDPOptions
{
  /// Maximum number of forward/backward iterations
  int max_iterations {100};

  /// Convergence tolerance (relative gap between upper and lower bounds)
  double convergence_tol {1e-4};

  /// Penalty cost for elastic slack variables used in the feasibility filter
  double elastic_penalty {1e6};

  /// Initial lower bound for the future cost variable α.
  /// Represents the minimum expected future cost (default: 0, i.e. non-negative
  /// future costs).  Units match the objective function units (e.g. $).
  double alpha_min {0.0};

  /// Initial upper bound for the future cost variable α.
  /// Should be set large enough to not constrain the true future cost.
  /// Units match the objective function units (e.g. $).
  double alpha_max {1e12};

  /// Relative margin for relaxing fixed state-variable bounds in the elastic
  /// filter (fraction of the absolute fixed value, default 10%).
  double elastic_margin_factor {0.1};
};

/**
 * @brief Result of a single SDDP iteration (forward + backward pass)
 */
struct SDDPIterationResult
{
  int iteration {};  ///< Iteration number (1-based)
  double lower_bound {};  ///< Lower bound (sum of phase objectives with α)
  double upper_bound {};  ///< Upper bound (sum of actual phase costs)
  double gap {};  ///< Relative gap: (UB - LB) / max(1, |UB|)
  bool converged {};  ///< True if gap < convergence tolerance
  int cuts_added {};  ///< Number of Benders cuts added in this iteration
  bool feasibility_issue {};  ///< True if elastic filter was activated
};

/**
 * @brief Describes one state-variable linkage between two consecutive phases
 *
 * A state variable has a *source* column in phase t (the value chosen by
 * the optimiser, e.g. efin of a reservoir, capainst of a generator) and
 * a *dependent* column in phase t+1 (fixed to the source value in the
 * forward pass, e.g. eini of a reservoir, capainst_ini of a generator).
 */
struct StateVarLink
{
  ColIndex source_col {};  ///< Source column in source phase's LP
  ColIndex dependent_col {};  ///< Dependent column in target phase's LP
  PhaseIndex source_phase {};  ///< Phase where the source column lives
  PhaseIndex target_phase {};  ///< Phase where the dependent column lives
  double trial_value {0.0};  ///< Trial value from the last forward pass
};

/**
 * @brief Per-phase tracking for the SDDP iteration
 */
struct PhaseStateInfo
{
  /// Column index of the future cost variable α in this phase's LP
  /// (ColIndex{unknown_index} for the last phase, which has no future cost)
  ColIndex alpha_col {unknown_index};

  /// State variable linkages FROM this phase TO the next phase.
  /// Each entry describes one source→dependent pair.
  std::vector<StateVarLink> outgoing_links {};

  /// Objective value of this phase (excluding α) from the last forward pass
  double forward_objective {0.0};
};

/**
 * @class SDDPSolver
 * @brief Iterative SDDP solver for multi-phase power system planning
 *
 * Wraps a `PlanningLP` instance and adds Benders decomposition on top of
 * the per-phase LP subproblems.  The solver uses the generic state-variable
 * infrastructure to handle reservoir volumes, capacity expansion variables,
 * and any future state variable types (e.g. irrigation rights).
 *
 * ### Usage
 * ```cpp
 * PlanningLP planning_lp(planning);
 * SDDPSolver sddp(planning_lp);
 * auto results = sddp.solve();
 * ```
 */
class SDDPSolver
{
public:
  /**
   * @brief Construct from an existing PlanningLP
   * @param planning_lp  Reference to the planning LP (must outlive solver)
   * @param opts         SDDP-specific configuration options
   */
  explicit SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts = {}) noexcept;

  /**
   * @brief Run the SDDP iterative solve
   * @param lp_opts  LP solver options forwarded to each phase solve
   * @return Vector of per-iteration results, or an error
   */
  [[nodiscard]] auto solve(const SolverOptions& lp_opts = {})
      -> std::expected<std::vector<SDDPIterationResult>, Error>;

  /// Access per-phase state information (after at least one iteration)
  [[nodiscard]] constexpr const auto& phase_states() const noexcept
  {
    return m_phase_states_;
  }

  /// Access SDDP options
  [[nodiscard]] constexpr const SDDPOptions& options() const noexcept
  {
    return m_options_;
  }

private:
  /// Add α (future cost) variables to all phases except the last
  void initialize_alpha_variables(SceneIndex scene_index);

  /// Discover all state-variable linkages between consecutive phases
  void collect_state_variable_links(SceneIndex scene_index);

  /**
   * @brief Execute one forward pass (solve phases in order)
   * @return Upper bound (sum of actual operating costs, excluding α)
   */
  [[nodiscard]] auto forward_pass(SceneIndex scene_index,
                                  const SolverOptions& lp_opts)
      -> std::expected<double, Error>;

  /**
   * @brief Execute one backward pass (compute duals, add Benders cuts)
   * @return Number of cuts added, or an error
   */
  [[nodiscard]] auto backward_pass(SceneIndex scene_index,
                                   const SolverOptions& lp_opts)
      -> std::expected<int, Error>;

  /**
   * @brief Apply elastic filter when a phase LP is infeasible
   *
   * Temporarily relaxes the dependent state-variable bounds with penalised
   * slack variables, re-solves, and uses the dual information to generate
   * a feasibility cut for the previous phase.
   *
   * @return true if the elastic solve succeeded, false otherwise
   */
  [[nodiscard]] bool apply_elastic_filter(SceneIndex scene_index,
                                          PhaseIndex phase_index,
                                          const SolverOptions& lp_opts);

  PlanningLP& m_planning_lp_;
  SDDPOptions m_options_;

  /// Per-phase state tracking (indexed by PhaseIndex)
  StrongIndexVector<PhaseIndex, PhaseStateInfo> m_phase_states_;

  /// Whether α variables have been initialised
  bool m_initialized_ {false};
};

}  // namespace gtopt
