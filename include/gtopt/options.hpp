/**
 * @file      options.hpp
 * @brief     Configuration options for power system optimization
 * @date      Sun Mar 23 21:39:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Options structure that contains configuration
 * parameters for power system optimization. Options include input/output
 * settings, solver parameters, modeling choices, and cost factors.
 *
 * ### JSON Example
 * ```json
 * {
 *   "options": {
 *     "solver_type": "sddp",
 *     "demand_fail_cost": 1000,
 *     "use_kirchhoff": true,
 *     "scale_objective": 1000,
 *     "annual_discount_rate": 0.1,
 *     "output_format": "parquet",
 *     "input_directory": "input",
 *     "sddp_options": {
 *       "cut_sharing_mode": "expected",
 *       "cut_directory": "cuts",
 *       "api_enabled": true,
 *       "efficiency_update_skip": 0,
 *       "elastic_mode": "single_cut",
 *       "multi_cut_threshold": 10
 *     }
 *   }
 * }
 * ```
 */

#pragma once

#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/variable_scale.hpp>

namespace gtopt
{

/**
 * @brief SDDP-specific solver configuration parameters
 *
 * Groups all SDDP-related options into a single sub-object for clearer
 * JSON organization.  Field names omit the `sddp_` prefix since they
 * already live inside the `sddp_options` namespace.
 *
 * All fields are optional — defaults are applied via `OptionsLP`.
 */
struct SddpOptions
{
  /** @brief Cut sharing mode: `"none"` (default), `"expected"`,
   *  `"accumulate"`, or `"max"` */
  OptName cut_sharing_mode {};
  /** @brief Directory for Benders cut files (default: `"cuts"`) */
  OptName cut_directory {};
  /** @brief Enable the SDDP monitoring API (writes JSON status file each
   * iteration; default: true) */
  OptBool api_enabled {};
  /** @brief Global default for iterations to skip between efficiency
   * coefficient updates.  0 = update every iteration (PLP default). */
  OptInt efficiency_update_skip {};

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
  /** @brief Enable hot-start from previously saved cuts (default: false).
   *  When true and no explicit `cuts_input_file` is given, the solver
   *  loads cuts from the `cut_directory`. */
  OptBool hot_start {};
  /** @brief Save cuts to CSV after each iteration (default: true).
   *  When false, cuts are only saved at the end of the solve or on stop. */
  OptBool save_per_iteration {};
  /** @brief File path for loading initial cuts (hot-start; empty = cold start)
   */
  OptName cuts_input_file {};
  /** @brief Path to a sentinel file; if it exists, the solver stops gracefully
   * after the current iteration (analogous to PLP's userstop) */
  OptName sentinel_file {};
  /** @brief Elastic filter mode: `"single_cut"` (default, alias `"cut"`) or
   *         `"multi_cut"` or `"backpropagate"` */
  OptName elastic_mode {};
  /** @brief Forward-pass infeasibility count threshold for switching from
   *         single_cut to multi_cut (default: 10; 0 = never auto-switch) */
  OptInt multi_cut_threshold {};
  /** @brief Number of apertures (hydrological realisations) for the backward
   *         pass. 0 = disabled (default); -1 = all scenarios; N > 0 = first N
   */
  OptInt num_apertures {};
  /** @brief Directory for aperture-specific scenario data.
   *
   * When present, scenarios referenced by `Aperture::source_scenario` are
   * first looked up in this directory.  If not found there, they fall back
   * to the regular `input_directory`.  This allows backward-pass apertures
   * to use different affluent data than the forward-pass scenarios.
   */
  OptName aperture_directory {};

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

  /** @brief How boundary cuts are loaded: `"noload"`, `"separated"` (default),
   * or `"combined"`.
   *
   * - `"noload"` — do not load boundary cuts even if a file is given.
   * - `"separated"` — load cuts per scene: each cut is assigned to the
   *   scene matching its `scene` column (scene UID from `scene_array`).
   * - `"combined"` — load all cuts into all scenes (broadcast).
   */
  OptName boundary_cuts_mode {};

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

  void merge(SddpOptions&& opts)
  {
    merge_opt(cut_sharing_mode, std::move(opts.cut_sharing_mode));
    merge_opt(cut_directory, std::move(opts.cut_directory));
    merge_opt(api_enabled, opts.api_enabled);
    merge_opt(efficiency_update_skip, opts.efficiency_update_skip);
    merge_opt(max_iterations, opts.max_iterations);
    merge_opt(min_iterations, opts.min_iterations);
    merge_opt(convergence_tol, opts.convergence_tol);
    merge_opt(elastic_penalty, opts.elastic_penalty);
    merge_opt(alpha_min, opts.alpha_min);
    merge_opt(alpha_max, opts.alpha_max);
    merge_opt(hot_start, opts.hot_start);
    merge_opt(save_per_iteration, opts.save_per_iteration);
    merge_opt(cuts_input_file, std::move(opts.cuts_input_file));
    merge_opt(sentinel_file, std::move(opts.sentinel_file));
    merge_opt(elastic_mode, std::move(opts.elastic_mode));
    merge_opt(multi_cut_threshold, opts.multi_cut_threshold);
    merge_opt(num_apertures, opts.num_apertures);
    merge_opt(aperture_directory, std::move(opts.aperture_directory));
    merge_opt(boundary_cuts_file, std::move(opts.boundary_cuts_file));
    merge_opt(boundary_cuts_mode, std::move(opts.boundary_cuts_mode));
    merge_opt(boundary_max_iterations, opts.boundary_max_iterations);
    merge_opt(named_cuts_file, std::move(opts.named_cuts_file));

    auto _ = std::move(opts);
  }
};

/**
 * @brief Monolithic solver configuration parameters
 *
 * Groups monolithic-solver-specific options into a single sub-object for
 * clearer JSON organization.  All fields are optional — defaults are
 * applied via `OptionsLP`.
 */
struct MonolithicOptions
{
  /** @brief Solve mode: `"monolithic"` (default) or `"sequential"` */
  OptName solve_mode {};
  /** @brief CSV file with boundary (future-cost) cuts.
   *
   * When non-empty, the monolithic solver loads boundary cuts from this
   * file before solving.  The cuts approximate the expected future cost
   * beyond the planning horizon (analogous to SDDP boundary cuts).
   */
  OptName boundary_cuts_file {};
  /** @brief Boundary cuts load mode: `"noload"`, `"separated"` (default),
   * or `"combined"` */
  OptName boundary_cuts_mode {};
  /** @brief Maximum iterations to load from boundary cuts file (0 = all) */
  OptInt boundary_max_iterations {};

  void merge(MonolithicOptions&& opts)
  {
    merge_opt(solve_mode, std::move(opts.solve_mode));
    merge_opt(boundary_cuts_file, std::move(opts.boundary_cuts_file));
    merge_opt(boundary_cuts_mode, std::move(opts.boundary_cuts_mode));
    merge_opt(boundary_max_iterations, opts.boundary_max_iterations);

    auto _ = std::move(opts);
  }
};

/**
 * @brief Global configuration parameters for the optimization model
 *
 * All fields are optional, allowing partial specification and merging across
 * multiple JSON files.  When a field is absent, the solver applies a built-in
 * default (see `OptionsLP` for the resolved defaults).
 */
struct Options
{
  // ── Input settings ─────────────────────────────────────────────────────────
  /** @brief Root directory for external input data files (CSV/Parquet) */
  OptName input_directory {};
  /** @brief Preferred format for reading external files: `"parquet"` or `"csv"`
   */
  OptName input_format {};

  // ── Model parameters ───────────────────────────────────────────────────────
  /** @brief Penalty cost for unserved demand (load shedding) [$/MWh] */
  OptReal demand_fail_cost {};
  /** @brief Penalty cost for unserved spinning-reserve requirement [$/MWh] */
  OptReal reserve_fail_cost {};
  /** @brief Whether to model resistive line losses (default: true) */
  OptBool use_line_losses {};
  /** @brief Default number of piecewise-linear segments for quadratic line
   * losses (default: 1, meaning linear model only) */
  OptInt loss_segments {};
  /** @brief Whether to apply DC Kirchhoff voltage-law constraints (default:
   * false) */
  OptBool use_kirchhoff {};
  /** @brief Whether to collapse the network to a single bus (copper-plate
   * model) */
  OptBool use_single_bus {};
  /** @brief Minimum bus voltage [kV] below which Kirchhoff is not applied [kV]
   */
  OptReal kirchhoff_threshold {};
  /** @brief Divisor applied to all objective coefficients for numerical
   * stability [dimensionless] */
  OptReal scale_objective {};
  /** @brief Scaling factor for voltage-angle variables [dimensionless] */
  OptReal scale_theta {};
  /** @brief Annual discount rate for multi-stage CAPEX calculations [p.u./year]
   */
  OptReal annual_discount_rate {};

  // ── Output settings ────────────────────────────────────────────────────────
  /** @brief Root directory for output result files */
  OptName output_directory {};
  /** @brief Format for output files: `"parquet"` (default) or `"csv"` */
  OptName output_format {};
  /** @brief Compression codec for Parquet output: `"gzip"` (default), `"zstd"`,
   * `"uncompressed"` */
  OptName output_compression {};
  /** @brief LP naming level: 0=none, 1=names+warn (default),
   * 2=names+error.
   *
   * Level 0 disables LP names entirely (smallest memory footprint).
   * Level 1 assigns names, populates name-to-index maps, and warns on
   *   duplicate row/column names.
   * Level 2 assigns names, populates maps, and throws on duplicates.
   *
   * Backward-compatible: JSON `true` maps to 1, `false` to 0.
   */
  OptInt use_lp_names {};
  /** @brief Use element UIDs instead of names in output filenames */
  OptBool use_uid_fname {};

  // ── Solver algorithm settings (deprecated: use solver_options instead) ────
  /** @brief @deprecated Use `solver_options.algorithm` instead.
   * LP algorithm: 0=auto, 1=primal simplex, 2=dual simplex, 3=barrier */
  OptInt lp_algorithm {};
  /** @brief @deprecated Use `solver_options.threads` instead.
   * Number of solver threads (0=automatic) [dimensionless] */
  OptInt lp_threads {};
  /** @brief @deprecated Use `solver_options.presolve` instead.
   * Whether to apply the solver's built-in presolve (default: true) */
  OptBool lp_presolve {};

  /** @brief Planning solver type: `"monolithic"` (default) or `"sddp"`.
   *
   * This is the only supported way to select the solver.
   * Example:
   *
   * ```json
   * { "options": { "solver_type": "sddp" } }
   * ```
   */
  OptName solver_type {};

  // ── Logging ────────────────────────────────────────────────────────────────
  /** @brief Directory for log and trace files (default: `"logs"`).
   * Used for error LP dumps (both monolithic and SDDP) and SDDP iteration
   * logs. */
  OptName log_directory {};
  /** @brief When true, save LP debug files to the log directory.
   * Monolithic solver: saves one LP file per (scene, phase) after building the
   * model.  SDDP solver: saves one LP file per (iteration, scene, phase) during
   * the forward pass. */
  OptBool lp_debug {};
  /** @brief Compression codec for LP debug files.
   *
   * Controls how LP debug files (`lp_debug=true`) are compressed.
   *
   * - `""` / not set (default): let `gtopt_compress_lp` decide; falls back to
   *   `zstd` (inline libzstd), then `gzip`, then inline zlib when the script
   *   is not available.
   * - `"none"`: never compress; keep plain `.lp` files.
   * - `"gzip"`, `"zstd"`, `"lz4"`, `"bzip2"`, `"xz"`: request a specific
   *   codec.  The value is passed as `--codec <codec>` to `gtopt_compress_lp`;
   *   if the script or codec is unavailable the named binary is tried directly,
   *   then `zstd`, then `gzip`, then inline zlib.
   */
  OptName lp_compression {};
  /** @brief When true, build all scene/phase LP matrices but skip solving.
   * Both the monolithic and SDDP solvers exit immediately after LP matrix
   * assembly — no solving occurs at all.
   * Combine with `lp_debug=true` to save every scene/phase LP file. */
  OptBool just_build_lp {};

  /** @brief LP coefficient ratio threshold for numerical conditioning
   * diagnostics.  When the global max/min |coefficient| ratio exceeds this
   * value, a per-scene/phase breakdown is printed.  (default: 1e7) */
  OptReal lp_coeff_ratio_threshold {};

  // ── Monolithic-specific options (grouped sub-object)
  // ────────────────────────
  /** @brief Monolithic solver configuration (sub-object) */
  MonolithicOptions monolithic_options {};

  // ── SDDP-specific options (grouped sub-object) ────────────────────────────
  /** @brief SDDP solver configuration (sub-object) */
  SddpOptions sddp_options {};

  // ── LP solver options (grouped sub-object) ────────────────────────────────
  /** @brief LP solver configuration (algorithm, tolerances, threads, etc.)
   *
   * Exposes the full @c SolverOptions struct as a JSON sub-object so that
   * users can set LP solver parameters — including the optional tolerance
   * values — directly in the planning JSON:
   *
   * ```json
   * { "options": { "solver_options": { "algorithm": 3,
   *                                    "optimal_eps": 1e-8,
   *                                    "feasible_eps": 1e-8 } } }
   * ```
   *
   * Individual top-level fields (@c lp_algorithm, @c lp_threads,
   * @c lp_presolve) are still respected for backward compatibility and take
   * precedence over the corresponding @c solver_options sub-fields.
   */
  SolverOptions solver_options {};

  // ── Variable scaling ──────────────────────────────────────────────────────
  /** @brief Per-class/variable LP scale overrides.
   *
   * Provides a uniform, extensible mechanism for defining LP variable scale
   * factors via JSON.  Each entry maps a (class, variable, optional UID)
   * triple to a scale factor where `physical = LP × scale`.
   *
   * Per-element fields (`Battery::energy_scale`, `Reservoir::vol_scale`) and
   * global options (`scale_theta`) take precedence over entries here.
   *
   * ### JSON Example
   * ```json
   * {
   *   "options": {
   *     "variable_scales": [
   *       {"class_name": "Bus",       "variable": "theta",   "uid": -1,
   *        "scale": 0.001},
   *       {"class_name": "Reservoir",  "variable": "volume",  "uid": -1,
   *        "scale": 1000.0},
   *       {"class_name": "Battery",    "variable": "energy",  "uid": 1,
   *        "scale": 10.0}
   *     ]
   *   }
   * }
   * ```
   */
  Array<VariableScale> variable_scales {};

  void merge(Options&& opts)
  {
    // Merge input-related options (always moving string values)
    merge_opt(input_directory, std::move(opts.input_directory));
    merge_opt(input_format, std::move(opts.input_format));

    // Merge optimization parameters

    merge_opt(demand_fail_cost, opts.demand_fail_cost);
    merge_opt(reserve_fail_cost, opts.reserve_fail_cost);
    merge_opt(use_line_losses, opts.use_line_losses);
    merge_opt(loss_segments, opts.loss_segments);
    merge_opt(use_kirchhoff, opts.use_kirchhoff);
    merge_opt(use_single_bus, opts.use_single_bus);
    merge_opt(kirchhoff_threshold, opts.kirchhoff_threshold);
    merge_opt(scale_objective, opts.scale_objective);
    merge_opt(scale_theta, opts.scale_theta);

    // Merge output-related options (always moving string values)
    merge_opt(output_directory, std::move(opts.output_directory));
    merge_opt(output_format, std::move(opts.output_format));
    merge_opt(output_compression, std::move(opts.output_compression));

    merge_opt(use_lp_names, opts.use_lp_names);
    merge_opt(use_uid_fname, opts.use_uid_fname);
    merge_opt(annual_discount_rate, opts.annual_discount_rate);

    // Merge solver algorithm settings
    merge_opt(lp_algorithm, opts.lp_algorithm);
    merge_opt(lp_threads, opts.lp_threads);
    merge_opt(lp_presolve, opts.lp_presolve);
    merge_opt(solver_type, std::move(opts.solver_type));
    merge_opt(log_directory, std::move(opts.log_directory));
    merge_opt(lp_debug, opts.lp_debug);
    merge_opt(lp_compression, std::move(opts.lp_compression));
    merge_opt(just_build_lp, opts.just_build_lp);
    merge_opt(lp_coeff_ratio_threshold, opts.lp_coeff_ratio_threshold);

    // Merge monolithic-specific options
    monolithic_options.merge(std::move(opts.monolithic_options));

    // Merge SDDP-specific options
    sddp_options.merge(std::move(opts.sddp_options));

    // Merge LP solver options (only optional tolerance fields are merged;
    // non-optional fields in the first file win)
    solver_options.merge(opts.solver_options);

    // Merge variable scales (append incoming entries)
    if (!opts.variable_scales.empty()) {
      variable_scales.insert(
          variable_scales.end(),
          std::make_move_iterator(opts.variable_scales.begin()),
          std::make_move_iterator(opts.variable_scales.end()));
    }

    auto _ = std::move(opts);
  }
};

}  // namespace gtopt
