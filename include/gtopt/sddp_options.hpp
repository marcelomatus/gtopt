/**
 * @file      sddp_options.hpp
 * @brief     SDDP-specific solver configuration parameters
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/enum_option.hpp>
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
struct SddpOptions
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
  /** @brief Penalty for elastic slack variables in feasibility (default: 1e6)
   */
  OptReal elastic_penalty {};
  /** @brief Lower bound for future cost variable α (default: 0.0) */
  OptReal alpha_min {};
  /** @brief Upper bound for future cost variable α (default: 1e12) */
  OptReal alpha_max {};

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
  /** @brief Save cuts to CSV after each iteration (default: true).
   *  When false, cuts are only saved at the end of the solve or on stop. */
  OptBool save_per_iteration {};
  /** @brief File path for loading initial cuts (hot-start; empty = cold start)
   */
  OptName cuts_input_file {};
  /** @brief Path to a sentinel file; if it exists, the solver stops gracefully
   * after the current iteration (analogous to PLP's userstop) */
  OptName sentinel_file {};
  /** @brief Elastic filter mode: single_cut (default, alias "cut") or
   *         multi_cut or backpropagate */
  std::optional<ElasticFilterMode> elastic_mode {};
  /** @brief Forward-pass infeasibility count threshold for switching from
   *         single_cut to multi_cut (default: 10; 0 = never auto-switch) */
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

  /** @brief Enable warm-start for SDDP resolves.
   *  When true, previous forward-pass primal/dual solutions are loaded
   *  into clone LPs before resolving (backward pass, elastic filter,
   *  apertures).  Combined with solver_options.reuse_basis, this enables
   *  efficient incremental re-solves.  Default when unset: true. */
  OptBool warm_start {};

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
   * ```
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
   * ```
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
  /// Reuse cached LP clones for aperture solves.  Default: true.
  OptBool use_clone_pool {};

  /// Run in simulation mode: no training iterations (max_iterations=0),
  /// forward-only evaluation of the policy from loaded cuts.
  /// No cuts are saved.  Default: false.
  OptBool simulation_mode {};

  /** @brief State variable propagation mode between SDDP phases.
   *
   * - `last_iteration` (default): state vars are NOT pinned to the
   *   previous phase's solution.  Values come from the previous
   *   iteration's warm-start, recovered file state, or vini default.
   * - `inter_phase`: state vars are pinned to the previous phase's
   *   solution within the same forward pass (classic chaining).
   */
  std::optional<StatePropagation> state_propagation {};

  // ── Secondary (stationary gap) convergence ─────────────────────────────────
  /** @brief Tolerance for secondary stationary-gap convergence criterion.
   *
   * When the relative change in the convergence gap over the last
   * `stationary_window` iterations falls below this value, the solver
   * declares convergence even if the gap is above `convergence_tol`.
   * This handles problems where the gap converges to a non-zero stationary
   * value (a known theoretical limitation of SDDP/Benders on certain
   * stochastic programs).
   *
   * Formula (after at least `min_iterations` and `stationary_window`
   * iterations have completed):
   *   gap_change = |gap[i] − gap[i − window]| / max(1e-10, gap[i − window])
   *   if gap_change < stationary_tol → declare convergence
   *
   * Default: 0.0 (disabled; secondary criterion is off).
   * Set to a small positive value (e.g. 0.01) to enable.
   */
  OptReal stationary_tol {};

  /** @brief Number of iterations to look back when checking for a stationary
   * gap (secondary convergence criterion).  Only used when `stationary_tol`
   * is positive.  Default: 10.
   */
  OptInt stationary_window {};

  // ── LP solver options (per-pass override)
  // ───────────────────────────────────
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
   * Typical use: use dual simplex with reuse_basis for the backward pass
   * (warm-started resolves after adding cuts).
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
    merge_opt(alpha_min, opts.alpha_min);
    merge_opt(alpha_max, opts.alpha_max);
    merge_opt(cut_recovery_mode, opts.cut_recovery_mode);
    merge_opt(recovery_mode, opts.recovery_mode);
    merge_opt(save_per_iteration, opts.save_per_iteration);
    merge_opt(cuts_input_file, std::move(opts.cuts_input_file));
    merge_opt(sentinel_file, std::move(opts.sentinel_file));
    merge_opt(elastic_mode, opts.elastic_mode);
    merge_opt(multi_cut_threshold, opts.multi_cut_threshold);
    if (opts.apertures.has_value()) {
      apertures = std::move(opts.apertures);
    }
    merge_opt(aperture_directory, std::move(opts.aperture_directory));
    merge_opt(aperture_timeout, opts.aperture_timeout);
    merge_opt(save_aperture_lp, opts.save_aperture_lp);
    merge_opt(boundary_cuts_file, std::move(opts.boundary_cuts_file));
    merge_opt(boundary_cuts_mode, opts.boundary_cuts_mode);
    merge_opt(boundary_max_iterations, opts.boundary_max_iterations);
    merge_opt(named_cuts_file, std::move(opts.named_cuts_file));
    merge_opt(max_cuts_per_phase, opts.max_cuts_per_phase);
    merge_opt(cut_prune_interval, opts.cut_prune_interval);
    merge_opt(prune_dual_threshold, opts.prune_dual_threshold);
    merge_opt(single_cut_storage, opts.single_cut_storage);
    merge_opt(max_stored_cuts, opts.max_stored_cuts);
    merge_opt(use_clone_pool, opts.use_clone_pool);
    merge_opt(simulation_mode, opts.simulation_mode);
    merge_opt(state_propagation, opts.state_propagation);
    merge_opt(warm_start, opts.warm_start);
    merge_opt(stationary_tol, opts.stationary_tol);
    merge_opt(stationary_window, opts.stationary_window);
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
