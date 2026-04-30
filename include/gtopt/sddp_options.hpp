/**
 * @file      sddp_options.hpp
 * @brief     SDDP-specific solver configuration parameters
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/planning_enums.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief SDDP-specific solver configuration parameters
 *
 * Groups all SDDP-related options into a single sub-object for clearer
 * JSON organization.  Field names omit the `sddp_` prefix since they
 * already live inside the `sddp_options` namespace.
 *
 * All fields are optional — defaults are applied via `PlanningOptionsLP`.
 */
struct SddpOptions  // NOLINT(clang-analyzer-optin.performance.Padding)
{
  /** @brief Cut sharing mode: none (default), expected, accumulate, or max */
  std::optional<CutSharingMode> cut_sharing_mode {};
  /** @brief Directory for Benders cut files (default: `"cuts"`) */
  OptName cut_directory {};
  /** @brief Enable the SDDP monitoring API (writes JSON status file each
   * iteration; default: true) */
  OptBool api_enabled {};
  /** @brief Iterations to skip between update_lp dispatches.
   * 0 = update every iteration (default).  Applies to all volume-dependent
   * LP element updates (seepage, discharge limit, production factor). */
  OptInt update_lp_skip {};

  // ── Iteration control ──────────────────────────────────────────────────────
  /** @brief Maximum number of forward/backward iterations (default: 100) */
  OptInt max_iterations {};
  /** @brief Minimum iterations before declaring convergence (default: 2) */
  OptInt min_iterations {};
  /** @brief Relative gap tolerance for convergence (default: 1e-4) */
  OptReal convergence_tol {};

  // ── Advanced tuning ────────────────────────────────────────────────────────
  //
  // Naming conventions for fields below — kept stable on purpose:
  //
  //   * ``scale_*``       -> mirrors ``scale_objective`` / ``scale_theta``
  //                          in ``model_options`` and ``lp_matrix_options``
  //                          (any ``scale_X`` is "divisor that keeps the
  //                          LP coefficients of variable X near unity").
  //   * ``*_eps``         -> mirrors ``optimal_eps``, ``feasible_eps``,
  //                          ``barrier_eps``, ``stats_eps``, ``matrix_eps``
  //                          (numerical-tolerance parameters).
  //   * ``*_threshold``   -> trigger thresholds on derived metrics
  //                          (e.g. ``lp_coeff_ratio_threshold``,
  //                          ``prune_dual_threshold``).
  //
  // Renaming any of ``scale_alpha`` / ``multi_cut_threshold`` /
  // ``cut_coeff_eps`` would break: existing JSON inputs, golden test
  // fixtures, the formulation white paper, and external CI pipelines
  // that pin these keys.  The names are kept as the canonical
  // representation; the doxygen blocks below carry the
  // expert-facing explanation.
  /** @brief Penalty for elastic slack variables in feasibility.
   *  Default: `PlanningOptionsLP::default_sddp_elastic_penalty`
   *  (see `planning_options_lp.hpp`). */
  OptReal elastic_penalty {};
  /** @brief Scale divisor for future cost variable α (default: 0 = auto).
   *
   * The LP alpha variable is α_lp = α / scale_alpha, with an objective
   * coefficient of scale_alpha so that the physical contribution is
   * preserved.  When 0 (default), auto-computed as max(var_scale) across
   * all state variables — keeps α O(1) relative to the largest state
   * variable in LP units. */
  OptReal scale_alpha {};

  // ── Cut file management ────────────────────────────────────────────────────
  /** @brief Cut persistence mode: none (default), keep, append, or replace.
   *  Controls whether to load cuts from a previous run and how to handle
   *  the combined output file on completion. */
  std::optional<HotStartMode> cut_recovery_mode {};
  /** @brief Recovery mode: none (0), cuts (1), or full (2).
   *  Controls what is recovered from a previous run:
   *  - none:  no recovery (cold start).
   *  - cuts:  recover only Benders cuts.
   *  - full:  recover cuts + state variable solutions (default). */
  std::optional<RecoveryMode> recovery_mode {};
  /** @brief Cut/state I/O format: csv (default) or json */
  std::optional<CutIOFormat> cut_io_format {};
  /** @brief Save cuts after each iteration (default: true).
   *  When false, cuts are only saved at the end of the solve or on stop. */
  OptBool save_per_iteration {};
  /** @brief File path for loading initial cuts (hot-start; empty = cold start)
   */
  OptName cuts_input_file {};
  /** @brief Path to a sentinel file; if it exists, the solver stops gracefully
   * after the current iteration (analogous to PLP's userstop) */
  OptName sentinel_file {};
  /** @brief Elastic filter mode: chinneck (default, alias "iis"),
   *         single_cut (alias "cut"), or multi_cut.
   *         See ElasticFilterMode in sddp_enums.hpp for semantics.
   *         The legacy "backpropagate" mode is no longer supported;
   *         it falls through to the default. */
  std::optional<ElasticFilterMode> elastic_mode {};
  /** @brief Forward-pass infeasibility count threshold for switching
   *         from single_cut to multi_cut.  Default:
   *         `PlanningOptionsLP::default_sddp_multi_cut_threshold`
   *         (see `planning_options_lp.hpp`).  `0 = always multi_cut`;
   *         `< 0 = disabled`.
   *
   *         The counter is **persistent**: it accumulates across
   *         iterations and is not reset when a (scene, phase) solves
   *         feasibly.  Switch fires when `infeas_count >= threshold`. */
  OptInt multi_cut_threshold {};
  /** @brief Aperture UIDs for the backward pass.
   *
   * - absent (nullopt) – use per-phase `Phase::apertures` (default)
   * - empty array `[]` – no apertures (pure Benders)
   * - non-empty `[1,2,3]` – use exactly these aperture UIDs,
   *   overriding per-phase apertures
   */
  std::optional<Array<Uid>> apertures {};
  /** @brief Directory for aperture-specific scenario data.
   *
   * When present, scenarios referenced by `Aperture::source_scenario` are
   * first looked up in this directory.  If not found there, they fall back
   * to the regular `input_directory`.  This allows backward-pass apertures
   * to use different affluent data than the forward-pass scenarios.
   */
  OptName aperture_directory {};

  /** @brief Timeout in seconds for individual aperture LP solves in the
   *  SDDP backward pass.
   *
   * When an aperture LP exceeds this time, it is treated as infeasible
   * (skipped), a WARNING is logged, and the solver continues with the
   * remaining apertures.  Default 15 seconds.  0 = no timeout.
   */
  OptReal aperture_timeout {};

  /** @brief Save LP files for infeasible apertures to the log directory.
   *
   * When true, each infeasible aperture clone is written as
   * ``error_aperture_sc_<scene>_ph_<phase>_ap_<uid>.lp`` in the log
   * directory.  Useful for debugging but expensive in large cases.
   * Default: false (disabled).
   */
  OptBool save_aperture_lp {};

  /** @brief CSV file with boundary (future-cost) cuts for the last phase.
   *
   * These are analogous to PLP's "planos de embalse" — external optimality
   * cuts that approximate the expected future cost beyond the planning
   * horizon.  Each cut is of the form:
   *
   *   α ≥ rhs + Σ_i  coeff_i · state_var_i
   *
   * The CSV header row names the state variables (reservoir / battery);
   * subsequent rows provide the cut name, iteration, scene UID,
   * RHS, and gradient coefficients.
   *
   * Format:
   *
   * ```text
   * name,iteration,scene,rhs,Reservoir1,Reservoir2,...
   * cut_001,1,1,-5000.0,0.25,0.75,...
   * ```
   *
   * The `scene` column contains the scene UID (matching the `uid` field
   * in gtopt's `scene_array`).  The solver maps column headers to the LP
   * state-variable columns in the last phase and adds each cut as a
   * lower-bound constraint on the future cost variable α.
   * If empty, no boundary cuts are loaded.
   */
  OptName boundary_cuts_file {};

  /** @brief How boundary cuts are loaded: noload, separated (default),
   * or combined.
   *
   * - noload — do not load boundary cuts even if a file is given.
   * - separated — load cuts per scene (scene UID matching; default).
   * - combined — load all cuts into all scenes (broadcast).
   */
  std::optional<BoundaryCutsMode> boundary_cuts_mode {};

  /** @brief Maximum number of SDDP iterations to load from the boundary
   * cuts file.  Only cuts from the last N iterations (by `iteration`
   * column, i.e. PLP's IPDNumIte) are loaded.  0 = load all (default).
   */
  OptInt boundary_max_iterations {};

  /** @brief How to handle cuts referencing state variables not in the model.
   *
   * - `skip_coeff` (default): drop the missing coefficient, load the cut.
   * - `skip_cut`: skip the entire cut if any missing variable has a
   *   non-zero coefficient.
   */
  std::optional<MissingCutVarMode> missing_cut_var_mode {};

  /** @brief CSV file with named-variable cuts for hot-start across all phases.
   *
   * Unlike boundary cuts (which apply only to the last phase), these cuts
   * include a `phase` column indicating which phase they belong to.  The
   * solver resolves named state-variable headers (reservoir / battery /
   * junction) to LP column indices in the specified phase, then adds each cut
   * as:
   *
   *   α_phase ≥ rhs + Σ_i coeff_i · state_var_i[phase]
   *
   * Format:
   *
   * ```text
   * name,iteration,scene,phase,rhs,Reservoir1,Reservoir2,...
   * hs_1_1_3,1,1,3,-5000.0,0.25,0.75,...
   * ```
   *
   * If empty, no named hot-start cuts are loaded.
   */
  OptName named_cuts_file {};

  /// Maximum retained cuts per (scene, phase) LP.  0 = unlimited (default).
  OptInt max_cuts_per_phase {};
  /// Iterations between cut pruning passes.  Default: 10.
  OptInt cut_prune_interval {};
  /// Dual threshold for inactive cut detection.  Default: 1e-8.
  OptReal prune_dual_threshold {};

  /// Use single cut storage: store in per-scene vectors only.  Default: false.
  OptBool single_cut_storage {};
  /// Maximum total stored cuts per scene (0 = unlimited).  Default: 0.
  OptInt max_stored_cuts {};

  /// Run in simulation mode: no training iterations (max_iterations=0),
  /// forward-only evaluation of the policy from loaded cuts.
  /// No cuts are saved.  Default: false.
  OptBool simulation_mode {};

  /// Low memory mode: off, compress (default for SDDP/cascade), or rebuild.
  /// Trades CPU time (reconstruction + optional decompression, or full
  /// re-flatten under `rebuild`) for significant memory savings on large
  /// problems.  Under `rebuild`, the initial up-front build loop is
  /// skipped entirely and each per-(scene, phase) LP is built lazily
  /// inside the same task that solves or clones it.
  ///
  /// When unset, `PlanningOptionsLP::sddp_low_memory()` resolves to
  /// `compress` for SDDP/cascade methods (the historical default was
  /// `off`).  Pass `--memory-saving off` to restore the eager-resident
  /// behaviour.
  std::optional<LowMemoryMode> low_memory_mode {};

  /// In-memory compression codec for low_memory level 2.
  /// Selects the algorithm used to compress the saved FlatLinearProblem.
  /// Default: auto (picks best available: lz4 > snappy > zstd > gzip).
  /// Accepted values: "auto", "none", "lz4", "snappy", "zstd", "gzip".
  std::optional<CompressionCodec> memory_codec {};

  /** @brief Absolute tolerance for filtering numerically tiny Benders cut
   * coefficients.
   *
   * When constructing an optimality cut, any state-variable coefficient
   * (reduced cost or row dual) whose absolute value is below this threshold
   * is dropped — both the coefficient and its corresponding RHS adjustment
   * are skipped.  This removes solver numerical noise that would otherwise
   * produce ill-conditioned cuts.
   *
   * Inspired by PLP's OptiEPS mechanism (default 1e-8 there).
   *
   * Default: 0.0 (no filtering — all coefficients are kept).
   * Typical useful values: 1e-12 to 1e-8.
   */
  OptReal cut_coeff_eps {};

  /** @brief How update_lp elements obtain reservoir/battery volume between
   * phases.
   *
   * Controls the fallback in StorageLP::physical_eini for nonlinear LP
   * coefficient updates (seepage, production factor, discharge limit).
   * Does NOT affect SDDP state-variable chaining or cut generation.
   *
   * - `warm_start` (default): volume from warm-start solution, recovered
   *   state file, or vini.  No cross-phase lookup.
   * - `cross_phase`: volume from the previous phase's efin within the
   *   same forward pass.
   */
  std::optional<StateVariableLookupMode> state_variable_lookup_mode {};

  // ── Convergence criteria
  // ────────────────────────────────────────────────────

  /** @brief Convergence criterion mode selection.
   *
   * - `gap_only`:        deterministic gap test only.
   * - `gap_stationary`:  gap + stationary gap detection.
   * - `statistical`:     gap + stationary + CI (default, PLP-style).
   *
   * The `statistical` mode degrades gracefully to `gap_stationary` when
   * only one scene is present (no apertures / pure Benders).
   */
  std::optional<ConvergenceMode> convergence_mode {};

  /** @brief Tolerance for stationary-gap convergence criterion.
   *
   * When the relative change in the convergence gap over the last
   * `stationary_window` iterations falls below this value, the gap is
   * considered stationary (no longer improving).  This triggers
   * convergence in two situations:
   *
   *  1. Standalone: gap is stationary → declare convergence even if
   *     gap > convergence_tol (non-zero gap accepted).
   *  2. Combined with convergence_confidence: when the CI test fails
   *     (gap > z*σ) but the gap is stationary → declare convergence
   *     (the non-zero gap has stabilised and further iterations won't
   *     help).
   *
   * Formula (after min_iterations and stationary_window completed):
   *   gap_change = |gap[i] − gap[i − window]| / max(1e-10, gap[i − window])
   *   if gap_change < stationary_tol → gap is stationary
   *
   * Default: 0.01 (1%).  Set to 0.0 to disable.
   */
  OptReal stationary_tol {};

  /** @brief Number of iterations to look back when checking for a stationary
   * gap.  Used by both the standalone stationary criterion and the
   * statistical+stationary criterion.  Default: 10.
   */
  OptInt stationary_window {};

  /** @brief Confidence level for statistical convergence criterion (0-1).
   *
   * When > 0 and multiple scenes exist, convergence is checked via
   * PLP-style confidence interval: UB - LB <= z_{α/2} * σ.
   * Combined with stationary_tol, also handles the non-zero-gap case
   * where the gap stabilises above the CI threshold.
   *
   * Default: 0.95 (95% CI).  Set to 0.0 to disable. */
  OptReal convergence_confidence {};

  // ── LP solver options (per-pass override)
  // ───────────────────────────────────

  /** @brief Maximum algorithm fallback attempts for forward-pass solves.
   *
   *  Controls how many alternative algorithms the solver tries when a
   *  forward-pass LP returns non-optimal.  Default: 2 (full cycle).
   */
  OptInt forward_max_fallbacks {};

  /** @brief Scene-level fail-stop forward pass (default: true).
   *
   *  When true (the new default), an infeasible phase that produces a
   *  feasibility cut on its predecessor causes the scene's forward pass
   *  to **stop immediately for the current iteration**: the cut is
   *  installed on phase p-1, the scene is marked failed-this-iter, and
   *  control returns to the caller.  The next iteration starts fresh
   *  from p1 with the newly accumulated cuts (preserved in the global
   *  cut store).
   *
   *  When false, the legacy PLP-style backtracking forward pass is
   *  restored: after installing the fcut on p-1, `phase_idx` is
   *  decremented and p-1 is re-solved under the new cut.  If p-1 is
   *  still infeasible, a fresh fcut is installed on p-2 and the
   *  cascade continues — bounded by `forward_max_attempts`.  Kept
   *  available for regression tests and academic fixtures that depend
   *  on the cascade dynamics.
   */
  OptBool forward_fail_stop {};

  /** @brief Maximum algorithm fallback attempts for backward-pass and
   *  aperture solves.
   *
   *  Controls how many alternative algorithms the solver tries when a
   *  backward-pass or aperture LP returns non-optimal.  Default: 0
   *  (no fallback — fail immediately).
   */
  OptInt backward_max_fallbacks {};

  /** @brief SDDP work pool CPU over-commit factor.
   *  Multiplied by hardware_concurrency to set max pool threads.
   *  Default 4.0 — extra threads keep CPUs busy while others block on
   *  the clone mutex. */
  OptReal pool_cpu_factor {};

  /** @brief SDDP work pool load-average ceiling factor.
   *  Dispatch is blocked while
   *  `loadavg > pool_load_factor × would-be active workers`
   *  (and the pool is at ≥ 50 % of `max_threads`).
   *  Default 1.25 — leaves 25 % "load room" over the active-worker
   *  count.  Set 1.5 for a looser ceiling, 0.0 to disable the gate. */
  OptReal pool_load_factor {};

  /** @brief Process memory limit in MB for the SDDP work pool.
   *  When non-zero, the pool blocks task dispatch if process RSS exceeds
   *  this value.  Accepts values parsed by parse_memory_size (e.g. 5G).
   *  0 = no limit (default). */
  OptReal pool_memory_limit_mb {};

  /** @brief Maximum iteration spread between fastest and slowest scene
   *  when cut_sharing is none and multiple scenes exist.
   *
   * When > 0, the solver runs scenes asynchronously: each scene
   * progresses through its own forward/backward iteration loop.  The
   * pool's SDDPTaskKey priority naturally schedules slower scenes first
   * (lower iteration -> higher priority), self-regulating the spread.
   *
   * 0 = synchronous (current behavior, default).
   */
  OptInt max_async_spread {};

  /** @brief Optional LP solver configuration for SDDP forward pass.
   *
   * When set, these options are merged with the global
   * `PlanningOptions::solver_options`.  Forward-pass-specific options
   * take precedence over the global ones.
   *
   * Typical use: use barrier for the forward pass (fresh solves) while
   * using dual simplex for the backward pass (warm-started resolves).
   */
  std::optional<SolverOptions> forward_solver_options {};

  /** @brief Optional LP solver configuration for SDDP backward pass.
   *
   * When set, these options are merged with the global
   * `PlanningOptions::solver_options`.  Backward-pass-specific options
   * take precedence over the global ones.
   *
   * Typical use: use dual simplex for the backward pass.
   */
  std::optional<SolverOptions> backward_solver_options {};

  void merge(SddpOptions&& opts)
  {
    merge_opt(cut_sharing_mode, opts.cut_sharing_mode);
    merge_opt(cut_directory, std::move(opts.cut_directory));
    merge_opt(api_enabled, opts.api_enabled);
    merge_opt(update_lp_skip, opts.update_lp_skip);
    merge_opt(max_iterations, opts.max_iterations);
    merge_opt(min_iterations, opts.min_iterations);
    merge_opt(convergence_tol, opts.convergence_tol);
    merge_opt(elastic_penalty, opts.elastic_penalty);
    merge_opt(scale_alpha, opts.scale_alpha);
    merge_opt(cut_recovery_mode, opts.cut_recovery_mode);
    merge_opt(recovery_mode, opts.recovery_mode);
    merge_opt(cut_io_format, opts.cut_io_format);
    merge_opt(save_per_iteration, opts.save_per_iteration);
    merge_opt(cuts_input_file, std::move(opts.cuts_input_file));
    merge_opt(sentinel_file, std::move(opts.sentinel_file));
    merge_opt(elastic_mode, opts.elastic_mode);
    merge_opt(multi_cut_threshold, opts.multi_cut_threshold);
    merge_opt(apertures, std::move(opts.apertures));
    merge_opt(aperture_directory, std::move(opts.aperture_directory));
    merge_opt(aperture_timeout, opts.aperture_timeout);
    merge_opt(save_aperture_lp, opts.save_aperture_lp);
    merge_opt(boundary_cuts_file, std::move(opts.boundary_cuts_file));
    merge_opt(boundary_cuts_mode, opts.boundary_cuts_mode);
    merge_opt(boundary_max_iterations, opts.boundary_max_iterations);
    merge_opt(missing_cut_var_mode, opts.missing_cut_var_mode);
    merge_opt(named_cuts_file, std::move(opts.named_cuts_file));
    merge_opt(max_cuts_per_phase, opts.max_cuts_per_phase);
    merge_opt(cut_prune_interval, opts.cut_prune_interval);
    merge_opt(prune_dual_threshold, opts.prune_dual_threshold);
    merge_opt(single_cut_storage, opts.single_cut_storage);
    merge_opt(max_stored_cuts, opts.max_stored_cuts);
    merge_opt(simulation_mode, opts.simulation_mode);
    merge_opt(low_memory_mode, opts.low_memory_mode);
    merge_opt(memory_codec, opts.memory_codec);
    merge_opt(cut_coeff_eps, opts.cut_coeff_eps);
    merge_opt(state_variable_lookup_mode, opts.state_variable_lookup_mode);
    merge_opt(convergence_mode, opts.convergence_mode);
    merge_opt(stationary_tol, opts.stationary_tol);
    merge_opt(stationary_window, opts.stationary_window);
    merge_opt(convergence_confidence, opts.convergence_confidence);
    merge_opt(forward_max_fallbacks, opts.forward_max_fallbacks);
    merge_opt(forward_fail_stop, opts.forward_fail_stop);
    merge_opt(backward_max_fallbacks, opts.backward_max_fallbacks);
    merge_opt(max_async_spread, opts.max_async_spread);
    merge_opt(pool_cpu_factor, opts.pool_cpu_factor);
    merge_opt(pool_load_factor, opts.pool_load_factor);
    merge_opt(pool_memory_limit_mb, opts.pool_memory_limit_mb);
    if (opts.forward_solver_options.has_value()) {
      if (forward_solver_options.has_value()) {
        forward_solver_options->merge(*opts.forward_solver_options);
      } else {
        forward_solver_options = opts.forward_solver_options;
      }
    }
    if (opts.backward_solver_options.has_value()) {
      if (backward_solver_options.has_value()) {
        backward_solver_options->merge(*opts.backward_solver_options);
      } else {
        backward_solver_options = opts.backward_solver_options;
      }
    }

    auto _ = std::move(opts);
  }
};

}  // namespace gtopt
