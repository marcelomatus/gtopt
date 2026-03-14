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
 *     "demand_fail_cost": 1000,
 *     "use_kirchhoff": true,
 *     "scale_objective": 1000,
 *     "annual_discount_rate": 0.1,
 *     "output_format": "parquet",
 *     "input_directory": "input",
 *     "sddp_options": {
 *       "sddp_solver_type": "sddp",
 *       "sddp_cut_sharing_mode": "expected",
 *       "sddp_cut_directory": "cuts",
 *       "sddp_api_enabled": true,
 *       "sddp_efficiency_update_skip": 0,
 *       "sddp_elastic_mode": "single-cut",
 *       "sddp_multi_cut_threshold": 10
 *     }
 *   }
 * }
 * ```
 */

#pragma once

#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief SDDP-specific solver configuration parameters
 *
 * Groups all SDDP-related options into a single sub-object for clearer
 * JSON organization and consistent `sddp_` prefix naming.
 *
 * All fields are optional — defaults are applied via `OptionsLP`.
 */
struct SddpOptions
{
  /** @brief Solver type: `"monolithic"` (default) or `"sddp"` */
  OptName sddp_solver_type {};
  /** @brief Cut sharing mode: `"none"`, `"expected"`, or `"max"` */
  OptName sddp_cut_sharing_mode {};
  /** @brief Directory for Benders cut files (default: `"cuts"`) */
  OptName sddp_cut_directory {};
  /** @brief Enable the SDDP monitoring API (writes JSON status file each
   * iteration; default: true) */
  OptBool sddp_api_enabled {};
  /** @brief Global default for iterations to skip between efficiency
   * coefficient updates.  0 = update every iteration (PLP default). */
  OptInt sddp_efficiency_update_skip {};

  // ── Iteration control ──────────────────────────────────────────────────────
  /** @brief Maximum number of forward/backward iterations (default: 100) */
  OptInt sddp_max_iterations {};
  /** @brief Relative gap tolerance for convergence (default: 1e-4) */
  OptReal sddp_convergence_tol {};

  // ── Advanced tuning ────────────────────────────────────────────────────────
  /** @brief Penalty for elastic slack variables in feasibility (default: 1e6)
   */
  OptReal sddp_elastic_penalty {};
  /** @brief Lower bound for future cost variable α (default: 0.0) */
  OptReal sddp_alpha_min {};
  /** @brief Upper bound for future cost variable α (default: 1e12) */
  OptReal sddp_alpha_max {};

  // ── Cut file management ────────────────────────────────────────────────────
  /** @brief File path for loading initial cuts (hot-start; empty = cold start)
   */
  OptName sddp_cuts_input_file {};
  /** @brief Path to a sentinel file; if it exists, the solver stops gracefully
   * after the current iteration (analogous to PLP's userstop) */
  OptName sddp_sentinel_file {};
  /** @brief Elastic filter mode: `"single-cut"` (default, alias `"cut"`) or
   *         `"multi-cut"` or `"backpropagate"` */
  OptName sddp_elastic_mode {};
  /** @brief Forward-pass infeasibility count threshold for switching from
   *         single-cut to multi-cut (default: 10; 0 = never auto-switch) */
  OptInt sddp_multi_cut_threshold {};
  /** @brief Number of apertures (hydrological realisations) for the backward
   *         pass. 0 = disabled (default); -1 = all scenarios; N > 0 = first N
   */
  OptInt sddp_num_apertures {};
  /** @brief Directory for aperture-specific scenario data.
   *
   * When present, scenarios referenced by `Aperture::source_scenario` are
   * first looked up in this directory.  If not found there, they fall back
   * to the regular `input_directory`.  This allows backward-pass apertures
   * to use different affluent data than the forward-pass scenarios.
   */
  OptName sddp_aperture_directory {};

  /** @brief CSV file with boundary (future-cost) cuts for the last phase.
   *
   * These are analogous to PLP's "planos de embalse" — external optimality
   * cuts that approximate the expected future cost beyond the planning
   * horizon.  Each cut is of the form:
   *
   *   α ≥ rhs + Σ_i  coeff_i · state_var_i
   *
   * The CSV header row names the state variables (reservoir / battery);
   * subsequent rows provide the cut name, scenario index, RHS, and
   * gradient coefficients.
   *
   * Format:
   * ```
   * name,scenario,rhs,Reservoir1,Reservoir2,...
   * cut_001,0,-5000.0,0.25,0.75,...
   * ```
   *
   * The solver maps column headers to the LP state-variable columns in the
   * last phase and adds each cut as a lower-bound constraint on the future
   * cost variable α.  If empty, no boundary cuts are loaded.
   */
  OptName sddp_boundary_cuts_file {};

  void merge(SddpOptions&& opts)
  {
    merge_opt(sddp_solver_type, std::move(opts.sddp_solver_type));
    merge_opt(sddp_cut_sharing_mode, std::move(opts.sddp_cut_sharing_mode));
    merge_opt(sddp_cut_directory, std::move(opts.sddp_cut_directory));
    merge_opt(sddp_api_enabled, opts.sddp_api_enabled);
    merge_opt(sddp_efficiency_update_skip, opts.sddp_efficiency_update_skip);
    merge_opt(sddp_max_iterations, opts.sddp_max_iterations);
    merge_opt(sddp_convergence_tol, opts.sddp_convergence_tol);
    merge_opt(sddp_elastic_penalty, opts.sddp_elastic_penalty);
    merge_opt(sddp_alpha_min, opts.sddp_alpha_min);
    merge_opt(sddp_alpha_max, opts.sddp_alpha_max);
    merge_opt(sddp_cuts_input_file, std::move(opts.sddp_cuts_input_file));
    merge_opt(sddp_sentinel_file, std::move(opts.sddp_sentinel_file));
    merge_opt(sddp_elastic_mode, std::move(opts.sddp_elastic_mode));
    merge_opt(sddp_multi_cut_threshold, opts.sddp_multi_cut_threshold);
    merge_opt(sddp_num_apertures, opts.sddp_num_apertures);
    merge_opt(sddp_aperture_directory, std::move(opts.sddp_aperture_directory));
    merge_opt(sddp_boundary_cuts_file, std::move(opts.sddp_boundary_cuts_file));

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
  /** @brief Use descriptive variable names in the LP model (aids debugging) */
  OptBool use_lp_names {};
  /** @brief Use element UIDs instead of names in output filenames */
  OptBool use_uid_fname {};

  // ── Solver algorithm settings ──────────────────────────────────────────────
  /** @brief LP algorithm: 0=auto, 1=primal simplex, 2=dual simplex, 3=barrier
   */
  OptInt lp_algorithm {};
  /** @brief Number of solver threads (0=automatic) [dimensionless] */
  OptInt lp_threads {};
  /** @brief Whether to apply the solver's built-in presolve (default: true) */
  OptBool lp_presolve {};

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

  // ── SDDP-specific options (grouped sub-object) ────────────────────────────
  /** @brief SDDP solver configuration (sub-object with sddp_* fields) */
  SddpOptions sddp_options {};

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
    merge_opt(log_directory, std::move(opts.log_directory));
    merge_opt(lp_debug, opts.lp_debug);

    // Merge SDDP-specific options
    sddp_options.merge(std::move(opts.sddp_options));

    auto _ = std::move(opts);
  }
};

}  // namespace gtopt
