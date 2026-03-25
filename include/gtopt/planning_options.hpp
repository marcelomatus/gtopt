/**
 * @file      planning_options.hpp
 * @brief     Global configuration parameters for power system optimization
 * @date      Sun Mar 23 21:39:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the PlanningOptions structure that contains configuration
 * parameters for power system optimization. PlanningOptions include
 * input/output settings, solver parameters, modeling choices, and cost factors.
 *
 * ### JSON Example
 * ```json
 * {
 *   "options": {
 *     "method": "sddp",
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
 *       "update_lp_skip": 0,
 *       "elastic_mode": "single_cut",
 *       "multi_cut_threshold": 10
 *     }
 *   }
 * }
 * ```
 */

#pragma once

#include <gtopt/cascade_options.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/lp_build_options.hpp>
#include <gtopt/model_options.hpp>
#include <gtopt/monolithic_options.hpp>
#include <gtopt/sddp_options.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/variable_scale.hpp>

namespace gtopt
{

/**
 * @brief Global configuration parameters for the optimization model
 *
 * All fields are optional, allowing partial specification and merging across
 * multiple JSON files.  When a field is absent, the solver applies a built-in
 * default (see `PlanningOptionsLP` for the resolved defaults).
 */
struct PlanningOptions
{
  // ── Input settings ─────────────────────────────────────────────────────────
  /** @brief Root directory for external input data files (CSV/Parquet) */
  OptName input_directory {};
  /** @brief Preferred format for reading external files: parquet or csv */
  std::optional<DataFormat> input_format {};

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
  /** @brief Format for output files: parquet (default) or csv */
  std::optional<DataFormat> output_format {};
  /** @brief Compression codec for Parquet output: gzip (default), zstd,
   * uncompressed */
  std::optional<CompressionCodec> output_compression {};
  /** @brief Use element UIDs instead of names in output filenames */
  OptBool use_uid_fname {};

  /** @brief Planning solver type: monolithic (default), sddp, or cascade.
   *
   * This is the only supported way to select the solver.
   * Example:
   *
   * ```json
   * { "options": { "method": "cascade" } }
   * ```
   */
  std::optional<MethodType> method {};

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
   * - not set (default): let `gtopt_compress_lp` decide; falls back to
   *   `zstd` (inline libzstd), then `gzip`, then inline zlib when the script
   *   is not available.
   * - `uncompressed`: never compress; keep plain `.lp` files.
   * - `gzip`, `zstd`, `lz4`, `bzip2`, `xz`: request a specific
   *   codec.  The value is passed as `--codec <codec>` to `gtopt_compress_lp`;
   *   if the script or codec is unavailable the named binary is tried directly,
   *   then `zstd`, then `gzip`, then inline zlib.
   */
  std::optional<CompressionCodec> lp_compression {};
  /** @brief When true, build all scene/phase LP matrices but skip solving.
   * Both the monolithic and SDDP solvers exit immediately after LP matrix
   * assembly — no solving occurs at all.
   * Combine with `lp_debug=true` to save every scene/phase LP file. */
  OptBool lp_build {};

  // Note: solve_timeout is per-solver (sddp_options and monolithic_options)
  // with different defaults: 180s for SDDP, 18000s for monolithic.

  // ── Model options (grouped sub-object) ──────────────────────────────────
  /** @brief Power system model configuration (sub-object).
   *
   * New preferred location for LP-construction options.  Fields here
   * are overridden by the corresponding flat fields in PlanningOptions when
   * both are set (backward compatibility).
   */
  ModelOptions model_options {};

  // ── Monolithic-specific options (grouped sub-object)
  // ────────────────────────
  /** @brief Monolithic solver configuration (sub-object) */
  MonolithicOptions monolithic_options {};

  // ── SDDP-specific options (grouped sub-object) ────────────────────────────
  /** @brief SDDP solver configuration (sub-object) */
  SddpOptions sddp_options {};

  // ── Cascade-specific options (grouped sub-object) ────────────────────────
  /** @brief Cascade solver configuration (sub-object) */
  CascadeOptions cascade_options {};

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

  // ── LP build options (grouped sub-object) ──────────────────────────────
  /** @brief LP matrix assembly configuration (epsilon, naming, stats, etc.)
   *
   * Exposes the full @c LpBuildOptions struct as a JSON sub-object so that
   * users can set LP assembly parameters directly in the planning JSON:
   *
   * ```json
   * { "options": { "lp_build_options": { "eps": 1e-10,
   *                                      "compute_stats": true } } }
   * ```
   */
  LpBuildOptions lp_build_options {};

  // ── Variable scaling ──────────────────────────────────────────────────────
  /** @brief Per-class/variable LP scale overrides.
   *
   * Provides a uniform, extensible mechanism for defining LP variable scale
   * factors via JSON.  Each entry maps a (class, variable, optional UID)
   * triple to a scale factor where `physical = LP × scale`.
   *
   * Per-element fields (`Battery::energy_scale`, `Reservoir::energy_scale`)
   * and global options (`scale_theta`) take precedence over entries here.
   *
   * ### JSON Example
   * ```json
   * {
   *   "options": {
   *     "variable_scales": [
   *       {"class_name": "Bus",       "variable": "theta",   "uid": -1,
   *        "scale": 0.001},
   *       {"class_name": "Reservoir",  "variable": "energy",  "uid": -1,
   *        "scale": 1000.0},
   *       {"class_name": "Battery",    "variable": "energy",  "uid": 1,
   *        "scale": 10.0}
   *     ]
   *   }
   * }
   * ```
   */
  Array<VariableScale> variable_scales {};

  void merge(PlanningOptions&& opts)
  {
    // Merge input-related options (always moving string values)
    merge_opt(input_directory, std::move(opts.input_directory));
    merge_opt(input_format, opts.input_format);

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
    merge_opt(output_format, opts.output_format);
    merge_opt(output_compression, opts.output_compression);

    merge_opt(use_uid_fname, opts.use_uid_fname);
    merge_opt(annual_discount_rate, opts.annual_discount_rate);

    // Merge solver settings
    merge_opt(method, opts.method);
    merge_opt(log_directory, std::move(opts.log_directory));
    merge_opt(lp_debug, opts.lp_debug);
    merge_opt(lp_compression, opts.lp_compression);
    merge_opt(lp_build, opts.lp_build);
    // solve_timeout is per-solver (sddp_options, monolithic_options)

    // Merge model options
    model_options.merge(opts.model_options);

    // Merge monolithic-specific options
    monolithic_options.merge(std::move(opts.monolithic_options));

    // Merge SDDP-specific options
    sddp_options.merge(std::move(opts.sddp_options));

    // Merge Cascade-specific options
    cascade_options.merge(std::move(opts.cascade_options));

    // Merge LP solver options (only optional tolerance fields are merged;
    // non-optional fields in the first file win)
    solver_options.merge(opts.solver_options);

    // Merge LP build options
    merge_opt(lp_build_options.names_level, opts.lp_build_options.names_level);
    merge_opt(lp_build_options.lp_coeff_ratio_threshold,
              opts.lp_build_options.lp_coeff_ratio_threshold);

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

/// @brief Backward-compatibility alias (deprecated — use PlanningOptions)
using Options = PlanningOptions;

}  // namespace gtopt
