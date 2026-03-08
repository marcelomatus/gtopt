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
 *     "input_directory": "input"
 *   }
 * }
 * ```
 */

#pragma once

#include <gtopt/utils.hpp>

namespace gtopt
{

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

    auto _ = std::move(opts);
  }
};

}  // namespace gtopt
