/**
 * @file      options_lp.hpp
 * @brief     Wrapper for Options with default value handling
 * @date      Tue Apr 22 03:21:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the OptionsLP class, which wraps the Options structure
 * and provides accessors with default values for optional parameters. This
 * allows consistent and easy access to option values throughout the linear
 * programming (LP) optimization processes.
 */

#pragma once

#include <gtopt/options.hpp>

namespace gtopt
{

/**
 * @brief Wrapper for Options with default value handling
 *
 * OptionsLP wraps an Options structure and provides accessor methods that
 * automatically apply default values when options are not explicitly set.
 * This ensures consistent behavior throughout the optimization code by
 * defining system-wide defaults in one place.
 */
class OptionsLP
{
public:
  // Default values for input settings
  /** @brief Default input directory path */
  static constexpr auto default_input_directory = "input";
  /** @brief Default input file format */
  static constexpr auto default_input_format = "parquet";

  // Default values for optimization parameters
  /** @brief Default setting for line loss modeling */
  static constexpr Bool default_use_line_losses = true;
  /** @brief Default setting for Kirchhoff constraints */
  static constexpr Bool default_use_kirchhoff = true;
  /** @brief Default setting for single-bus modeling */
  static constexpr Bool default_use_single_bus = false;
  /** @brief Default threshold for Kirchhoff constraints */
  static constexpr Real default_kirchhoff_threshold = 0;
  /** @brief Default objective function scaling factor */
  static constexpr Real default_scale_objective = 1'000;
  /** @brief Default voltage angle scaling factor (10 * 100 * 100 = 100,000) */
  static constexpr Real default_scale_theta = 10'000;

  // Default values for output settings
  /** @brief Default output directory path */
  static constexpr auto default_output_directory = "output";
  /** @brief Default output file format */
  static constexpr auto default_output_format = "parquet";
  /** @brief Default compression format for output files */
  static constexpr auto default_compression_format = "gzip";
  /** @brief Default setting for using LP variable/constraint names */
  static constexpr Bool default_use_lp_names = true;
  /** @brief Default setting for using UIDs in filenames */
  static constexpr Bool default_use_uid_fname = true;
  /** @brief Default annual discount rate for multi-year planning */
  static constexpr Real default_annual_discount_rate = 0.0;

  /**
   * @brief Constructs an OptionsLP wrapper around an Options object
   * @param poptions The Options object to wrap (defaults to empty Options)
   */
  explicit OptionsLP(Options poptions = {})
      : m_options_(std::move(poptions))
  {
  }

  /**
   * @brief Gets the input directory path, using default if not set
   * @return The input directory path
   */
  [[nodiscard]] constexpr auto input_directory() const
  {
    return m_options_.input_directory.value_or(default_input_directory);
  }

  /**
   * @brief Gets the input file format, using default if not set
   * @return The input file format
   */
  [[nodiscard]] constexpr auto input_format() const
  {
    return m_options_.input_format.value_or(default_input_format);
  }

  /**
   * @brief Gets the demand failure cost
   * @return The demand failure cost as an optional value
   */
  [[nodiscard]] constexpr auto demand_fail_cost() const
  {
    return m_options_.demand_fail_cost;
  }

  /**
   * @brief Gets the reserve failure cost
   * @return The reserve failure cost as an optional value
   */
  [[nodiscard]] constexpr auto reserve_fail_cost() const
  {
    return m_options_.reserve_fail_cost;
  }

  /**
   * @brief Gets the line loss modeling flag, using default if not set
   * @return Whether to model line losses
   */
  [[nodiscard]] constexpr auto use_line_losses() const
  {
    return m_options_.use_line_losses.value_or(default_use_line_losses);
  }

  /**
   * @brief Gets the Kirchhoff constraints flag, using default if not set
   * @return Whether to apply Kirchhoff constraints
   */
  [[nodiscard]] constexpr auto use_kirchhoff() const
  {
    return m_options_.use_kirchhoff.value_or(default_use_kirchhoff);
  }

  /**
   * @brief Gets the single-bus modeling flag, using default if not set
   * @return Whether to model the system as a single bus
   */
  [[nodiscard]] constexpr auto use_single_bus() const
  {
    return m_options_.use_single_bus.value_or(default_use_single_bus);
  }

  /**
   * @brief Gets the objective function scaling factor, using default if not set
   * @return The objective function scaling factor
   */
  [[nodiscard]] constexpr auto scale_objective() const
  {
    return m_options_.scale_objective.value_or(default_scale_objective);
  }

  /**
   * @brief Gets the Kirchhoff threshold, using default if not set
   * @return The threshold for applying Kirchhoff constraints
   */
  [[nodiscard]] constexpr auto kirchhoff_threshold() const
  {
    return m_options_.kirchhoff_threshold.value_or(default_kirchhoff_threshold);
  }

  /**
   * @brief Gets the voltage angle scaling factor, using default if not set
   * @return The voltage angle scaling factor
   */
  [[nodiscard]] constexpr auto scale_theta() const
  {
    return m_options_.scale_theta.value_or(default_scale_theta);
  }

  /**
   * @brief Gets the LP names usage flag, using default if not set
   * @return Whether to use descriptive names in the LP model
   */
  [[nodiscard]] constexpr auto use_lp_names() const
  {
    return m_options_.use_lp_names.value_or(default_use_lp_names);
  }

  /**
   * @brief Gets the UID filename usage flag, using default if not set
   * @return Whether to use UIDs in filenames
   */
  [[nodiscard]] constexpr auto use_uid_fname() const
  {
    return m_options_.use_uid_fname.value_or(default_use_uid_fname);
  }

  /**
   * @brief Gets the output directory path, using default if not set
   * @return The output directory path
   */
  [[nodiscard]] constexpr auto output_directory() const
  {
    return m_options_.output_directory.value_or(default_output_directory);
  }

  /**
   * @brief Gets the output file format, using default if not set
   * @return The output file format
   */
  [[nodiscard]] constexpr auto output_format() const
  {
    return m_options_.output_format.value_or(default_output_format);
  }

  /**
   * @brief Gets the compression format, using default if not set
   * @return The compression format for output files
   */
  [[nodiscard]] constexpr auto compression_format() const
  {
    return m_options_.compression_format.value_or(default_compression_format);
  }

  /**
   * @brief Gets the annual discount rate, using default if not set
   * @return The annual discount rate for multi-year planning
   */
  [[nodiscard]] constexpr auto annual_discount_rate() const
  {
    return m_options_.annual_discount_rate.value_or(
        default_annual_discount_rate);
  }

private:
  /** @brief The wrapped Options object */
  Options m_options_;
};

}  // namespace gtopt
