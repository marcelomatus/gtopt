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

#include <gtopt/aperture.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/benders_cut.hpp>
#include <gtopt/error.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/reservoir_efficiency_lp.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

// ─── Cut sharing mode ───────────────────────────────────────────────────────

/**
 * @brief How optimality cuts are shared between scenes at the same phase
 *
 * Four modes are supported:
 *
 * - `None`:       No sharing; cuts stay in their originating scene.
 * - `Expected`:   Probability-weighted average cut across scenes, added to
 *                 all scenes.  Correct when LP objectives do NOT include
 *                 probability factors.
 * - `Accumulate`: Sum all scenario cuts into one accumulated cut, added to
 *                 all scenes.  Correct when LP objectives already include
 *                 probability factors (each cut is pre-weighted).
 *                 Reference: Birge & Louveaux (2011) §5.1.
 * - `Max`:        All cuts from all scenes added to all scenes (default).
 */
enum class CutSharingMode : uint8_t
{
  None = 0,  ///< No sharing; cuts stay in their originating scene
  Expected,  ///< Probability-weighted average cut shared to all scenes
  Accumulate,  ///< Sum all cuts directly (LP objectives pre-weighted)
  Max,  ///< All cuts from all scenes added to all scenes
};

/// Parse a cut-sharing mode from a string
/// ("none", "expected", "accumulate", "max")
[[nodiscard]] CutSharingMode parse_cut_sharing_mode(std::string_view name);

// ─── Configuration ──────────────────────────────────────────────────────────

/// File naming patterns for per-scene cut files
namespace sddp_file
{
/// Combined cut file name
constexpr auto combined_cuts = "sddp_cuts.csv";
/// Per-scene cut file pattern: format with scene UID
constexpr auto scene_cuts_fmt = "scene_{}.csv";
/// Error-prefixed cut file pattern for infeasible scenes (scene UID)
constexpr auto error_scene_cuts_fmt = "error_scene_{}.csv";
/// Error LP file pattern for infeasible scene/phase (scene UID, phase UID)
constexpr auto error_lp_fmt = "error_scene_{}_phase_{}";
/// Debug LP file pattern: format with iteration, scene UID, phase UID
constexpr auto debug_lp_fmt = "gtopt_iter_{}_scene_{}_phase_{}";
/// Sentinel file name: if this file exists in the output directory, the
/// SDDP solver stops gracefully after the current iteration and saves cuts.
/// Created externally (e.g. by the webservice stop endpoint).
constexpr auto stop_sentinel = "sddp_stop";
/// Monitoring API stop-request file name: if this file exists, the solver
/// stops gracefully after the current iteration (same behaviour as the
/// sentinel file).  Written by the webservice soft-stop endpoint as part of
/// the bidirectional monitoring API.  Complements rather than replaces the
/// sentinel mechanism so that external scripts using the raw sentinel still
/// work.  The solver checks: sentinel_file exists || stop_request file exists.
constexpr auto stop_request = "sddp_stop_request.json";
}  // namespace sddp_file

// ─── Elastic filter mode ────────────────────────────────────────────────────

/**
 * @brief How the elastic filter handles feasibility issues in the backward pass
 *
 * When adding a Benders cut to phase k makes it infeasible, the elastic
 * filter can handle the situation in two ways:
 *
 * - `FeasibilityCut` / "single-cut" (default): clone the LP, relax the
 *   fixed state-variable bounds with penalised slack variables, solve the
 *   clone, and build a single feasibility-like Benders cut for phase k-1
 *   from the elastic clone's reduced costs.  This is the standard NBD
 *   approach.
 *
 * - `MultiCut` / "multi-cut": same as single-cut, but also adds one
 *   additional bound-constraint cut per state variable whose slack was
 *   activated (non-zero) in the elastic clone solution.  If the forward
 *   pass has encountered infeasibility at this (scene, phase) more than
 *   `multi_cut_threshold` times, the solver automatically switches from
 *   single-cut to multi-cut.
 *
 * - `BackpropagateBounds` (PLP mechanism): same clone/relax/solve as above,
 *   but instead of building a cut, propagate the slack-adjusted trial values
 *   back as updated bounds on the source state variables in phase k-1.
 *   Concretely, the source column in phase k-1 is tightened so that its
 *   upper and lower bounds equal the elastic-clone solution value for the
 *   dependent column.  This forces phase k-1 to produce a trial point that
 *   is known to be feasible for phase k, avoiding further infeasibility.
 *   This is the approach used in PLP (`osicallsc.cpp`).
 */
enum class ElasticFilterMode : uint8_t
{
  FeasibilityCut = 0,  ///< Build a single Benders feasibility cut (single-cut)
  MultiCut,  ///< Build a Benders cut + per-slack bound cuts (multi-cut)
  BackpropagateBounds,  ///< Update source bounds to elastic trial values (PLP)
};

/// Parse an elastic filter mode from a string.
/// Accepts "single-cut" or its backward-compatible alias "cut"
/// (= FeasibilityCut), "multi-cut" (= MultiCut), and "backpropagate"
/// (= BackpropagateBounds).
[[nodiscard]] ElasticFilterMode parse_elastic_filter_mode(
    std::string_view name);

/// Configuration options for the SDDP iterative solver
struct SDDPOptions
{
  int max_iterations {100};  ///< Maximum forward/backward iterations
  double convergence_tol {1e-4};  ///< Relative gap tolerance for convergence
  double elastic_penalty {1e6};  ///< Penalty for elastic slack variables
  double alpha_min {0.0};  ///< Lower bound for future cost variable α ($)
  double alpha_max {1e12};  ///< Upper bound for future cost variable α ($)
  CutSharingMode cut_sharing {CutSharingMode::Max};  ///< Cut sharing mode

  /// Elastic filter mode: how to handle backward-pass infeasibility.
  /// `FeasibilityCut` / "single-cut" (default) adds a single Benders
  /// feasibility cut to the previous phase.  `MultiCut` / "multi-cut" adds
  /// the same cut plus one bound-constraint cut per activated slack variable.
  /// `BackpropagateBounds` updates the source column bounds to match the
  /// elastic-clone solution (PLP mechanism).
  ElasticFilterMode elastic_filter_mode {ElasticFilterMode::FeasibilityCut};

  /// Forward-pass infeasibility counter threshold for automatic switching
  /// from single-cut to multi-cut.  When the forward pass has encountered
  /// infeasibility at (scene, phase) more than this many times without
  /// recovery, the backward-pass infeasibility handler switches to multi-cut
  /// mode for that (scene, phase).
  ///  = 0  always use multi-cut for any infeasibility (force multi-cut).
  ///  > 0  switch to multi-cut after the counter exceeds this threshold.
  ///  < 0  never auto-switch (disabled; use explicit mode only).
  /// Default: 10.
  int multi_cut_threshold {10};

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

  /// When true, save a debug LP file for every (iteration, scene, phase)
  /// during the forward pass to log_directory.
  /// Files are named using sddp_file::debug_lp_fmt.
  bool lp_debug {false};

  /// When true, build all scene×phase LP matrices and exit immediately —
  /// no solving of any kind is performed.  Applies to both the monolithic
  /// and SDDP solvers.  If lp_debug is also true, every LP file is saved
  /// before returning.  Useful for profiling LP build time without solver
  /// overhead.
  bool just_build_lp {false};

  /// Compression format for LP debug files ("gzip" / "uncompressed" / "").
  /// Empty or "uncompressed" means no compression; any other value uses gzip.
  std::string lp_debug_compression {};

  /// Enable the monitoring API: write a JSON status file after each iteration
  /// and periodically update real-time workpool statistics.  Consumers
  /// (e.g. sddp_monitor.py) can poll this file to display live charts.
  /// Default: true.
  bool enable_api {true};

  /// Path for the JSON status file.  If empty, the solver writes to
  /// "<output_directory>/sddp_status.json" (derived at solve time from the
  /// PlanningLP options).
  std::string api_status_file {};

  /// Path for the monitoring API stop-request file.  When this file exists
  /// the solver stops gracefully after the current iteration and saves cuts,
  /// exactly like the sentinel_file mechanism.  The file is written by the
  /// webservice soft-stop endpoint as part of the bidirectional monitoring
  /// API.  Use sddp_file::stop_request ("sddp_stop_request.json") as the
  /// filename in the output directory.  Empty = feature disabled.
  std::string api_stop_request_file {};

  /// Interval at which the background monitoring thread refreshes real-time
  /// workpool statistics (CPU load, active workers) in the status file.
  std::chrono::milliseconds api_update_interval {500};

  /// Number of apertures (hydrological realisations) to solve in each
  /// backward-pass phase.  Each aperture clones the phase LP and updates
  /// the flow column bounds to the corresponding scenario's discharge values,
  /// then solves the clone to obtain an independent Benders cut.  The final
  /// cut added to the previous phase is the probability-weighted average of
  /// all aperture cuts (expected cut).
  ///
  ///  0  – disabled; use the cached forward-pass solution (default, current
  ///       behaviour).
  /// -1  – use all available scenarios as apertures (one-to-one mapping).
  ///  N > 0 – use the first N scenarios as apertures (capped at the total
  ///          number of scenarios defined in the simulation).
  ///
  /// Note: apertures only update flow column bounds (affluent values).
  /// Other stochastic parameters (demand, generator profiles) are not
  /// updated.  State variable bounds remain fixed at the forward-pass
  /// trial values.
  int num_apertures {0};

  /// CSV file with boundary (future-cost) cuts for the last phase.
  ///
  /// These cuts approximate the expected future cost beyond the planning
  /// horizon, analogous to PLP's "planos de embalse" (reservoir future-cost
  /// function).  Each cut has the form:
  ///   α ≥ rhs + Σ_i coeff_i · state_var_i
  ///
  /// The CSV header row names the state variables (reservoirs / batteries).
  /// The solver maps these names to LP columns in the last phase and adds
  /// each cut as a lower-bound constraint on the future cost variable α.
  /// Empty = no boundary cuts.
  std::string boundary_cuts_file {};

  /// How boundary cuts are loaded:
  /// - "noload"    — skip loading even if a file is specified
  /// - "separated" — assign each cut to the scene matching its `scene`
  ///                 column (scene UID); unmatched UIDs are skipped
  /// - "combined"  — broadcast all cuts to all scenes
  /// Default: "separated".
  std::string boundary_cuts_mode {"separated"};

  /// Maximum number of SDDP iterations to load from the boundary cuts
  /// file.  Only cuts from the last N distinct iterations (by the
  /// `iteration` column / PLP IPDNumIte) are retained.  0 = load all.
  int boundary_max_iterations {0};
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

  /// Wall-clock time in seconds for the forward pass (all scenes).
  double forward_pass_s {};
  /// Wall-clock time in seconds for the backward pass (all scenes).
  double backward_pass_s {};
  /// Total wall-clock time in seconds for this iteration.
  double iteration_s {};

  /// Number of successful elastic-filter solves this iteration.
  /// Each elastic-filter solve corresponds to an LP infeasibility event;
  /// in the backward pass these become Benders feasibility cuts.
  int infeasible_cuts_added {};

  /// Per-scene upper bounds (forward-pass costs).  Size = num_scenes.
  std::vector<double> scene_upper_bounds {};
  /// Per-scene lower bounds (phase-0 objective values).  Size = num_scenes.
  std::vector<double> scene_lower_bounds {};
};

// ─── Utility free functions (independently testable) ─────────────────────────

/// Compute normalised per-scene probability weights.
///
/// For each scene: weight = sum of scenario probability_factors if positive,
/// else 1.0.  Infeasible scenes (scene_feasible[si]==0) get weight 0.
/// Weights are normalised to sum to 1 across feasible scenes.
/// Falls back to equal weights when no positive probabilities are found.
///
/// @param scenes         The scene objects from SimulationLP
/// @param scene_feasible Per-scene feasibility flag (0 = infeasible);
///                       output size equals scene_feasible.size()
/// @returns Normalised weight vector of size scene_feasible.size()
[[nodiscard]] std::vector<double> compute_scene_weights(
    std::span<const SceneLP> scenes,
    std::span<const uint8_t> scene_feasible) noexcept;

/// Compute relative convergence gap: (UB - LB) / max(1.0, |UB|).
/// Always returns a non-negative value.
[[nodiscard]] double compute_convergence_gap(double upper_bound,
                                             double lower_bound) noexcept;

// ─── State variable linkage, elastic filter, and cut functions ──────────────
// Now provided by <gtopt/benders_cut.hpp> — included above.
// The following types and functions are available via that header:
//   StateVarLink, RelaxedVarInfo, ElasticSolveResult, FeasibilityCutResult,
//   propagate_trial_values(), build_benders_cut(),
//   relax_fixed_state_variable(), elastic_filter_solve(),
//   build_feasibility_cut(), build_multi_cuts(), average_benders_cut(),
//   weighted_average_benders_cut()

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
  int phase {};  ///< Phase UID this cut was added to
  int scene {};  ///< Scene UID that generated this cut (-1 = shared)
  std::string name {};  ///< Cut name
  double rhs {};  ///< Right-hand side (lower bound)
  /// Coefficient pairs: (column_index, coefficient)
  std::vector<std::pair<int, double>> coefficients {};
};

// ─── Callback / observer API ────────────────────────────────────────────────

/// Callback invoked after each SDDP iteration.
/// If the callback returns `true`, the solver stops after this iteration.
using SDDPIterationCallback =
    std::function<bool(const SDDPIterationResult& result)>;

/// Outcome of running the forward pass across all scenes
struct ForwardPassOutcome
{
  std::vector<double> scene_upper_bounds {};
  std::vector<uint8_t> scene_feasible {};
  int scenes_solved {0};
  bool has_feasibility_issue {false};
  double elapsed_s {0.0};
};

/// Outcome of running the backward pass across all scenes
struct BackwardPassOutcome
{
  int total_cuts {0};
  bool has_feasibility_issue {false};
  double elapsed_s {0.0};
};

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

  /// Load boundary (future-cost) cuts from a named-variable CSV file.
  ///
  /// The CSV header names the state variables (e.g. reservoir or battery
  /// names); subsequent rows provide {name, scenario, rhs, coefficients}.
  /// Cuts are added only to the last phase, with an alpha column created
  /// if needed.  This is analogous to PLP's "planos de embalse".
  ///
  /// @return Number of cuts loaded, or an error.
  [[nodiscard]] auto load_boundary_cuts(const std::string& filepath)
      -> std::expected<int, Error>;

private:
  using scene_phase_states_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, PhaseStateInfo>>;

  /// Type alias for backward compatibility — now uses the public
  /// ElasticSolveResult from benders_cut.hpp.
  using ElasticResult = ElasticSolveResult;

  void initialize_alpha_variables(SceneIndex scene);
  void collect_state_variable_links(SceneIndex scene);

  [[nodiscard]] auto forward_pass(SceneIndex scene,
                                  int iteration,
                                  const SolverOptions& opts)
      -> std::expected<double, Error>;

  [[nodiscard]] auto backward_pass(SceneIndex scene,
                                   const SolverOptions& opts,
                                   int iteration = 0)
      -> std::expected<int, Error>;

  /**
   * @brief Backward pass with apertures (hydrological realisations).
   *
   * For each phase in the backward pass (from last to first), this method
   * solves the phase LP once per aperture.  Each aperture corresponds to
   * one of the available simulation scenarios; the flow column bounds are
   * updated to the aperture scenario's discharge values before solving.
   * The probability-weighted average of all aperture cuts (expected cut) is
   * then added to the previous phase's LP.
   *
   * Apertures use the same state-variable trial values as the original
   * forward pass (only the flow bounds change).  If a specific aperture LP
   * is infeasible the aperture is skipped and its probability weight is
   * discarded.
   *
   * Falls back to the regular backward_pass() if num_apertures is 0 or no
   * scenarios are available.
   *
   * Uses the work pool for parallel aperture solves when available.
   */
  [[nodiscard]] auto backward_pass_with_apertures(SceneIndex scene,
                                                  const SolverOptions& opts,
                                                  int iteration = 0)
      -> std::expected<int, Error>;

  /// Update volume-dependent LP coefficients (turbine efficiency, etc.)
  /// before solving a phase in the forward pass.  Uses reservoir eini for
  /// the first iteration and the previous iteration's solved volumes for
  /// subsequent iterations.
  void update_coefficients_for_phase(SceneIndex scene,
                                     PhaseIndex phase,
                                     int iteration);

  /// Clone the LP, apply elastic filter on the clone, and solve it.
  /// Returns an ElasticResult (with solution data and per-link slack info)
  /// if feasible, nullopt otherwise.
  /// The original LP is never modified (PLP clone pattern).
  [[nodiscard]] std::optional<ElasticResult> elastic_solve(
      SceneIndex scene, PhaseIndex phase, const SolverOptions& opts);

  // ── Refactored helper methods ──

  /// Store a cut for sharing and persistence (thread-safe).
  /// Writes to both per-scene storage and shared storage.
  void store_cut(SceneIndex scene, PhaseIndex src_phase, const SparseRow& cut);

  /// Resolve an LP via the SDDP work pool.  Falls back to direct resolve if
  /// the pool is not available.  Avoids naked direct resolve() calls.
  [[nodiscard]] auto resolve_via_pool(
      LinearInterface& li,
      const SolverOptions& opts,
      const BasicTaskRequirements<SDDPTaskKey>& task_req = {})
      -> std::expected<int, Error>;

  /// Resolve a cloned LP via the SDDP work pool.  The clone is moved into a
  /// shared_ptr for the pool task, then moved back after completion.
  [[nodiscard]] auto resolve_clone_via_pool(
      LinearInterface& clone,
      const SolverOptions& opts,
      const BasicTaskRequirements<SDDPTaskKey>& task_req = {})
      -> std::expected<int, Error>;

  /// Iterative feasibility backpropagation: propagate from start_phase
  /// backward to phase 0 using elastic filter and cuts.
  /// Returns the number of additional cuts added.
  [[nodiscard]] auto feasibility_backpropagate(SceneIndex scene,
                                               Index start_phase,
                                               int total_cuts,
                                               const SolverOptions& opts)
      -> std::expected<int, Error>;

  /// Solve all apertures for a single phase and return the
  /// probability-weighted expected cut, or nullopt if all failed.
  /// When @p phase_apertures is non-empty, only those aperture UIDs are used;
  /// otherwise all apertures from @p aperture_defs participate.
  [[nodiscard]] auto solve_apertures_for_phase(
      SceneIndex scene,
      PhaseIndex phase,
      const PhaseStateInfo& src_state,
      const ScenarioLP& base_scenario,
      std::span<const ScenarioLP> all_scenarios,
      std::span<const Aperture> aperture_defs,
      std::span<const Uid> phase_apertures,
      int total_cuts,
      const SolverOptions& opts) -> std::optional<SparseRow>;

  /// Check whether the sentinel file exists (user-requested stop)
  [[nodiscard]] bool check_sentinel_stop() const;

  /// Check whether the monitoring API stop-request file exists
  [[nodiscard]] bool check_api_stop_request() const;

  /// Check all stop conditions: sentinel file, API stop request, programmatic
  /// stop, callback
  [[nodiscard]] bool should_stop() const;

  /// Apply cut sharing across scenes for a given phase
  void share_cuts_for_phase(
      PhaseIndex phase,
      const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts);

  /// Validate that the simulation has ≥2 phases and ≥1 scene.
  [[nodiscard]] auto validate_inputs() const -> std::optional<Error>;

  /// Bootstrap-solve + initialize α variables, state links, and hot-start.
  /// Called once (guarded by m_initialized_).
  [[nodiscard]] auto initialize_solver() -> std::expected<void, Error>;

  /// Reset live-query atomics to their start-of-solve values.
  void reset_live_state() noexcept;

  /// Run the forward pass for all scenes in parallel.
  /// Returns a ForwardPassOutcome or an error if ALL scenes failed.
  [[nodiscard]] auto run_forward_pass_all_scenes(int iter,
                                                 SDDPWorkPool& pool,
                                                 const SolverOptions& opts)
      -> std::expected<ForwardPassOutcome, Error>;

  /// Run the backward pass for all feasible scenes in parallel.
  [[nodiscard]] auto run_backward_pass_all_scenes(
      std::span<const uint8_t> scene_feasible,
      SDDPWorkPool& pool,
      const SolverOptions& opts,
      int iter) -> BackwardPassOutcome;

  /// Compute and fill ir.upper_bound, ir.lower_bound, ir.scene_lower_bounds.
  void compute_iteration_bounds(SDDPIterationResult& ir,
                                std::span<const uint8_t> scene_feasible,
                                std::span<const double> weights) const;

  /// Apply cut-sharing across scenes for all phases generated in this
  /// iteration.  @param cuts_before is m_stored_cuts_.size() BEFORE this
  /// iteration's backward pass.
  void apply_cut_sharing_for_iteration(std::size_t cuts_before);

  /// Compute gap, update convergence flag, update live-query atomics, log.
  void finalize_iteration_result(SDDPIterationResult& ir, int iter);

  /// Write the monitoring API status file if API is enabled.
  void maybe_write_api_status(const std::string& status_file,
                              const std::vector<SDDPIterationResult>& results,
                              std::chrono::steady_clock::time_point solve_start,
                              const SolverMonitor& monitor) const;

  /// Save cuts (combined + per-scene) after an iteration, handling infeasible
  /// scene renaming.
  void save_cuts_for_iteration(int iter,
                               std::span<const uint8_t> scene_feasible);

  // Accessor for the wrapped PlanningLP reference (avoids raw reference member)
  [[nodiscard]] PlanningLP& planning_lp() noexcept
  {
    return m_planning_lp_.get();
  }
  [[nodiscard]] const PlanningLP& planning_lp() const noexcept
  {
    return m_planning_lp_.get();
  }

  /// Get the scene UID for a given SceneIndex.
  [[nodiscard]] int scene_uid(SceneIndex si) const noexcept
  {
    return static_cast<int>(planning_lp().simulation().scenes()[si].uid());
  }

  /// Get the phase UID for a given PhaseIndex.
  [[nodiscard]] int phase_uid(PhaseIndex pi) const noexcept
  {
    return static_cast<int>(planning_lp().simulation().phases()[pi].uid());
  }

  std::reference_wrapper<PlanningLP> m_planning_lp_;
  SDDPOptions m_options_;
  LabelMaker m_label_maker_;
  scene_phase_states_t m_scene_phase_states_;
  std::vector<StoredCut> m_stored_cuts_ {};
  mutable std::mutex m_cuts_mutex_;  ///< Protects m_stored_cuts_

  /// Per-scene cut storage — each scene writes its own vector without
  /// needing the shared m_cuts_mutex_, preventing lock contention during
  /// parallel backward passes.
  StrongIndexVector<SceneIndex, std::vector<StoredCut>> m_scene_cuts_ {};

  /// Per-(scene, phase) count of consecutive forward-pass infeasibilities.
  /// Incremented when the elastic filter is used in forward_pass at (scene,
  /// phase).  Reset to 0 when the phase is solved normally (no elastic).
  /// Used by the backward pass to decide single-cut vs multi-cut mode.
  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, int>>
      m_infeasibility_counter_;

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

  // ── BendersCut: wraps elastic-filter LP solves via the work pool ──
  /// Constructed with null pool; updated in solve() once the pool is created.
  /// Uses m_aux_pool_ (AdaptiveWorkPool) for LP solve monitoring.
  BendersCut m_benders_cut_;

  /// Non-owning pointer to the SDDP work pool (SDDPWorkPool) created in
  /// solve().  Uses SDDPTaskKey for tuple-based priority ordering.
  /// Set to non-null while solve() is running; reset to nullptr on return.
  /// Used by resolve_via_pool() and resolve_clone_via_pool().
  SDDPWorkPool* m_pool_ {nullptr};

  /// Non-owning pointer to the auxiliary AdaptiveWorkPool used by
  /// BendersCut (elastic LP solves) and LpDebugWriter (gzip compression).
  /// Separate from m_pool_ so that those helpers continue to use the
  /// simple int64_t key while the main SDDP LP solves use SDDPTaskKey.
  AdaptiveWorkPool* m_aux_pool_ {nullptr};

  /// LP debug writer — active when lp_debug is enabled and log_directory is
  /// set.  Initialised at the start of solve() and drained at the end.
  LpDebugWriter m_lp_debug_writer_ {};

  // ── Monitoring API (SolverMonitor owns the background thread) ──

  /// Write a JSON status file for the monitoring API.
  /// Called after each iteration.
  /// @param status_file  Path to write the JSON file.
  /// @param results      Iteration results accumulated so far.
  /// @param elapsed_s    Seconds elapsed since solve() started.
  /// @param monitor      The SolverMonitor whose history to include.
  void write_api_status(const std::string& status_file,
                        const std::vector<SDDPIterationResult>& results,
                        double elapsed_s,
                        const SolverMonitor& monitor) const;

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
  std::vector<SDDPIterationResult> m_last_results_ {};
};

}  // namespace gtopt
