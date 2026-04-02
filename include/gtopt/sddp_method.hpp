/**
 * @file      sddp_method.hpp
 * @brief     SDDP (Stochastic Dual Dynamic Programming) method for multi-phase
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/aperture.hpp>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/benders_cut.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/error.hpp>
#include <gtopt/iteration_lp.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_clone_pool.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

// ─── Cut sharing mode ───────────────────────────────────────────────────────
// CutSharingMode is now defined in <gtopt/sddp_enums.hpp>.
// The generic enum_from_name<CutSharingMode>() replaces the old
// parse_cut_sharing_mode() free function.

/// Parse a cut-sharing mode from a string (backward-compatible wrapper).
/// ("none", "expected", "accumulate", "max")
[[nodiscard]] CutSharingMode parse_cut_sharing_mode(std::string_view name);

// ─── Configuration ──────────────────────────────────────────────────────────

/// File naming patterns for per-scene cut files
namespace sddp_file
{
/// Combined cut file name
constexpr auto combined_cuts = "sddp_cuts.csv";
/// Versioned cut file pattern: format with iteration number
constexpr auto versioned_cuts_fmt = "sddp_cuts_{}.csv";
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
/// State variable column solution (latest)
constexpr auto state_cols = "sddp_state.csv";
/// Versioned state column solution: format with iteration number
constexpr auto versioned_state_fmt = "sddp_state_{}.csv";
/// Monitoring API stop-request file name: if this file exists, the solver
/// stops gracefully after the current iteration (same behaviour as the
/// sentinel file).  Written by the webservice soft-stop endpoint as part of
/// the bidirectional monitoring API.  Complements rather than replaces the
/// sentinel mechanism so that external scripts using the raw sentinel still
/// work.  The solver checks: sentinel_file exists || stop_request file exists.
constexpr auto stop_request = "sddp_stop_request.json";
}  // namespace sddp_file

// ─── Elastic filter mode ────────────────────────────────────────────────────
// ElasticFilterMode is now defined in <gtopt/sddp_enums.hpp>.
// The generic enum_from_name<ElasticFilterMode>() replaces the old
// parse_elastic_filter_mode() free function.

/// Parse an elastic filter mode from a string (backward-compatible wrapper).
/// Accepts "single_cut" / "cut" (= single_cut), "multi_cut",
/// "backpropagate".
[[nodiscard]] ElasticFilterMode parse_elastic_filter_mode(
    std::string_view name);

/// Configuration options for the SDDP iterative solver
struct SDDPOptions  // NOLINT(clang-analyzer-optin.performance.Padding)
{
  int max_iterations {100};  ///< Maximum forward/backward iterations
  int min_iterations {2};  ///< Minimum iterations before convergence
  double convergence_tol {1e-4};  ///< Relative gap tolerance for convergence
  double elastic_penalty {1e6};  ///< Penalty for elastic slack variables
  double alpha_min {0.0};  ///< Lower bound for future cost variable α ($)
  double alpha_max {1e15};  ///< Upper bound for future cost variable α ($)
  double scale_alpha {10'000'000};  ///< Scale divisor for α (PLP varphi scale)
  CutSharingMode cut_sharing {CutSharingMode::none};  ///< Cut sharing mode

  /// Elastic filter mode: how to handle backward-pass infeasibility.
  /// `single_cut` (default) adds a single Benders feasibility cut to the
  /// previous phase.  `multi_cut` adds the same cut plus one
  /// bound-constraint cut per activated slack variable.
  /// `backpropagate` updates the source column bounds to match the
  /// elastic-clone solution (PLP mechanism).
  ElasticFilterMode elastic_filter_mode {ElasticFilterMode::single_cut};

  /// How Benders cut coefficients are extracted from solved subproblems.
  /// `reduced_cost` (default): reduced costs of fixed dependent columns.
  /// `row_dual`: row duals of explicit coupling constraint rows (PLP-style).
  CutCoeffMode cut_coeff_mode {CutCoeffMode::reduced_cost};

  /// Absolute tolerance for filtering tiny Benders cut coefficients.
  /// Coefficients with |value| < cut_coeff_eps are dropped from the cut.
  /// 0.0 = no filtering (default).
  double cut_coeff_eps {0.0};

  /// Maximum allowed absolute coefficient in a Benders cut row.
  /// When max|coeff| exceeds this, the entire row is rescaled uniformly.
  /// 0.0 = disabled (default).
  double cut_coeff_max {0.0};

  /// Forward-pass infeasibility counter threshold for automatic switching
  /// from single_cut to multi_cut.  When the forward pass has encountered
  /// infeasibility at (scene, phase) more than this many times without
  /// recovery, the backward-pass infeasibility handler switches to multi_cut
  /// mode for that (scene, phase).
  ///  = 0  always use multi_cut for any infeasibility (force multi_cut).
  ///  > 0  switch to multi_cut after the counter exceeds this threshold.
  ///  < 0  never auto-switch (disabled; use explicit mode only).
  /// Default: 10.
  int multi_cut_threshold {10};

  /// Save cuts to CSV after each training iteration (default: true).
  /// When false, cuts are only saved at the end of the solve or on stop.
  bool save_per_iteration {true};

  /// Save feasibility cuts produced during the simulation pass (default:
  /// false).  When false, only training-iteration cuts are persisted,
  /// ensuring hot-start reproducibility.
  bool save_simulation_cuts {false};

  /// Global solve timeout in seconds (0 = no timeout).
  /// When non-zero, each forward-pass LP solve is given this time limit;
  /// if exceeded, the LP is saved to a debug file, a CRITICAL message is
  /// logged, and the scene is marked as failed.
  double solve_timeout {0.0};

  /// File path for saving cuts (empty = no save)
  std::string cuts_output_file {};
  /// File path for loading initial cuts (empty = no load / cold start)
  std::string cuts_input_file {};
  /// Hot-start mode: controls both cut loading and output file handling.
  /// - `none`:    cold start — no cuts loaded (default)
  /// - `keep`:    load cuts; keep original output file unchanged
  /// - `append`:  load cuts; append new cuts to original file
  /// - `replace`: load cuts; replace original file with all cuts
  HotStartMode cut_recovery_mode {HotStartMode::none};

  /// Controls what is recovered from a previous SDDP run:
  /// - `none`:  no recovery (cold start)
  /// - `cuts`:  recover only Benders cuts
  /// - `full`:  recover cuts + state variable solutions (default)
  RecoveryMode recovery_mode {RecoveryMode::full};

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
  bool lp_build {false};

  /// Compression format for LP debug files ("gzip" / "uncompressed" / "").
  /// Empty or "uncompressed" means no compression; any other value uses gzip.
  std::string lp_debug_compression {};

  /// Selective LP debug filters: when set, only save LP files whose
  /// scene/phase UIDs fall within [min, max] (inclusive).
  OptInt lp_debug_scene_min {};
  OptInt lp_debug_scene_max {};
  OptInt lp_debug_phase_min {};
  OptInt lp_debug_phase_max {};

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
  /// Aperture UIDs for the backward pass.
  ///
  ///  nullopt – use per-phase `Phase::apertures` or simulation-level
  ///            `aperture_array` (default behaviour).
  ///  empty   – no apertures; use pure Benders backward pass.
  ///  [1,2,3] – use exactly these aperture UIDs, overriding per-phase sets.
  ///
  /// When a non-empty list is given but no matching `Aperture` definitions
  /// exist in `simulation.aperture_array`, synthetic apertures are built
  /// from scenarios whose UIDs match.
  ///
  /// Note: apertures only update flow column bounds (affluent values).
  /// Other stochastic parameters (demand, generator profiles) are not
  /// updated.  State variable bounds remain fixed at the forward-pass
  /// trial values.
  std::optional<std::vector<Uid>> apertures {};

  /// Timeout in seconds for individual aperture LP solves in the backward
  /// pass.  When an aperture LP exceeds this time, it is treated as
  /// infeasible (skipped), a WARNING is logged, and the solver continues
  /// with the remaining apertures.  0 = no timeout (default).
  double aperture_timeout {0.0};

  /// Save LP files for infeasible apertures to log_directory (default: false).
  bool save_aperture_lp {false};

  /// Enable warm-start optimizations for SDDP resolves (forward pass,
  /// backward pass, apertures, elastic filter).  When true, resolves use
  /// dual simplex + no presolve, pivoting from the saved forward-pass
  /// solution.  Especially important when the initial solve uses barrier
  /// (the default).
  bool warm_start {true};

  /// Maximum number of retained cuts per (scene, phase) LP after pruning.
  /// 0 = unlimited (default, no pruning).  When non-zero, at every
  /// cut_prune_interval iterations the solver removes inactive cuts
  /// (|dual| < prune_dual_threshold) until at most max_cuts_per_phase
  /// active cuts remain.
  int max_cuts_per_phase {0};

  /// Number of iterations between cut pruning passes.
  /// Only used when max_cuts_per_phase > 0.  Default: 10.
  int cut_prune_interval {10};

  /// Dual-value threshold for considering a cut inactive.
  /// Cuts with |dual| below this value are candidates for removal.
  /// Default: 1e-8.
  double prune_dual_threshold {1e-8};

  /// Use single cut storage: store cuts only in per-scene vectors.
  /// Combined storage for persistence is built on demand from the
  /// per-scene vectors.  Halves the memory cost of stored cuts.
  /// Default: false (backward compatible).
  bool single_cut_storage {false};

  /// Maximum total stored cuts per scene (0 = unlimited).  When
  /// non-zero, the oldest cuts beyond this limit are dropped after
  /// each iteration.  Default: 0 (no cap).
  int max_stored_cuts {0};

  /// Reuse a cached LP clone for aperture solves instead of cloning
  /// anew each time.  The clone's column bounds are reset from the
  /// source LP before each solve; rows beyond the base count are
  /// deleted.  Avoids repeated heap allocations for the CLP solver
  /// internal matrix.  Default: true.
  bool use_clone_pool {true};

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
  BoundaryCutsMode boundary_cuts_mode {BoundaryCutsMode::separated};

  /// Maximum number of SDDP iterations to load from the boundary cuts
  /// file.  Only cuts from the last N distinct iterations (by the
  /// `iteration` column / PLP IPDNumIte) are retained.  0 = load all.
  int boundary_max_iterations {0};

  /// How to handle cut rows referencing state variables not in the model.
  MissingCutVarMode missing_cut_var_mode {MissingCutVarMode::skip_coeff};

  /// CSV file with named-variable hot-start cuts for all phases.
  ///
  /// Unlike boundary cuts (which apply only to the last phase), these
  /// cuts include a `phase` column indicating which phase they belong to.
  /// The solver resolves named state-variable headers (reservoir / battery /
  /// junction) to LP column indices in the specified phase, then adds each
  /// cut as a lower-bound constraint on the corresponding α variable:
  ///   α_phase ≥ rhs + Σ_i coeff_i · state_var_i[phase]
  ///
  /// Format:
  ///   name,iteration,scene,phase,rhs,StateVar1,StateVar2,...
  ///
  /// Empty = no named hot-start cuts.
  std::string named_cuts_file {};

  // ── Secondary (stationary gap) convergence ──────────────────────────────

  /// Tolerance for the secondary stationary-gap convergence criterion.
  ///
  /// When the relative change in the convergence gap over the last
  /// `stationary_window` iterations falls below this value, the solver
  /// declares convergence even if the gap is above `convergence_tol`.
  /// This handles problems where the SDDP gap converges to a non-zero
  /// stationary value due to stochastic noise or problem structure
  /// (a known theoretical limitation of SDDP/Benders on certain programs).
  ///
  /// Criterion (after min_iterations and stationary_window iters done):
  ///   gap_change = |gap[i] − gap[i − window]| / max(1e-10, gap[i − window])
  ///   gap_change < stationary_tol → declare convergence
  ///
  /// Convergence criterion mode.  Default: statistical (PLP-style).
  ConvergenceMode convergence_mode {ConvergenceMode::statistical};

  /// Default: 0.01 (1%).  Set to 0.0 to disable.
  double stationary_tol {0.01};

  /// Number of iterations to look back when checking gap stationarity.
  /// Only used when stationary_tol > 0.0.  Default: 10.
  int stationary_window {10};

  /// Confidence level for statistical convergence criterion (0-1).
  /// When > 0 and multiple scenes exist, convergence is also checked via
  /// confidence interval: UB - LB <= z_{α/2} * σ (PLP-style).
  /// Combined with stationary_tol, also declares convergence when the
  /// gap stabilises above the CI threshold (non-zero gap case).
  /// Default: 0.95 (95% CI).
  double convergence_confidence {0.95};

  /// Optional LP solver options for the forward pass.
  /// When set, these override the global solver options for forward-pass
  /// solves.  The options are pre-merged with the global solver options at
  /// construction time (forward takes precedence).
  std::optional<SolverOptions> forward_solver_options {};

  /// Optional LP solver options for the backward pass.
  /// When set, these override the global solver options for backward-pass
  /// solves.  The options are pre-merged with the global solver options at
  /// construction time (backward takes precedence).
  std::optional<SolverOptions> backward_solver_options {};
};

// ─── Iteration result ───────────────────────────────────────────────────────

/// Result of a single SDDP iteration (forward + backward pass)
struct SDDPIterationResult
{
  IterationIndex iteration {};  ///< Iteration number (0-based)
  double lower_bound {};  ///< Lower bound (phase 0 objective including α)
  double upper_bound {};  ///< Upper bound (sum of actual phase costs)
  double gap {};  ///< Relative gap: (UB − LB) / max(1, |UB|)
  /// Relative change in gap vs. `stationary_window` iterations ago.
  /// Populated only when `stationary_tol > 0` and enough iterations have
  /// elapsed; 1.0 otherwise (meaning "not yet checked / not applicable").
  double gap_change {1.0};
  bool converged {};  ///< True if gap < convergence tolerance
  /// True when convergence was declared by the stationary-gap criterion
  /// (gap_change < stationary_tol) rather than the primary criterion.
  bool stationary_converged {};
  /// True when convergence was declared by the statistical CI criterion
  /// (|UB - LB| <= z_{α/2} * σ / √N) rather than the primary criterion.
  bool statistical_converged {};
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
/// @param rescale_mode   When `runtime`, normalize weights over feasible
///                       scenes to sum 1.0.  When `build` or `none`, use
///                       raw probability weights (no re-normalization).
/// @returns Weight vector of size scene_feasible.size()
[[nodiscard]] std::vector<double> compute_scene_weights(
    std::span<const SceneLP> scenes,
    std::span<const uint8_t> scene_feasible,
    ProbabilityRescaleMode rescale_mode =
        ProbabilityRescaleMode::runtime) noexcept;

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
  size_t base_nrows {0};  ///< Row count before any Benders cuts were added
  double forward_objective {0.0};  ///< Opex from last forward pass
  /// Full LP objective from last forward solve (including α).
  /// Cached for the backward pass so the original LP need not be re-queried.
  double forward_full_obj {0.0};
  /// Reduced costs from last forward solve (cached for backward pass).
  std::vector<double> forward_col_cost {};
  /// Primal solution from last forward solve (warm-start for apertures).
  std::vector<double> forward_col_sol {};
  /// Dual solution from last forward solve (warm-start for apertures).
  std::vector<double> forward_row_dual {};
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

// ─── SDDPMethod ─────────────────────────────────────────────────────────────

/**
 * @class SDDPMethod
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
 * SDDPMethod sddp(planning_lp, SDDPOptions{.max_iterations = 100});
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
class SDDPMethod
{
public:
  explicit SDDPMethod(PlanningLP& planning_lp, SDDPOptions opts = {}) noexcept;

  /// Run the SDDP iterative solve
  [[nodiscard]] auto solve(const SolverOptions& lp_opts = {})
      -> std::expected<std::vector<SDDPIterationResult>, Error>;

  /// Ensure the solver is initialized (alpha variables, state links, etc.).
  /// Safe to call multiple times (guarded by m_initialized_ flag).
  /// Call before loading external cuts that reference alpha columns.
  [[nodiscard]] auto ensure_initialized() -> std::expected<void, Error>
  {
    return initialize_solver();
  }

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

  /// Current pass: 0=idle, 1=forward, 2=backward
  [[nodiscard]] int current_pass() const noexcept
  {
    return m_current_pass_.load();
  }

  /// Number of scenes completed in the current pass
  [[nodiscard]] int scenes_done() const noexcept
  {
    return m_scenes_done_.load();
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

  /// SDDP options (const)
  [[nodiscard]] constexpr auto& options() const noexcept { return m_options_; }

  /// Mutable options access (for cascade orchestration between solve() calls).
  [[nodiscard]] constexpr auto& mutable_options() noexcept
  {
    return m_options_;
  }

  /// Clear all stored cut metadata (combined + per-scene).
  /// Used by CascadePlanningMethod between cascade phases.
  void clear_stored_cuts() noexcept;

  /// Remove the first @p count cuts from stored cuts and from the LP.
  /// This deletes the LP rows associated with those cuts (via
  /// `delete_rows`) and erases them from `m_stored_cuts_`.  Used by
  /// the cascade solver's "forget inherited cuts" feature: the first
  /// N cuts loaded via hot-start are dropped so the solver continues
  /// with only self-generated cuts.
  ///
  /// @param count  Number of leading cuts to remove (clamped to size).
  void forget_first_cuts(int count);

  /// Update dual values of stored cuts from the current LP solution.
  /// Call after the solver finishes to populate the dual field in each
  /// StoredCut with the row dual from the last forward-pass solve.
  void update_stored_cut_duals();

  /// All stored cuts (for persistence / inspection)
  [[nodiscard]] const auto& stored_cuts() const noexcept
  {
    return m_cut_store_.stored_cuts();
  }

  /// Number of stored cuts (thread-safe).
  /// In single_cut_storage mode, counts across all per-scene vectors.
  [[nodiscard]] int num_stored_cuts() const noexcept
  {
    return m_cut_store_.num_stored_cuts(m_options_.single_cut_storage);
  }

  /// Access the cut store (for cascade orchestration, etc.).
  [[nodiscard]] SDDPCutStore& cut_store() noexcept { return m_cut_store_; }

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
      -> std::expected<CutLoadResult, Error>;

  /// Load all per-scene cut files from a directory.
  /// Files matching `scene_<N>.csv` are loaded; files with the `error_`
  /// prefix (from infeasible scenes in a previous run) are skipped to
  /// prevent loading invalid cuts during hot-start.
  [[nodiscard]] auto load_scene_cuts_from_directory(
      const std::string& directory) -> std::expected<CutLoadResult, Error>;

  /// Load boundary (future-cost) cuts from a named-variable CSV file.
  ///
  /// The CSV header names the state variables (e.g. reservoir or battery
  /// names); subsequent rows provide {name, scenario, rhs, coefficients}.
  /// Cuts are added only to the last phase, with an alpha column created
  /// if needed.  This is analogous to PLP's "planos de embalse".
  ///
  /// @return CutLoadResult with count and max iteration, or an error.
  [[nodiscard]] auto load_boundary_cuts(const std::string& filepath)
      -> std::expected<CutLoadResult, Error>;

  /// Load named-variable cuts from a CSV file with a `phase` column.
  ///
  /// Unlike `load_boundary_cuts()` (which loads into the last phase only),
  /// this method resolves named state-variable headers in each specified
  /// phase and adds the cuts to the corresponding phase LP.  The CSV
  /// format is:
  ///   name,iteration,scene,phase,rhs,StateVar1,StateVar2,...
  ///
  /// This is used for hot-start from PLP planos data where cuts span
  /// multiple stages (mapped to gtopt phases).
  ///
  /// @return CutLoadResult with count and max iteration, or an error.
  [[nodiscard]] auto load_named_cuts(const std::string& filepath)
      -> std::expected<CutLoadResult, Error>;

  /// Save state variable column solutions and reduced costs to a CSV file.
  /// Writes one row per column with its name, phase, scene, value, and
  /// reduced cost.  Saved alongside cuts for hot-start state restoration.
  [[nodiscard]] auto save_state(const std::string& filepath) const
      -> std::expected<void, Error>;

  /// Load state variable column solutions from a CSV file.
  /// Sets the warm column solution on each phase's LinearInterface so
  /// that physical_eini/physical_efin return loaded values before the
  /// first solve.
  [[nodiscard]] auto load_state(const std::string& filepath)
      -> std::expected<void, Error>;

  /// Get the global max kappa across all (scene, phase) LP solves.
  [[nodiscard]] double global_max_kappa() const noexcept
  {
    double gmax = 1.0;
    for (const auto& phase_kappas : m_max_kappa_) {
      for (const auto k : phase_kappas) {
        gmax = std::max(gmax, k);
      }
    }
    return gmax;
  }

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
                                  const SolverOptions& opts,
                                  IterationIndex iteration)
      -> std::expected<double, Error>;

  [[nodiscard]] auto backward_pass(SceneIndex scene,
                                   const SolverOptions& opts,
                                   IterationIndex iteration = {})
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
                                                  IterationIndex iteration = {})
      -> std::expected<int, Error>;

  /// Update the per-(scene, phase) max kappa value after an LP solve.
  /// Also checks kappa against the threshold and emits a warning or
  /// saves the LP file depending on the kappa_warning mode.
  void update_max_kappa(SceneIndex scene,
                        PhaseIndex phase,
                        const LinearInterface& li,
                        IterationIndex iteration = {});

  /// Update max kappa from an already-known value (no LP save possible).
  void update_max_kappa(SceneIndex scene,
                        PhaseIndex phase,
                        double kappa) noexcept
  {
    m_max_kappa_[scene][phase] = std::max(m_max_kappa_[scene][phase], kappa);
  }

  /// Check whether update_lp should be dispatched for this iteration.
  /// Returns false when the iteration is explicitly disabled or skipped
  /// by the global skip count.
  [[nodiscard]] bool should_dispatch_update_lp(IterationIndex iteration) const;

  /// Run update_lp for a single phase, setting prev_phase_sys for
  /// cross-phase physical_eini lookup.  Returns the number of updated
  /// LP elements.
  int update_lp_for_phase(SceneIndex scene, PhaseIndex phase);

  /// Conditionally dispatch update_lp for all phases in a scene.
  /// Checks the preallocated iteration vector for explicit skip/force
  /// flags and the global skip count before calling SystemLP::update_lp()
  /// on each phase.
  void dispatch_update_lp(SceneIndex scene, IterationIndex iteration);

  /// Clone the LP, apply elastic filter on the clone, and solve it.
  /// Returns an ElasticResult (with solution data and per-link slack info)
  /// if feasible, nullopt otherwise.
  /// The original LP is never modified (PLP clone pattern).
  [[nodiscard]] std::optional<ElasticResult> elastic_solve(
      SceneIndex scene, PhaseIndex phase, const SolverOptions& opts);

  // ── Refactored helper methods ──

  /// Store a cut for sharing and persistence (thread-safe).
  /// Writes to both per-scene storage and shared storage.
  void store_cut(SceneIndex scene,
                 PhaseIndex src_phase,
                 const SparseRow& cut,
                 CutType type = CutType::Optimality,
                 RowIndex row = RowIndex {-1});

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
                                               PhaseIndex start_phase,
                                               int total_cuts,
                                               const SolverOptions& opts,
                                               IterationIndex iteration)
      -> std::expected<int, Error>;

  /// Create a submit function that submits complete aperture tasks to
  /// the work pool for parallel execution.
  ///
  /// When the SDDP work pool is available, aperture tasks are submitted
  /// to the pool for concurrent execution across available threads.
  /// When the pool is unavailable, tasks run synchronously in the caller.
  ///
  /// @param phase      Phase index (for pool priority ordering)
  /// @param iteration  SDDP iteration index (for pool priority ordering)
  [[nodiscard]] auto make_aperture_submit_fn(PhaseIndex phase,
                                             IterationIndex iteration)
      -> ApertureSubmitFunc;

  /// Prune inactive cuts from all (scene, phase) LPs.
  /// Removes cuts whose |dual| < prune_dual_threshold, keeping at most
  /// max_cuts_per_phase active cuts per LP.  Only LP rows are removed;
  /// m_stored_cuts_ and m_scene_cuts_ are preserved for persistence.
  void prune_inactive_cuts();

  /// Cap per-scene stored cuts to max_stored_cuts (oldest dropped first).
  void cap_stored_cuts();

  /// Build combined stored cuts from per-scene vectors (for persistence).
  [[nodiscard]] std::vector<StoredCut> build_combined_cuts() const;

  /// Get a pooled clone pointer for aperture solves (nullptr if pool disabled).
  LinearInterface* get_pooled_clone_ptr(SceneIndex scene, PhaseIndex phase)
  {
    if (!m_options_.use_clone_pool || !m_clone_pool_.is_allocated()) {
      return nullptr;
    }
    return m_clone_pool_.get_ptr(
        scene,
        phase,
        planning_lp(),
        m_scene_phase_states_[scene][phase].base_nrows);
  }

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
      const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
      IterationIndex iteration);

  /// Validate that the simulation has ≥2 phases and ≥1 scene.
  [[nodiscard]] auto validate_inputs() const -> std::optional<Error>;

  /// Bootstrap-solve + initialize α variables, state links, and hot-start.
  /// Called once (guarded by m_initialized_).
  [[nodiscard]] auto initialize_solver() -> std::expected<void, Error>;

  /// Reset live-query atomics to their start-of-solve values.
  void reset_live_state() noexcept;

  /// Run the forward pass for all scenes in parallel.
  /// Returns a ForwardPassOutcome or an error if ALL scenes failed.
  [[nodiscard]] auto run_forward_pass_all_scenes(SDDPWorkPool& pool,
                                                 const SolverOptions& opts,
                                                 IterationIndex iter)
      -> std::expected<ForwardPassOutcome, Error>;

  /// Run the backward pass for all feasible scenes in parallel.
  /// When cut_sharing is None, scenes run their full backward pass
  /// independently.  When cut sharing is enabled, the backward pass is
  /// synchronized per-phase: all scenes complete a phase before optimality
  /// cuts are shared and the next phase is processed.
  [[nodiscard]] auto run_backward_pass_all_scenes(
      std::span<const uint8_t> scene_feasible,
      SDDPWorkPool& pool,
      const SolverOptions& opts,
      IterationIndex iter) -> BackwardPassOutcome;

  /// Process a single backward-pass phase step (pi → pi-1) for one scene.
  /// Builds the optimality cut, stores it, adds it to the LP, re-solves,
  /// and handles feasibility backpropagation.
  /// @return Number of optimality cuts added during this step.
  [[nodiscard]] auto backward_pass_single_phase(SceneIndex scene,
                                                PhaseIndex phase,
                                                int cut_offset,
                                                const SolverOptions& opts,
                                                IterationIndex iteration)
      -> std::expected<int, Error>;

  /// Process a single backward-pass phase step (pi → pi-1) with apertures.
  /// @return Number of optimality cuts added during this step.
  [[nodiscard]] auto backward_pass_with_apertures_single_phase(
      SceneIndex scene,
      PhaseIndex phase,
      int cut_offset,
      const SolverOptions& opts,
      IterationIndex iteration) -> std::expected<int, Error>;

  /// Implementation helper for aperture per-phase backward step.
  /// Used by backward_pass_with_apertures_single_phase.
  [[nodiscard]] auto backward_pass_aperture_phase_impl(
      SceneIndex scene,
      PhaseIndex phase,
      int cut_offset,
      const ScenarioLP& base_scenario,
      std::span<const ScenarioLP> all_scenarios,
      std::span<const Aperture> aperture_defs,
      const SolverOptions& opts,
      IterationIndex iteration) -> std::expected<int, Error>;

  /// Phase-synchronized backward pass: processes phases one at a time,
  /// sharing optimality cuts between scenes after each phase completes.
  [[nodiscard]] auto run_backward_pass_synchronized(
      std::span<const uint8_t> scene_feasible,
      SDDPWorkPool& pool,
      const SolverOptions& opts,
      IterationIndex iter) -> BackwardPassOutcome;

  /// Compute and fill ir.upper_bound, ir.lower_bound, ir.scene_lower_bounds.
  void compute_iteration_bounds(SDDPIterationResult& ir,
                                std::span<const uint8_t> scene_feasible,
                                std::span<const double> weights) const;

  /// Apply cut-sharing across scenes for all phases generated in this
  /// iteration.
  /// @param cuts_before  Value of m_stored_cuts_.size() BEFORE this
  ///                     iteration's backward pass.
  /// @param iteration    Current SDDP iteration index.
  void apply_cut_sharing_for_iteration(std::size_t cuts_before,
                                       IterationIndex iteration);

  /// Compute gap, update convergence flag, update live-query atomics, log.
  void finalize_iteration_result(SDDPIterationResult& ir, IterationIndex iter);

  /// Write the monitoring API status file if API is enabled.
  void maybe_write_api_status(const std::string& status_file,
                              const std::vector<SDDPIterationResult>& results,
                              std::chrono::steady_clock::time_point solve_start,
                              const SolverMonitor& monitor) const;

  /// Save cuts (combined + per-scene) after an iteration, handling infeasible
  /// scene renaming.
  void save_cuts_for_iteration(IterationIndex iter,
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
  [[nodiscard]] SceneUid scene_uid(SceneIndex si) const noexcept
  {
    return planning_lp().simulation().scenes()[si].uid();
  }

  /// Get the phase UID for a given PhaseIndex.
  [[nodiscard]] PhaseUid phase_uid(PhaseIndex pi) const noexcept
  {
    return planning_lp().simulation().phases()[pi].uid();
  }

  std::reference_wrapper<PlanningLP> m_planning_lp_;
  SDDPOptions m_options_;
  ApertureDataCache m_aperture_cache_;
  LabelMaker m_label_maker_;
  scene_phase_states_t m_scene_phase_states_;
  SDDPCutStore m_cut_store_;

  /// Clone pool: one cached LinearInterface per (scene, phase) for aperture
  /// reuse.  Empty when use_clone_pool is false.
  SDDPClonePool m_clone_pool_ {};

  /// Per-(scene, phase) count of consecutive forward-pass infeasibilities.
  /// Incremented when the elastic filter is used in forward_pass at (scene,
  /// phase).  Reset to 0 when the phase is solved normally (no elastic).
  /// Used by the backward pass to decide single_cut vs multi_cut mode.
  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, int>>
      m_infeasibility_counter_;

  /// Per-(scene, phase) maximum kappa (condition number) across all LP
  /// solves (forward, backward, aperture).  Updated after every solve call.
  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, double>>
      m_max_kappa_;

  bool m_initialized_ {false};

  /// Preallocated iteration vector, sized to `iteration_offset +
  /// max_iterations`. Default-constructed entries represent iterations with no
  /// user override. Populated in initialize_solver() after iteration_offset is
  /// known.
  StrongIndexVector<IterationIndex, IterationLP> m_iterations_ {};

  /// Iteration offset from hot-start cuts.  When cuts from a previous
  /// run are loaded, the solver starts numbering new iterations after
  /// the highest iteration found in the loaded cuts, avoiding name
  /// collisions.
  IterationIndex m_iteration_offset_ {};

  // ── Stop / callback machinery ──
  SDDPIterationCallback m_iteration_callback_ {};
  std::atomic<bool> m_stop_requested_ {false};
  /// When true, should_stop() returns false (simulation pass ignores stops).
  bool m_in_simulation_ {false};

  // ── Atomic live-query state ──
  std::atomic<int> m_current_iteration_ {0};
  std::atomic<double> m_current_gap_ {1.0};
  std::atomic<double> m_current_lb_ {0.0};
  std::atomic<double> m_current_ub_ {0.0};
  std::atomic<bool> m_converged_ {false};
  std::atomic<int> m_current_pass_ {0};  ///< 0=idle, 1=forward, 2=backward
  std::atomic<int> m_scenes_done_ {0};  ///< Scenes completed in current pass

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

  /// Generate an LP name only when names_level >= only_cols.
  /// @param args  Arguments forwarded to LabelMaker::lp_label.
  template<typename... Args>
  [[nodiscard]] auto sddp_label(Args&&... args) const -> std::string
  {
    return m_label_maker_.lp_label(std::forward<Args>(args)...);
  }
};

// ─── SDDPPlanningMethod ─────────────────────────────────────────────────────

/**
 * @class SDDPPlanningMethod
 * @brief Adapter that wraps SDDPMethod behind the PlanningMethod interface
 */
class SDDPPlanningMethod final : public PlanningMethod
{
public:
  explicit SDDPPlanningMethod(SDDPOptions opts = {}) noexcept;

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
