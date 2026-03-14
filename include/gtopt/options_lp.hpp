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
  /** @brief Default number of flow segments per line (1 = linear model,
   * &gt;1 = piecewise-linear quadratic approximation of P_loss = R·f²/V²) */
  static constexpr Int default_loss_segments = 1;
  /** @brief Default setting for Kirchhoff constraints */
  static constexpr Bool default_use_kirchhoff = true;
  /** @brief Default setting for single-bus modeling */
  static constexpr Bool default_use_single_bus = false;
  /** @brief Default threshold for Kirchhoff constraints */
  static constexpr Real default_kirchhoff_threshold = 0;
  /** @brief Default objective function scaling factor */
  static constexpr Real default_scale_objective = 1'000;
  /** @brief Default voltage angle scaling factor (10 * 100 * 100 = 100,000) */
  static constexpr Real default_scale_theta = 1'000;

  // Default values for output settings
  /** @brief Default output directory path */
  static constexpr auto default_output_directory = "output";
  /** @brief Default output file format */
  static constexpr auto default_output_format = "parquet";
  /** @brief Default compression codec for output files */
  static constexpr auto default_output_compression = "gzip";
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
  explicit constexpr OptionsLP(Options poptions = {})
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
   * @brief Gets the default number of piecewise-linear loss segments
   * @return Number of segments (1 = linear model, >1 = quadratic
   * approximation)
   */
  [[nodiscard]] constexpr auto loss_segments() const
  {
    return m_options_.loss_segments.value_or(default_loss_segments);
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
   * @brief Gets the output compression codec, using default if not set
   * @return The compression codec for output files
   */
  [[nodiscard]] constexpr auto output_compression() const
  {
    return m_options_.output_compression.value_or(default_output_compression);
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

  /**
   * @brief Gets the LP algorithm option (raw optional, no default applied)
   * @return The LP algorithm index as an optional int
   */
  [[nodiscard]] constexpr auto lp_algorithm() const
  {
    return m_options_.lp_algorithm;
  }

  /**
   * @brief Gets the LP solver threads option (raw optional, no default
   * applied)
   * @return The number of solver threads as an optional int
   */
  [[nodiscard]] constexpr auto lp_threads() const
  {
    return m_options_.lp_threads;
  }

  /**
   * @brief Gets the LP presolve option (raw optional, no default applied)
   * @return Whether to use presolve as an optional bool
   */
  [[nodiscard]] constexpr auto lp_presolve() const
  {
    return m_options_.lp_presolve;
  }

  /**
   * @brief Gets the LP debug flag, using default if not set
   * @return Whether to save debug LP files to the log directory
   */
  [[nodiscard]] constexpr auto lp_debug() const
  {
    return m_options_.lp_debug.value_or(false);
  }

  // Default values for SDDP solver settings
  /** @brief Default solver type */
  static constexpr auto default_sddp_solver_type = "monolithic";
  /** @brief Default cut sharing mode for SDDP */
  static constexpr auto default_sddp_cut_sharing_mode = "max";
  /** @brief Default directory for Benders cut files */
  static constexpr auto default_sddp_cut_directory = "cuts";
  /** @brief Default directory for log/trace files */
  static constexpr auto default_log_directory = "logs";
  /** @brief Default for SDDP monitoring API (enabled by default) */
  static constexpr Bool default_sddp_api_enabled = true;
  /** @brief Default iterations to skip between efficiency updates (0 = every
   * iteration, matching PLP behaviour) */
  static constexpr Int default_sddp_efficiency_update_skip = 0;
  /** @brief Default maximum SDDP iterations */
  static constexpr Int default_sddp_max_iterations = 100;
  /** @brief Default relative convergence tolerance */
  static constexpr Real default_sddp_convergence_tol = 1e-4;
  /** @brief Default elastic slack penalty */
  static constexpr Real default_sddp_elastic_penalty = 1e6;
  /** @brief Default lower bound for future cost variable α */
  static constexpr Real default_sddp_alpha_min = 0.0;
  /** @brief Default upper bound for future cost variable α */
  static constexpr Real default_sddp_alpha_max = 1e12;
  /** @brief Default elastic filter mode */
  static constexpr auto default_sddp_elastic_mode = "single-cut";
  /** @brief Default multi-cut threshold (auto-switch after this many
   *         consecutive forward-pass infeasibilities at a phase) */
  static constexpr int default_sddp_multi_cut_threshold = 10;

  /**
   * @brief Gets the solver type, using default if not set
   * @return The solver type ("monolithic" or "sddp")
   */
  [[nodiscard]] constexpr auto sddp_solver_type() const
  {
    return m_options_.sddp_options.sddp_solver_type.value_or(
        default_sddp_solver_type);
  }

  /**
   * @brief Gets the SDDP cut sharing mode, using default if not set
   * @return The cut sharing mode ("none", "expected", "accumulate", or "max")
   */
  [[nodiscard]] constexpr auto sddp_cut_sharing_mode() const
  {
    return m_options_.sddp_options.sddp_cut_sharing_mode.value_or(
        default_sddp_cut_sharing_mode);
  }

  /**
   * @brief Gets the cut directory for SDDP cut files, using default if not set
   * @return The cut directory path
   */
  [[nodiscard]] constexpr auto sddp_cut_directory() const
  {
    return m_options_.sddp_options.sddp_cut_directory.value_or(
        default_sddp_cut_directory);
  }

  /**
   * @brief Gets the log directory for log/trace files, using default if not set
   * @return The log directory path (global — used by both monolithic and SDDP)
   */
  [[nodiscard]] constexpr auto log_directory() const
  {
    return m_options_.log_directory.value_or(default_log_directory);
  }

  /**
   * @brief Gets the SDDP monitoring API enabled flag, using default if not set
   * @return Whether the SDDP monitoring API is enabled (default: true)
   */
  [[nodiscard]] constexpr auto sddp_api_enabled() const
  {
    return m_options_.sddp_options.sddp_api_enabled.value_or(
        default_sddp_api_enabled);
  }

  /**
   * @brief Gets the global efficiency update skip count
   * @return Number of SDDP iterations to skip between efficiency updates
   */
  [[nodiscard]] constexpr auto sddp_efficiency_update_skip() const
  {
    return m_options_.sddp_options.sddp_efficiency_update_skip.value_or(
        default_sddp_efficiency_update_skip);
  }

  /**
   * @brief Gets the maximum SDDP iterations
   * @return Maximum number of forward/backward iterations (default: 100)
   */
  [[nodiscard]] constexpr auto sddp_max_iterations() const
  {
    return m_options_.sddp_options.sddp_max_iterations.value_or(
        default_sddp_max_iterations);
  }

  /**
   * @brief Gets the SDDP convergence tolerance
   * @return Relative gap tolerance for convergence (default: 1e-4)
   */
  [[nodiscard]] constexpr auto sddp_convergence_tol() const
  {
    return m_options_.sddp_options.sddp_convergence_tol.value_or(
        default_sddp_convergence_tol);
  }

  /**
   * @brief Gets the elastic slack penalty
   * @return Penalty for elastic slack variables (default: 1e6)
   */
  [[nodiscard]] constexpr auto sddp_elastic_penalty() const
  {
    return m_options_.sddp_options.sddp_elastic_penalty.value_or(
        default_sddp_elastic_penalty);
  }

  /**
   * @brief Gets the lower bound for future cost variable α
   * @return α lower bound in $ (default: 0.0)
   */
  [[nodiscard]] constexpr auto sddp_alpha_min() const
  {
    return m_options_.sddp_options.sddp_alpha_min.value_or(
        default_sddp_alpha_min);
  }

  /**
   * @brief Gets the upper bound for future cost variable α
   * @return α upper bound in $ (default: 1e12)
   */
  [[nodiscard]] constexpr auto sddp_alpha_max() const
  {
    return m_options_.sddp_options.sddp_alpha_max.value_or(
        default_sddp_alpha_max);
  }

  /**
   * @brief Gets the input cut file for SDDP hot-start
   * @return Cut file path or empty string for cold start
   */
  [[nodiscard]] auto sddp_cuts_input_file() const -> Name
  {
    return m_options_.sddp_options.sddp_cuts_input_file.value_or("");
  }

  /**
   * @brief Gets the sentinel file path for graceful SDDP stop
   * @return Sentinel file path or empty string (no sentinel)
   */
  [[nodiscard]] auto sddp_sentinel_file() const -> Name
  {
    return m_options_.sddp_options.sddp_sentinel_file.value_or("");
  }

  /**
   * @brief Gets the elastic filter mode string
   * @return "single-cut" (default), "multi-cut", or "backpropagate"
   */
  [[nodiscard]] constexpr auto sddp_elastic_mode() const
  {
    return m_options_.sddp_options.sddp_elastic_mode.value_or(
        default_sddp_elastic_mode);
  }

  /**
   * @brief Gets the multi-cut threshold
   * @return Forward-pass infeasibility count before auto-switching to
   *         multi-cut (default: 10; 0 = never auto-switch)
   */
  [[nodiscard]] constexpr auto sddp_multi_cut_threshold() const
  {
    return m_options_.sddp_options.sddp_multi_cut_threshold.value_or(
        default_sddp_multi_cut_threshold);
  }

  [[nodiscard]] constexpr auto sddp_num_apertures() const
  {
    return m_options_.sddp_options.sddp_num_apertures.value_or(0);
  }

  /// Directory for aperture-specific scenario data (empty = use
  /// input_directory)
  [[nodiscard]] auto sddp_aperture_directory() const -> Name
  {
    return m_options_.sddp_options.sddp_aperture_directory.value_or(Name {});
  }

  /// CSV file with boundary (future-cost) cuts for the last phase.
  /// Empty = no boundary cuts.
  [[nodiscard]] auto sddp_boundary_cuts_file() const -> Name
  {
    return m_options_.sddp_options.sddp_boundary_cuts_file.value_or(Name {});
  }

  /// Boundary cuts load mode: "noload", "separated" (default), or "combined".
  [[nodiscard]] auto sddp_boundary_cuts_mode() const -> Name
  {
    return m_options_.sddp_options.sddp_boundary_cuts_mode.value_or(
        Name {"separated"});
  }

  /// Maximum boundary cut iterations to load (0 = all).
  [[nodiscard]] auto sddp_boundary_max_iterations() const -> int
  {
    return m_options_.sddp_options.sddp_boundary_max_iterations.value_or(0);
  }

private:
  /** @brief The wrapped Options object */
  Options m_options_;
};

}  // namespace gtopt
