/**
 * @file      sddp_types.hpp
 * @brief     Data types for the SDDP iterative solver
 * @date      2026-04-03
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains the configuration, result, and state types used by the SDDP
 * solver.  Extracted from sddp_method.hpp to reduce header size and
 * improve modularity: consumers that only need the types (e.g. monitors,
 * cut I/O, cascade orchestration) can include this lightweight header
 * instead of the full SDDPMethod class definition.
 *
 * Types provided:
 *   - SDDPOptions            — runtime solver configuration
 *   - SDDPIterationResult    — per-iteration convergence data
 *   - PhaseStateInfo         — per-phase alpha / links / cached solution
 *   - ForwardPassOutcome     — forward-pass summary
 *   - BackwardPassOutcome    — backward-pass summary
 *   - SDDPIterationCallback  — observer callback type
 *   - sddp_file namespace   — file naming constants
 *
 * Free functions:
 *   - parse_cut_sharing_mode()
 *   - parse_elastic_filter_mode()
 *   - compute_scene_weights()
 *   - compute_convergence_gap()
 */

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/benders_cut.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

// Forward declaration for compute_scene_weights()
class SceneLP;
class SimulationLP;

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
/// Combined cut file name (CSV format)
constexpr auto combined_cuts = "sddp_cuts.csv";
/// Versioned cut file pattern: format with iteration number
constexpr auto versioned_cuts_fmt = "sddp_cuts_{}.csv";
/// Per-scene cut file pattern: format with scene UID
constexpr auto scene_cuts_fmt = "scene_{}.csv";
/// Combined cut file name (JSON format)
constexpr auto combined_cuts_json = "sddp_cuts.json";
/// Versioned cut file pattern (JSON): format with iteration number
constexpr auto versioned_cuts_json_fmt = "sddp_cuts_{}.json";
/// Per-scene cut file pattern (JSON): format with scene UID
constexpr auto scene_cuts_json_fmt = "scene_{}.json";
/// Error-prefixed cut file pattern for infeasible scenes (scene UID)
constexpr auto error_scene_cuts_fmt = "error_scene_{}.csv";
/// Error LP file pattern for infeasible scene/phase (scene UID, phase UID)
constexpr auto error_lp_fmt = "error_scene_{}_phase_{}";
/// Debug LP file pattern: format with iteration, scene UID, phase UID
constexpr auto debug_lp_fmt = "gtopt_iter_{}_scene_{}_phase_{}";
/// Sentinel file name: if this file exists in the output directory, the
/// SDDP solver stops gracefully after the current iteration and saves
/// cuts.  Created externally (e.g. by the webservice stop endpoint).
constexpr auto stop_sentinel = "sddp_stop";
/// State variable column solution — CSV format (latest)
constexpr auto state_cols = "sddp_state.csv";
/// Versioned state column solution (CSV): format with iteration number
constexpr auto versioned_state_fmt = "sddp_state_{}.csv";
/// State variable column solution — JSON format (latest)
constexpr auto state_cols_json = "sddp_state.json";
/// Versioned state column solution (JSON): format with iteration number
constexpr auto versioned_state_json_fmt = "sddp_state_{}.json";
/// Monitoring API stop-request file name: if this file exists, the solver
/// stops gracefully after the current iteration (same behaviour as the
/// sentinel file).  Written by the webservice soft-stop endpoint as part
/// of the bidirectional monitoring API.  Complements rather than replaces
/// the sentinel mechanism so that external scripts using the raw sentinel
/// still work.  The solver checks: sentinel_file exists || stop_request
/// file exists.
constexpr auto stop_request = "sddp_stop_request.json";
}  // namespace sddp_file

// ─── Alpha (future-cost) variable naming ────────────────────────────────────
//
// Alpha is the method-owned cost-to-go variable added to every phase
// except the last.  It is registered in `sim.state_variables()` like any
// other state variable so that state/cut CSV I/O and cross-level
// resolution treat it uniformly.  These constants centralise the
// class/column name so every call site uses the same identifiers.
constexpr std::string_view sddp_alpha_class_name = "Sddp";
constexpr std::string_view sddp_alpha_col_name = "alpha";
/// Fixed uid used in the alpha `StateVariable::Key`.  The state-variable
/// map is partitioned by `(scene_index, phase_index)` and there is at
/// most one alpha per cell, so any constant uid disambiguates the key.
/// `Uid{0}` keeps the structured cut-key label as `Sddp:alpha:0`.
constexpr Uid sddp_alpha_uid {0};

// ─── Elastic filter mode ────────────────────────────────────────────────────
// ElasticFilterMode is now defined in <gtopt/sddp_enums.hpp>.
// The generic enum_from_name<ElasticFilterMode>() replaces the old
// parse_elastic_filter_mode() free function.

/// Parse an elastic filter mode from a string (backward-compatible
/// wrapper).  Accepts "single_cut" / "cut" (= single_cut), "multi_cut",
/// "chinneck" / "iis" (= chinneck).  Unknown strings — including the
/// retired "backpropagate" — fall back to the default mode (chinneck).
[[nodiscard]] ElasticFilterMode parse_elastic_filter_mode(
    std::string_view name);

/// Configuration options for the SDDP iterative solver
struct SDDPOptions  // NOLINT(clang-analyzer-optin.performance.Padding)
{
  int max_iterations {100};  ///< Maximum forward/backward iterations
  int min_iterations {2};  ///< Minimum iterations before convergence
  double convergence_tol {1e-4};  ///< Relative gap tolerance for convergence
  double elastic_penalty {1e3};  ///< Penalty for elastic slack variables
  double alpha_min {0.0};  ///< Lower bound for future cost variable a ($)
  double alpha_max {1e15};  ///< Upper bound for future cost variable a ($)
  double scale_alpha {0};  ///< Scale for α (0 = auto: max state var_scale)
  CutSharingMode cut_sharing {CutSharingMode::none};  ///< Cut sharing mode

  /// Elastic filter mode: how to handle backward-pass infeasibility.
  /// `single_cut` adds a single Benders feasibility cut to the
  /// previous phase.  `multi_cut` adds the same cut plus one
  /// bound-constraint cut per activated slack variable.
  /// `chinneck` (default) emits per-IIS-bound multi-cuts after a
  /// Chinneck-style elastic IIS filter pass (see ElasticFilterMode
  /// in sddp_enums.hpp for the algorithm).
  ElasticFilterMode elastic_filter_mode {ElasticFilterMode::chinneck};

  /// Absolute tolerance for filtering tiny Benders cut coefficients.
  /// Coefficients with |value| < cut_coeff_eps are dropped from the cut.
  /// 0.0 = no filtering (default).
  double cut_coeff_eps {0.0};

  /// Forward-pass infeasibility counter threshold for automatic switching
  /// from single_cut to multi_cut.  When the forward pass has encountered
  /// infeasibility at (scene, phase) more than this many times without
  /// recovery, the backward-pass infeasibility handler switches to
  /// multi_cut mode for that (scene, phase).
  ///  = 0  always use multi_cut for any infeasibility (force multi_cut).
  ///  > 0  switch to multi_cut after the counter exceeds this threshold.
  ///  < 0  never auto-switch (disabled; use explicit mode only).
  /// Default: 10.
  int multi_cut_threshold {10};

  /// File format for cut and state variable I/O (csv or json).
  /// CSV uses structured keys (class:var:uid=coeff) and is backward
  /// compatible with legacy name-based CSV files on the load side.
  /// JSON uses compact daw::json serialization with fully structured data.
  CutIOFormat cut_io_format {CutIOFormat::csv};

  /// Save cuts after each training iteration (default: true).
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
  /// - `none`:    cold start -- no cuts loaded (default)
  /// - `keep`:    load cuts; keep original output file unchanged
  /// - `append`:  load cuts; append new cuts to original file
  /// - `replace`: load cuts; replace original file with all cuts
  HotStartMode cut_recovery_mode {HotStartMode::none};

  /// Controls what is recovered from a previous SDDP run:
  /// - `none`:  no recovery (cold start)
  /// - `cuts`:  recover only Benders cuts
  /// - `full`:  recover cuts + state variable solutions (default)
  RecoveryMode recovery_mode {RecoveryMode::full};

  /// Caller-supplied lower bound for the iteration index at which this
  /// solver should start counting.  Composed with the hot-start offset
  /// via `std::max`, so hot-start cuts always win when they demand a
  /// higher offset.  Used by CascadePlanningMethod to place each level's
  /// iteration indices in a disjoint global range (avoids in-memory cut
  /// store collisions and gives log lines a globally monotonic index).
  /// Absent = solver starts at iteration 0 (or at the hot-start offset,
  /// whichever is higher).
  std::optional<IterationIndex> iteration_offset_hint {};

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

  /// Compression format for LP debug files ("gzip" / "uncompressed" / "").
  /// Empty or "uncompressed" means no compression; any other value uses
  /// gzip.
  std::string lp_debug_compression {};

  /// Selective LP debug filters: when set, only save LP files whose
  /// scene/phase UIDs fall within [min, max] (inclusive).
  OptInt lp_debug_scene_min {};
  OptInt lp_debug_scene_max {};
  OptInt lp_debug_phase_min {};
  OptInt lp_debug_phase_max {};

  /// Enable the monitoring API: write a JSON status file after each
  /// iteration and periodically update real-time workpool statistics.
  /// Consumers (e.g. sddp_monitor.py) can poll this file to display
  /// live charts.  Default: true.
  bool enable_api {true};

  /// Path for the JSON status file.  If empty, the solver writes to
  /// "<output_directory>/solver_status.json" (derived at solve time from
  /// the PlanningLP options).
  std::string api_status_file {};

  /// Path for the monitoring API stop-request file.  When this file
  /// exists the solver stops gracefully after the current iteration and
  /// saves cuts, exactly like the sentinel_file mechanism.  The file is
  /// written by the webservice soft-stop endpoint as part of the
  /// bidirectional monitoring API.  Use sddp_file::stop_request
  /// ("sddp_stop_request.json") as the filename in the output directory.
  /// Empty = feature disabled.
  std::string api_stop_request_file {};

  /// Interval at which the background monitoring thread refreshes
  /// real-time workpool statistics (CPU load, active workers) in the
  /// status file.
  std::chrono::milliseconds api_update_interval {500};

  /// Number of apertures (hydrological realisations) to solve in each
  /// backward-pass phase.  Each aperture clones the phase LP and updates
  /// the flow column bounds to the corresponding scenario's discharge
  /// values, then solves the clone to obtain an independent Benders cut.
  /// The final cut added to the previous phase is the
  /// probability-weighted average of all aperture cuts (expected cut).
  /// Aperture UIDs for the backward pass.
  ///
  ///  nullopt -- use per-phase `Phase::apertures` or simulation-level
  ///            `aperture_array` (default behaviour).
  ///  empty   -- no apertures; use pure Benders backward pass.
  ///  [1,2,3] -- use exactly these aperture UIDs, overriding per-phase
  ///             sets.
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

  /// Save LP files for infeasible apertures to log_directory (default:
  /// false).
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

  /// Low memory mode: off (default), snapshot, compress, or rebuild.
  /// Trades CPU time (reconstruction + optional decompression, or full
  /// re-flatten under `rebuild`) for significant memory savings on
  /// large problems.  Under `rebuild` the initial up-front build loop
  /// is skipped and each per-(scene, phase) LP is built lazily inside
  /// the same task that solves or clones it.
  LowMemoryMode low_memory_mode {LowMemoryMode::off};

  /// In-memory compression codec for low_memory compress mode.
  /// Default: auto_select (picks best available: lz4 > snappy > zstd > gzip).
  CompressionCodec memory_codec {CompressionCodec::auto_select};

  /// CSV file with boundary (future-cost) cuts for the last phase.
  ///
  /// These cuts approximate the expected future cost beyond the planning
  /// horizon, analogous to PLP's "planos de embalse" (reservoir
  /// future-cost function).  Each cut has the form:
  ///   a >= rhs + S_i coeff_i . state_var_i
  ///
  /// The CSV header row names the state variables (reservoirs /
  /// batteries).  The solver maps these names to LP columns in the last
  /// phase and adds each cut as a lower-bound constraint on the future
  /// cost variable a.  Empty = no boundary cuts.
  std::string boundary_cuts_file {};

  /// How boundary cuts are loaded:
  /// - "noload"    -- skip loading even if a file is specified
  /// - "separated" -- assign each cut to the scene matching its `scene`
  ///                 column (scene UID); unmatched UIDs are skipped
  /// - "combined"  -- broadcast all cuts to all scenes
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
  /// The solver resolves named state-variable headers (reservoir /
  /// battery / junction) to LP column indices in the specified phase,
  /// then adds each cut as a lower-bound constraint on the corresponding
  /// a variable:
  ///   a_phase >= rhs + S_i coeff_i . state_var_i[phase]
  ///
  /// Format:
  ///   name,iteration,scene,phase,rhs,StateVar1,StateVar2,...
  ///
  /// Empty = no named hot-start cuts.
  std::string named_cuts_file {};

  // ── Secondary (stationary gap) convergence ────────────────────────────

  /// Tolerance for the secondary stationary-gap convergence criterion.
  ///
  /// When the relative change in the convergence gap over the last
  /// `stationary_window` iterations falls below this value, the solver
  /// declares convergence even if the gap is above `convergence_tol`.
  /// This handles problems where the SDDP gap converges to a non-zero
  /// stationary value due to stochastic noise or problem structure
  /// (a known theoretical limitation of SDDP/Benders on certain
  /// programs).
  ///
  /// Criterion (after min_iterations and stationary_window iters done):
  ///   gap_change = |gap[i] - gap[i - window]|
  ///              / max(1e-10, gap[i - window])
  ///   gap_change < stationary_tol -> declare convergence
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
  /// confidence interval: UB - LB <= z_{a/2} * s (PLP-style).
  /// Combined with stationary_tol, also declares convergence when the
  /// gap stabilises above the CI threshold (non-zero gap case).
  /// Default: 0.95 (95% CI).
  double convergence_confidence {0.95};

  /// Optional LP solver options for the forward pass.
  /// When set, these override the global solver options for forward-pass
  /// solves.  The options are pre-merged with the global solver options
  /// at construction time (forward takes precedence).
  std::optional<SolverOptions> forward_solver_options {};

  /// Optional LP solver options for the backward pass.
  /// When set, these override the global solver options for
  /// backward-pass solves.  The options are pre-merged with the global
  /// solver options at construction time (backward takes precedence).
  std::optional<SolverOptions> backward_solver_options {};

  /// SDDP work pool CPU over-commit factor.  Multiplied by
  /// hardware_concurrency to set max pool threads.  Default 4.0 — extra
  /// threads keep CPUs busy while others block on the clone mutex.
  double pool_cpu_factor {4.0};

  /// Process memory limit in MB for the SDDP work pool.
  /// When non-zero, the pool blocks task dispatch if process RSS exceeds
  /// this value.  0 = no limit (default).
  double pool_memory_limit_mb {0.0};

  /// Maximum iteration spread between fastest and slowest scene when
  /// cut_sharing == none and multiple scenes exist.  When > 0, the
  /// solver runs scenes asynchronously: each scene progresses through
  /// its own forward/backward iteration loop, and the pool's priority
  /// queue (SDDPTaskKey) naturally gives higher priority to scenes at
  /// earlier iterations, self-regulating the spread.
  /// 0 = synchronous (current behavior, default).
  int max_async_spread {0};
};

// ─── Iteration result ───────────────────────────────────────────────────────

/// Result of a single SDDP iteration (forward + backward pass)
struct SDDPIterationResult
{
  IterationIndex iteration_index {};  ///< Iteration number (0-based)
  double lower_bound {};  ///< Lower bound (phase 0 obj including a)
  double upper_bound {};  ///< Upper bound (sum of actual phase costs)
  double gap {};  ///< Relative gap: (UB - LB) / max(1, |UB|)
  /// Relative change in gap vs. `stationary_window` iterations ago.
  /// Populated only when `stationary_tol > 0` and enough iterations have
  /// elapsed; 1.0 otherwise (meaning "not yet checked / not applicable").
  double gap_change {1.0};
  bool converged {};  ///< True if gap < convergence tolerance
  /// True when convergence was declared by the stationary-gap criterion
  /// (gap_change < stationary_tol) rather than the primary criterion.
  bool stationary_converged {};
  /// True when convergence was declared by the statistical CI criterion
  /// (|UB - LB| <= z_{a/2} * s / sqrt(N)) rather than the primary
  /// criterion.
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
  /// Per-scene lower bounds (phase-0 objective values).  Size =
  /// num_scenes.
  std::vector<double> scene_lower_bounds {};

  /// Per-scene iteration at the time this result was computed.
  /// Populated only in async mode (max_async_spread > 0).  Shows the
  /// iteration each scene had completed when this aggregate convergence
  /// check was triggered.  Size = num_scenes when populated, empty
  /// otherwise.
  std::vector<int> scene_iterations {};
};

// ─── Utility free functions (independently testable) ────────────────────────

/// Compute normalised per-scene probability weights.
///
/// For each scene: weight = sum of scenario probability_factors if
/// positive, else 1.0.  Infeasible scenes (scene_feasible[si]==0) get
/// weight 0.  Weights are normalised to sum to 1 across feasible scenes.
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
    ProbabilityRescaleMode rescale_mode = ProbabilityRescaleMode::runtime);

/// Compute relative convergence gap: (UB - LB) / max(1.0, |UB|).
/// Always returns a non-negative value.
[[nodiscard]] double compute_convergence_gap(double upper_bound,
                                             double lower_bound) noexcept;

/// Look up the alpha (future-cost) state variable registered by
/// `SDDPMethod::initialize_alpha_variables` for the given (scene, phase).
/// Returns `nullptr` for the last phase (which has no alpha) and for any
/// phase that has not yet been initialised.
///
/// Callers should read the alpha column index freshly via
/// `svar->col()` and the forward-pass trial value via `svar->col_sol()`,
/// which is populated by `capture_state_variable_values` after every
/// successful solve.  This replaces the former `PhaseStateInfo::alpha_col`
/// cache, which could become stale on low_memory reconstruct/clone paths.
[[nodiscard]] const StateVariable* find_alpha_state_var(
    const SimulationLP& sim,
    SceneIndex scene_index,
    PhaseIndex phase_index) noexcept;

// ─── Per-phase tracking ─────────────────────────────────────────────────────

/// Per-phase SDDP state: a variable, outgoing links, forward-pass cost.
///
/// Per-state-variable trial values (consumed by cut building and
/// next-phase propagation) live on `StateVariable::col_sol()`, populated
/// by `capture_state_variable_values` after every forward solve.
struct PhaseStateInfo
{
  std::vector<StateVarLink> outgoing_links {};  ///< Links TO next phase
  size_t base_nrows {0};  ///< Row count before any Benders cuts
  double forward_objective {0.0};  ///< Opex from last forward pass
  /// Full objective from last forward solve (including α), in physical
  /// ($) space — i.e. `LinearInterface::get_obj_value_physical()`, not
  /// the scaled LP raw value.  Cached here so the backward pass can
  /// call `build_benders_cut_physical` without re-querying the
  /// original LP.
  double forward_full_obj_physical {0.0};
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
  /// Total feasibility cuts (including multi-cut bound rows) installed
  /// across all scenes in this pass.  When zero and
  /// has_feasibility_issue is true, retrying will produce the same
  /// result — the caller's retry loop should break.
  std::size_t n_fcuts_installed {0};
  double elapsed_s {0.0};
};

/// Outcome of running the backward pass across all scenes
struct BackwardPassOutcome
{
  int total_cuts {0};
  bool has_feasibility_issue {false};
  double elapsed_s {0.0};
};

}  // namespace gtopt
