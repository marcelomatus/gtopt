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
 *   otherwise make the downstream LP infeasible.  Feasibility issues
 *   propagate backward iteratively: if adding a cut makes phase k
 *   infeasible, the solver builds a feasibility cut for phase k-1, and
 *   continues all the way to phase 0 if necessary.
 *
 * **Multi-scene support** – each scene is an independent trajectory (like
 *   a PLP scenario).  Scenes are solved in parallel via the work pool.
 *
 * **Cut sharing** – optimality cuts generated in one scene can be shared
 *   with other scenes at the same phase level.  Three modes are supported:
 *   - None:     cuts stay in their originating scene (default)
 *   - Expected: an average cut across scenes is computed and added to all
 *   - Max:      all cuts from all scenes are added to all scenes
 *
 * **Cut persistence** – cuts can be saved to and loaded from JSON files
 *   for hot-start capability.
 *
 * The solver iterates until the gap between the upper bound (forward-pass
 * cost) and the lower bound (with future-cost approximation) falls below
 * a configurable tolerance, or a maximum iteration count is reached.
 */

#pragma once

#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/error.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

// ─── Cut sharing mode ───────────────────────────────────────────────────────

/**
 * @brief How optimality cuts are shared between scenes at the same phase
 *
 * In PLP, only None and Expected are implemented.  gtopt supports all three.
 */
enum class CutSharingMode : uint8_t
{
  None = 0,  ///< No sharing; cuts stay in their originating scene
  Expected,  ///< Average cut across scenes, added to all scenes
  Max,  ///< All cuts from all scenes added to all scenes
};

/// Parse a cut-sharing mode from a string ("none", "expected", "max")
[[nodiscard]] CutSharingMode parse_cut_sharing_mode(std::string_view name);

// ─── Configuration ──────────────────────────────────────────────────────────

/// Configuration options for the SDDP iterative solver
struct SDDPOptions
{
  int max_iterations {100};  ///< Maximum forward/backward iterations
  double convergence_tol {1e-4};  ///< Relative gap tolerance for convergence
  double elastic_penalty {1e6};  ///< Penalty for elastic slack variables
  double alpha_min {0.0};  ///< Lower bound for future cost variable α ($)
  double alpha_max {1e12};  ///< Upper bound for future cost variable α ($)
  CutSharingMode cut_sharing {CutSharingMode::None};  ///< Cut sharing mode

  /// File path for saving cuts (empty = no save)
  std::string cuts_output_file {};
  /// File path for loading initial cuts (empty = no load / cold start)
  std::string cuts_input_file {};
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

// ─── Stored cut for persistence ─────────────────────────────────────────────

/// A serialisable representation of a Benders cut
struct StoredCut
{
  int phase {};  ///< Phase index this cut was added to
  int scene {};  ///< Scene that generated this cut (-1 = shared)
  std::string name {};  ///< Cut name
  double rhs {};  ///< Right-hand side (lower bound)
  /// Coefficient pairs: (column_index, coefficient)
  std::vector<std::pair<int, double>> coefficients {};
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

/// Compute an average cut from a collection of cuts (for Expected sharing)
[[nodiscard]] auto average_benders_cut(const std::vector<SparseRow>& cuts,
                                       const std::string& name) -> SparseRow;

// ─── SDDPSolver ─────────────────────────────────────────────────────────────

/**
 * @class SDDPSolver
 * @brief Iterative SDDP solver for multi-phase power system planning
 *
 * Wraps a `PlanningLP` and adds Benders decomposition on top of the
 * per-phase LP subproblems.  Handles reservoir volumes, capacity
 * expansion variables, and future state-variable types generically.
 *
 * Supports multiple scenes (solved in parallel), optimality cut sharing
 * between scenes, iterative feasibility backpropagation, and cut
 * persistence for hot-start.
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

  /// Per-phase state for a given scene (valid after at least one iteration)
  [[nodiscard]] constexpr auto& phase_states(SceneIndex scene) const noexcept
  {
    return m_scene_phase_states_[scene];
  }

  /// Legacy accessor for scene 0 (backward compatibility)
  [[nodiscard]] constexpr auto& phase_states() const noexcept
  {
    return m_scene_phase_states_[SceneIndex {0}];
  }

  /// SDDP options
  [[nodiscard]] constexpr auto& options() const noexcept { return m_options_; }

  /// All stored cuts (for persistence / inspection)
  [[nodiscard]] const auto& stored_cuts() const noexcept
  {
    return m_stored_cuts_;
  }

  /// Save accumulated cuts to a JSON file
  [[nodiscard]] auto save_cuts(const std::string& filepath) const
      -> std::expected<void, Error>;

  /// Load cuts from a JSON file and add to phase LPs
  [[nodiscard]] auto load_cuts(const std::string& filepath)
      -> std::expected<int, Error>;

private:
  using scene_phase_states_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, PhaseStateInfo>>;

  void initialize_alpha_variables(SceneIndex scene);
  void collect_state_variable_links(SceneIndex scene);

  [[nodiscard]] auto forward_pass(SceneIndex scene, const SolverOptions& opts)
      -> std::expected<double, Error>;

  [[nodiscard]] auto backward_pass(SceneIndex scene, const SolverOptions& opts)
      -> std::expected<int, Error>;

  [[nodiscard]] bool apply_elastic_filter(SceneIndex scene,
                                          PhaseIndex phase,
                                          const SolverOptions& opts);

  /// Apply cut sharing across scenes for a given phase
  void share_cuts_for_phase(
      PhaseIndex phase,
      const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts);

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
  scene_phase_states_t m_scene_phase_states_;
  std::vector<StoredCut> m_stored_cuts_;
  bool m_initialized_ {false};
};

// ─── SDDPPlanningSolver ─────────────────────────────────────────────────────

/**
 * @class SDDPPlanningSolver
 * @brief Adapter that wraps SDDPSolver behind the PlanningSolver interface
 */
class SDDPPlanningSolver final : public PlanningSolver
{
public:
  explicit SDDPPlanningSolver(SDDPOptions opts = {}) noexcept;

  [[nodiscard]] auto solve(PlanningLP& planning_lp, const SolverOptions& opts)
      -> std::expected<int, Error> override;

  /// Access the last iteration results (valid after solve())
  [[nodiscard]] const auto& last_results() const noexcept
  {
    return m_last_results_;
  }

private:
  SDDPOptions m_sddp_opts_;
  std::vector<SDDPIterationResult> m_last_results_;
};

}  // namespace gtopt
