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
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief Configuration options for power system optimization
 *
 * The Options structure contains all the configuration parameters that control
 * how the optimization model is constructed, solved, and output. All fields are
 * optional, allowing for partial specification and merging of different option
 * sets.
 */
struct Options
{
  // Input settings
  /** @brief Directory path for input data files */
  OptName input_directory {};
  /** @brief Format of input files (e.g., "json", "parquet") */
  OptName input_format {};

  // Optimization parameters
  /** @brief Cost penalty for demand curtailment ($/MWh) */
  OptReal demand_fail_cost {};
  /** @brief Cost penalty for reserve shortfall ($/MWh) */
  OptReal reserve_fail_cost {};
  /** @brief Whether to include transmission line losses in the model */
  OptBool use_line_losses {};
  /** @brief Whether to apply Kirchhoff's voltage law constraints */
  OptBool use_kirchhoff {};
  /** @brief Whether to model the system as a single-bus (copper plate) */
  OptBool use_single_bus {};
  /** @brief Threshold for enforcing Kirchhoff constraints (minimum impedance)
   */
  OptReal kirchhoff_threshold {};
  /** @brief Scaling factor for the objective function (for numerical stability)
   */
  OptReal scale_objective {};
  /** @brief Scaling factor for voltage angle variables (for numerical
   * stability) */
  OptReal scale_theta {};

  // Output settings
  /** @brief Directory path for output results */
  OptName output_directory {};
  /** @brief Format of output files (e.g., "csv", "parquet") */
  OptName output_format {};
  /** @brief Compression format for output files (e.g., "gzip") */
  OptName compression_format {};
  /** @brief Whether to use descriptive names in LP model for debugging */
  OptBool use_lp_names {};
  /** @brief Whether to use UIDs in filenames instead of names */
  OptBool use_uid_fname {};
  /** @brief Annual discount rate for multi-year planning (per unit) */
  OptReal annual_discount_rate {};

  void merge(Options&& opts)  // NOLINT
  {
    // Merge input-related options (always moving string values)
    merge_opt(input_directory, std::move(opts.input_directory));
    merge_opt(input_format, std::move(opts.input_format));

    // Merge optimization parameters

    merge_opt(demand_fail_cost, opts.demand_fail_cost);
    merge_opt(reserve_fail_cost, opts.reserve_fail_cost);
    merge_opt(use_line_losses, opts.use_line_losses);
    merge_opt(use_kirchhoff, opts.use_kirchhoff);
    merge_opt(use_single_bus, opts.use_single_bus);
    merge_opt(kirchhoff_threshold, opts.kirchhoff_threshold);
    merge_opt(scale_objective, opts.scale_objective);
    merge_opt(scale_theta, opts.scale_theta);

    // Merge output-related options (always moving string values)
    merge_opt(output_directory, std::move(opts.output_directory));
    merge_opt(output_format, std::move(opts.output_format));
    merge_opt(compression_format, std::move(opts.compression_format));

    merge_opt(use_lp_names, opts.use_lp_names);
    merge_opt(use_uid_fname, opts.use_uid_fname);
    merge_opt(annual_discount_rate, opts.annual_discount_rate);
  }
};

}  // namespace gtopt
