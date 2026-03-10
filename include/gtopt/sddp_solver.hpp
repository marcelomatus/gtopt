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

#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/error.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/reservoir_efficiency_lp.hpp>
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

/// File naming patterns for per-scene cut files
namespace sddp_file
{
/// Combined cut file name
constexpr auto combined_cuts = "sddp_cuts.csv";
/// Per-scene cut file pattern: format with scene index
constexpr auto scene_cuts_fmt = "scene_{}.csv";
/// Error-prefixed cut file pattern for infeasible scenes
constexpr auto error_scene_cuts_fmt = "error_scene_{}.csv";
/// Error LP file pattern for infeasible scene/phase
constexpr auto error_lp_fmt = "error_scene_{}_phase_{}";
}  // namespace sddp_file

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

  /// Path to a sentinel file: if the file exists, the solver stops
  /// gracefully after the current iteration (analogous to PLP's userstop).
  /// All accumulated cuts are saved before stopping.
  std::string sentinel_file {};

  /// Directory for log and error LP files (default: "logs").
  /// Error LP files for infeasible scenes are saved here.
  std::string log_directory {"logs"};

  /// Enable the monitoring API: write a JSON status file after each iteration
  /// and periodically update real-time workpool statistics.  Consumers
  /// (e.g. sddp_monitor.py) can poll this file to display live charts.
  /// Default: true.
  bool enable_api {true};

  /// Path for the JSON status file.  If empty, the solver writes to
  /// "<output_directory>/sddp_status.json" (derived at solve time from the
  /// PlanningLP options).
  std::string api_status_file {};

  /// Interval at which the background monitoring thread refreshes real-time
  /// workpool statistics (CPU load, active workers) in the status file.
  std::chrono::milliseconds api_update_interval {500};
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

  /// Per-scene upper bounds (forward-pass costs).  Size = num_scenes.
  std::vector<double> scene_upper_bounds {};
  /// Per-scene lower bounds (phase-0 objective values).  Size = num_scenes.
  std::vector<double> scene_lower_bounds {};
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
  /// Full LP objective from last forward solve (including α).
  /// Cached for the backward pass so the original LP need not be re-queried.
  double forward_full_obj {0.0};
  /// Reduced costs from last forward solve (cached for backward pass).
  std::vector<double> forward_col_cost {};
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
                                     std::string_view name) -> SparseRow;

/// Relax a single fixed state-variable column to its physical source bounds,
/// adding penalised slack variables.  Returns true if the column was relaxed.
[[nodiscard]] bool relax_fixed_state_variable(LinearInterface& li,
                                              const StateVarLink& link,
                                              PhaseIndex phase,
                                              double penalty);

/// Compute an average cut from a collection of cuts (for Expected sharing)
[[nodiscard]] auto average_benders_cut(const std::vector<SparseRow>& cuts,
                                       std::string_view name) -> SparseRow;

// ─── Callback / observer API ────────────────────────────────────────────────

/// Callback invoked after each SDDP iteration.
/// If the callback returns `true`, the solver stops after this iteration.
using SDDPIterationCallback =
    std::function<bool(const SDDPIterationResult& result)>;

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
 * ## API for external monitoring / GUI integration
 *
 * The solver exposes a rich API that enables GUI or monitoring tools to
 * observe the iterative process and control execution:
 *
 * - **Callback**: register an `SDDPIterationCallback` via
 *   `set_iteration_callback()`.  It is invoked after every iteration with
 *   the full `SDDPIterationResult`.  Return `true` from the callback to
 *   request a stop.
 * - **Programmatic stop**: call `request_stop()` from any thread; the
 *   solver checks this flag at the start of each iteration and exits
 *   gracefully, saving all accumulated cuts.
 * - **Live query**: call `current_iteration()`, `current_gap()`,
 *   `current_lower_bound()`, `current_upper_bound()` at any time (they
 *   are atomic and thread-safe) to poll the solver's convergence state.
 * - **Sentinel file**: same as PLP's `userstop` — check for a sentinel
 *   file on disk.
 *
 * @code
 * SDDPSolver sddp(planning_lp, SDDPOptions{.max_iterations = 100});
 *
 * // Register a callback that prints progress and stops at gap < 1e-6
 * sddp.set_iteration_callback([](const SDDPIterationResult& r) {
 *     fmt::print("iter {} gap={:.6f}\n", r.iteration, r.gap);
 *     return r.gap < 1e-6;  // true → stop
 * });
 *
 * // Start solving (blocks until done / stopped)
 * auto results = sddp.solve();
 *
 * // Or stop programmatically from another thread:
 * sddp.request_stop();
 * @endcode
 */
class SDDPSolver
{
public:
  explicit SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts = {}) noexcept;

  /// Run the SDDP iterative solve
  [[nodiscard]] auto solve(const SolverOptions& lp_opts = {})
      -> std::expected<std::vector<SDDPIterationResult>, Error>;

  // ── Iteration callback / observer ──

  /// Register a callback invoked after each iteration.
  /// If the callback returns `true`, the solver stops after that iteration.
  void set_iteration_callback(SDDPIterationCallback cb) noexcept
  {
    m_iteration_callback_ = std::move(cb);
  }

  // ── Programmatic stop (thread-safe) ──

  /// Request the solver to stop gracefully after the current iteration.
  /// Thread-safe — may be called from any thread.
  void request_stop() noexcept { m_stop_requested_.store(true); }

  /// Clear a previous stop request (e.g., before re-running solve()).
  void clear_stop() noexcept { m_stop_requested_.store(false); }

  /// Check whether a stop has been requested.
  [[nodiscard]] bool is_stop_requested() const noexcept
  {
    return m_stop_requested_.load();
  }

  // ── Live query (thread-safe, atomic reads) ──

  /// Current iteration number (0 before first iteration completes)
  [[nodiscard]] int current_iteration() const noexcept
  {
    return m_current_iteration_.load();
  }

  /// Current relative convergence gap
  [[nodiscard]] double current_gap() const noexcept
  {
    return m_current_gap_.load();
  }

  /// Current lower bound (phase-0 objective including α)
  [[nodiscard]] double current_lower_bound() const noexcept
  {
    return m_current_lb_.load();
  }

  /// Current upper bound (sum of actual phase costs)
  [[nodiscard]] double current_upper_bound() const noexcept
  {
    return m_current_ub_.load();
  }

  /// Whether the solver has converged
  [[nodiscard]] bool has_converged() const noexcept
  {
    return m_converged_.load();
  }

  // ── Data accessors (valid after at least one iteration) ──

  /// Per-phase state for a given scene
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

  /// Number of stored cuts (thread-safe)
  [[nodiscard]] int num_stored_cuts() const noexcept
  {
    const std::scoped_lock lock(m_cuts_mutex_);
    return static_cast<int>(m_stored_cuts_.size());
  }

  /// Save accumulated cuts to a CSV file for hot-start
  [[nodiscard]] auto save_cuts(const std::string& filepath) const
      -> std::expected<void, Error>;

  /// Save cuts for a single scene to a per-scene file.
  /// Uses scene-specific storage, avoiding lock contention when called
  /// in parallel for different scenes.
  [[nodiscard]] auto save_scene_cuts(SceneIndex scene,
                                     const std::string& directory) const
      -> std::expected<void, Error>;

  /// Save all scenes' cuts to per-scene files in the given directory.
  /// Each scene gets its own file: `<directory>/scene_<N>.csv`.
  [[nodiscard]] auto save_all_scene_cuts(const std::string& directory) const
      -> std::expected<void, Error>;

  /// Load cuts from a CSV file and add to all scenes' phase LPs.
  /// Cuts are broadcast to all scenes regardless of originating scene,
  /// since loaded cuts serve as warm-start approximations for the entire
  /// problem (analogous to PLP's cut sharing across scenarios).
  [[nodiscard]] auto load_cuts(const std::string& filepath)
      -> std::expected<int, Error>;

  /// Load all per-scene cut files from a directory.
  /// Files matching `scene_<N>.csv` are loaded; files with the `error_`
  /// prefix (from infeasible scenes in a previous run) are skipped to
  /// prevent loading invalid cuts during hot-start.
  [[nodiscard]] auto load_scene_cuts_from_directory(
      const std::string& directory) -> std::expected<int, Error>;

private:
  using scene_phase_states_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, PhaseStateInfo>>;

  void initialize_alpha_variables(SceneIndex scene);
  void collect_state_variable_links(SceneIndex scene);

  [[nodiscard]] auto forward_pass(SceneIndex scene,
                                  int iteration,
                                  const SolverOptions& opts)
      -> std::expected<double, Error>;

  [[nodiscard]] auto backward_pass(SceneIndex scene, const SolverOptions& opts)
      -> std::expected<int, Error>;

  /// Update volume-dependent LP coefficients (turbine efficiency, etc.)
  /// before solving a phase in the forward pass.  Uses reservoir eini for
  /// the first iteration and the previous iteration's solved volumes for
  /// subsequent iterations.
  void update_coefficients_for_phase(SceneIndex scene,
                                     PhaseIndex phase,
                                     int iteration);

  /// Clone the LP, apply elastic filter on the clone, and solve it.
  /// Returns the solved clone (with solution data) if feasible, nullopt
  /// otherwise.  The original LP is never modified (PLP clone pattern).
  [[nodiscard]] std::optional<LinearInterface> elastic_solve(
      SceneIndex scene, PhaseIndex phase, const SolverOptions& opts);

  /// Check whether the sentinel file exists (user-requested stop)
  [[nodiscard]] bool check_sentinel_stop() const;

  /// Check all stop conditions: sentinel file, programmatic stop, callback
  [[nodiscard]] bool should_stop() const;

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
  LabelMaker m_label_maker_;
  scene_phase_states_t m_scene_phase_states_;
  std::vector<StoredCut> m_stored_cuts_;
  mutable std::mutex m_cuts_mutex_;  ///< Protects m_stored_cuts_

  /// Per-scene cut storage — each scene writes its own vector without
  /// needing the shared m_cuts_mutex_, preventing lock contention during
  /// parallel backward passes.
  StrongIndexVector<SceneIndex, std::vector<StoredCut>> m_scene_cuts_;

  bool m_initialized_ {false};

  // ── Stop / callback machinery ──
  SDDPIterationCallback m_iteration_callback_ {};
  std::atomic<bool> m_stop_requested_ {false};

  // ── Atomic live-query state ──
  std::atomic<int> m_current_iteration_ {0};
  std::atomic<double> m_current_gap_ {1.0};
  std::atomic<double> m_current_lb_ {0.0};
  std::atomic<double> m_current_ub_ {0.0};
  std::atomic<bool> m_converged_ {false};

  // ── Monitoring API state ──

  /// A single real-time sample point (CPU load, active workers, timestamp).
  struct MonitorPoint
  {
    double timestamp {};  ///< Seconds since solve() started
    double cpu_load {};  ///< CPU load percentage [0–100]
    int active_workers {};  ///< Number of active worker threads
  };

  mutable std::mutex m_realtime_mutex_;  ///< Protects m_realtime_history_
  std::vector<MonitorPoint> m_realtime_history_;  ///< Sampled workpool stats

  /// Write a JSON status file for the monitoring API.
  /// Called after each iteration (and from the monitoring background thread).
  /// @param status_file  Path to write the JSON file.
  /// @param results      Iteration results accumulated so far.
  /// @param elapsed_s    Seconds elapsed since solve() started.
  void write_api_status(const std::string& status_file,
                        const std::vector<SDDPIterationResult>& results,
                        double elapsed_s) const;

  /// Generate an LP name only when use_lp_names is enabled.
  template<typename... Args>
  [[nodiscard]] auto sddp_label(Args&&... args) const -> std::string
  {
    return m_label_maker_.lp_label(std::forward<Args>(args)...);
  }
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
