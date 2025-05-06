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

  /**
   * @brief Merges another Options object into this one
   *
   * The merge operation takes values from the source object (opts) and
   * sets them in the destination object (this) when the source has a value.
   * If the source field doesn't have a value, the destination field is
   * unchanged. In other words, non-null values in opts override corresponding
   * values in this.
   *
   * @param opts Source options to merge from
   * @return Reference to this object after merging
   */
  /**
   * @brief Merges another Options object into this one
   *
   * The merge operation takes values from the source object and
   * sets them in the destination object (this) when the source has a value.
   * If the source field doesn't have a value, the destination field is
   * unchanged. Non-null values in the source override corresponding values in
   * this.
   *
   * This is a unified template method that handles both lvalue and rvalue
   * references. When merging from an rvalue reference, move semantics are used
   * automatically.
   *
   * @tparam T Options reference type (can be lvalue or rvalue reference)
   * @param opts Source options to merge from (will be moved from if it's an
   * rvalue)
   * @return Reference to this object after merging
   */
  template<typename T>
  constexpr Options& merge(T&& opts)
  {
    // Merge input-related options (always moving string values)
    merge_opt(input_directory,
              std::move(std::forward<T>(opts).input_directory));
    merge_opt(input_format, std::move(std::forward<T>(opts).input_format));

    // Merge optimization parameters
    // Using std::forward to preserve value category (lvalue vs rvalue)
    merge_opt(demand_fail_cost, std::forward<T>(opts).demand_fail_cost);
    merge_opt(reserve_fail_cost, std::forward<T>(opts).reserve_fail_cost);
    merge_opt(use_line_losses, std::forward<T>(opts).use_line_losses);
    merge_opt(use_kirchhoff, std::forward<T>(opts).use_kirchhoff);
    merge_opt(use_single_bus, std::forward<T>(opts).use_single_bus);
    merge_opt(kirchhoff_threshold, std::forward<T>(opts).kirchhoff_threshold);
    merge_opt(scale_objective, std::forward<T>(opts).scale_objective);
    merge_opt(scale_theta, std::forward<T>(opts).scale_theta);

    // Merge output-related options (always moving string values)
    merge_opt(output_directory,
              std::move(std::forward<T>(opts).output_directory));
    merge_opt(output_format, std::move(std::forward<T>(opts).output_format));
    merge_opt(compression_format,
              std::move(std::forward<T>(opts).compression_format));
    merge_opt(use_lp_names, std::forward<T>(opts).use_lp_names);
    merge_opt(use_uid_fname, std::forward<T>(opts).use_uid_fname);
    merge_opt(annual_discount_rate, std::forward<T>(opts).annual_discount_rate);

    return *this;
  }
};

}  // namespace gtopt
