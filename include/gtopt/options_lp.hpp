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

#include <filesystem>
#include <string>

#include <gtopt/enum_option.hpp>
#include <gtopt/options.hpp>
#include <gtopt/variable_scale.hpp>

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
  static constexpr auto default_output_compression = "zstd";
  /** @brief Default LP naming level (1 = names + warn on duplicates) */
  static constexpr Int default_use_lp_names = 1;
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
      , m_variable_scale_map_(m_options_.variable_scales)
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
   * @brief Gets the LP naming level, using default if not set
   * @return LP naming level: 0=none, 1=names+warn-on-dup,
   *         2=names+error-on-dup
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
   * @brief Gets the LP solver options sub-object.
   *
   * Returns the @c SolverOptions sub-object embedded in the planning JSON
   * @c options block.  The @c SolverOptions fields are the primary way to
   * configure the LP solver; the top-level @c lp_algorithm, @c lp_threads,
   * @c lp_presolve fields in @c Options are deprecated aliases.
   *
   * @return Const reference to the @c SolverOptions from the wrapped Options
   */
  [[nodiscard]] constexpr const SolverOptions& solver_options() const noexcept
  {
    return m_options_.solver_options;
  }

  /**
   * @brief Gets the LP debug flag, using default if not set
   * @return Whether to save debug LP files to the log directory
   */
  [[nodiscard]] constexpr auto lp_debug() const
  {
    return m_options_.lp_debug.value_or(false);
  }

  /**
   * @brief Gets the LP compression codec for debug LP files.
   *
   * Returns the value of `lp_compression` when set.  An empty string (the
   * default) means "inherit from output_compression" — the caller (e.g.
   * planning_solver.cpp) falls back to `output_compression()` when this is
   * empty.  `"none"` or `"uncompressed"` disables LP compression regardless
   * of `output_compression`.  Any other value is a codec name passed as
   * `--codec <value>` to `gtopt_compress_lp`.
   *
   * @return Compression codec string (may be empty = inherit)
   */
  [[nodiscard]] constexpr auto lp_compression() const
  {
    static constexpr auto default_lp_compression = "";
    return m_options_.lp_compression.value_or(default_lp_compression);
  }

  /**
   * @brief Gets the just_build_lp flag, using default if not set.
   *
   * When true, the solver builds all scene×phase LP matrices but skips
   * solving entirely.  Applies uniformly to both the monolithic solver and
   * the SDDP solver: exit right after LP assembly with no solve at all.
   * Combine with lp_debug=true to save every scene/phase LP file to disk.
   *
   * @return Whether to stop after LP building
   */
  [[nodiscard]] constexpr auto just_build_lp() const
  {
    return m_options_.just_build_lp.value_or(false);
  }

  /**
   * @brief Gets the LP coefficient ratio threshold for conditioning
   * diagnostics.
   * @return The threshold above which per-scene/phase LP stats are shown
   *         (default 1e7).
   */
  [[nodiscard]] constexpr auto lp_coeff_ratio_threshold() const
  {
    static constexpr double default_lp_coeff_ratio_threshold = 1e7;
    return m_options_.lp_coeff_ratio_threshold.value_or(
        default_lp_coeff_ratio_threshold);
  }

  /**
   * @brief Gets the global solve timeout in seconds (0 = no timeout).
   *
   * Applies to both monolithic and SDDP forward-pass LP solves.
   * When non-zero, LP solves exceeding this time trigger a CRITICAL log
   * and an error return.
   *
   * @return SDDP solve timeout (default 180s = 3 min)
   */
  [[nodiscard]] constexpr auto sddp_solve_timeout() const
  {
    return m_options_.sddp_options.solve_timeout.value_or(180.0);
  }

  /** @brief Aperture LP timeout in seconds.
   * @return Aperture timeout (default 15s)
   */
  [[nodiscard]] constexpr auto sddp_aperture_timeout() const
  {
    return m_options_.sddp_options.aperture_timeout.value_or(15.0);
  }

  /** @brief Monolithic LP solve timeout in seconds.
   * @return Monolithic solve timeout (default 18000s = 300 min)
   */
  [[nodiscard]] constexpr auto monolithic_solve_timeout() const
  {
    return m_options_.monolithic_options.solve_timeout.value_or(18000.0);
  }

  // ── Monolithic solver accessors ─────────────────────────────────────────

  /// Monolithic solve mode: "monolithic" (default) or "sequential".
  [[nodiscard]] auto monolithic_solve_mode() const -> Name
  {
    return m_options_.monolithic_options.solve_mode.value_or(
        Name {"monolithic"});
  }

  /// CSV file with boundary cuts for the monolithic solver (empty = none).
  [[nodiscard]] auto monolithic_boundary_cuts_file() const -> Name
  {
    return m_options_.monolithic_options.boundary_cuts_file.value_or(Name {});
  }

  /// Boundary cuts load mode: "noload", "separated" (default), "combined".
  [[nodiscard]] auto monolithic_boundary_cuts_mode() const -> Name
  {
    return m_options_.monolithic_options.boundary_cuts_mode.value_or(
        Name {"separated"});
  }

  /// Maximum boundary cut iterations to load (0 = all).
  [[nodiscard]] auto monolithic_boundary_max_iterations() const -> int
  {
    return m_options_.monolithic_options.boundary_max_iterations.value_or(0);
  }

  // Default values for SDDP solver settings
  /** @brief Default solver type */
  static constexpr auto default_sddp_solver_type = "monolithic";
  /** @brief Default cut sharing mode for SDDP */
  static constexpr auto default_sddp_cut_sharing_mode = "none";
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
  /** @brief Default minimum iterations before declaring convergence */
  static constexpr Int default_sddp_min_iterations = 2;
  /** @brief Default relative convergence tolerance */
  static constexpr Real default_sddp_convergence_tol = 1e-4;
  /** @brief Default elastic slack penalty */
  static constexpr Real default_sddp_elastic_penalty = 1e6;
  /** @brief Default lower bound for future cost variable α */
  static constexpr Real default_sddp_alpha_min = 0.0;
  /** @brief Default upper bound for future cost variable α */
  static constexpr Real default_sddp_alpha_max = 1e12;
  /** @brief Default elastic filter mode */
  static constexpr auto default_sddp_elastic_mode = "single_cut";
  /** @brief Default multi_cut threshold (auto-switch after this many
   *         consecutive forward-pass infeasibilities at a phase) */
  static constexpr int default_sddp_multi_cut_threshold = 10;

  /**
   * @brief Gets the solver type, using default if not set.
   *
   * Reads the top-level `solver_type` field (`"monolithic"` or `"sddp"`).
   * Defaults to `"monolithic"` when not set.
   *
   * @return The solver type ("monolithic" or "sddp")
   */
  [[nodiscard]] auto solver_type() const -> Name
  {
    return m_options_.solver_type.value_or(Name {default_sddp_solver_type});
  }

  /**
   * @brief Gets the SDDP cut sharing mode, using default if not set
   * @return The cut sharing mode ("none", "expected", "accumulate", or "max")
   */
  [[nodiscard]] constexpr auto sddp_cut_sharing_mode() const
  {
    return m_options_.sddp_options.cut_sharing_mode.value_or(
        default_sddp_cut_sharing_mode);
  }

  /**
   * @brief Gets the cut directory for SDDP cut files, using default if not set
   * @return The cut directory path
   */
  [[nodiscard]] constexpr auto sddp_cut_directory() const
  {
    return m_options_.sddp_options.cut_directory.value_or(
        default_sddp_cut_directory);
  }

  /**
   * @brief Gets the log directory for log/trace files.
   *
   * When `log_directory` is explicitly set in the JSON / CLI, that value is
   * used as-is.  Otherwise the default is `output_directory + "/logs"` so
   * that all solver output (results, cuts, logs) is consolidated under a
   * single root directory.
   *
   * @return The log directory path (global — used by both monolithic and SDDP)
   */
  [[nodiscard]] auto log_directory() const -> std::string
  {
    if (m_options_.log_directory.has_value()) {
      return m_options_.log_directory.value();
    }
    return (std::filesystem::path(output_directory()) / "logs").string();
  }

  /**
   * @brief Gets the SDDP monitoring API enabled flag, using default if not set
   * @return Whether the SDDP monitoring API is enabled (default: true)
   */
  [[nodiscard]] constexpr auto sddp_api_enabled() const
  {
    return m_options_.sddp_options.api_enabled.value_or(
        default_sddp_api_enabled);
  }

  /**
   * @brief Gets the global efficiency update skip count
   * @return Number of SDDP iterations to skip between efficiency updates
   */
  [[nodiscard]] constexpr auto sddp_efficiency_update_skip() const
  {
    return m_options_.sddp_options.efficiency_update_skip.value_or(
        default_sddp_efficiency_update_skip);
  }

  /**
   * @brief Gets the maximum SDDP iterations
   * @return Maximum number of forward/backward iterations (default: 100)
   */
  [[nodiscard]] constexpr auto sddp_max_iterations() const
  {
    return m_options_.sddp_options.max_iterations.value_or(
        default_sddp_max_iterations);
  }

  /**
   * @brief Gets the minimum SDDP iterations before convergence
   * @return Minimum iterations before convergence (default: 2)
   */
  [[nodiscard]] constexpr auto sddp_min_iterations() const
  {
    return m_options_.sddp_options.min_iterations.value_or(
        default_sddp_min_iterations);
  }

  /**
   * @brief Gets the SDDP convergence tolerance
   * @return Relative gap tolerance for convergence (default: 1e-4)
   */
  [[nodiscard]] constexpr auto sddp_convergence_tol() const
  {
    return m_options_.sddp_options.convergence_tol.value_or(
        default_sddp_convergence_tol);
  }

  /**
   * @brief Gets the elastic slack penalty
   * @return Penalty for elastic slack variables (default: 1e6)
   */
  [[nodiscard]] constexpr auto sddp_elastic_penalty() const
  {
    return m_options_.sddp_options.elastic_penalty.value_or(
        default_sddp_elastic_penalty);
  }

  /**
   * @brief Gets the lower bound for future cost variable α
   * @return α lower bound in $ (default: 0.0)
   */
  [[nodiscard]] constexpr auto sddp_alpha_min() const
  {
    return m_options_.sddp_options.alpha_min.value_or(default_sddp_alpha_min);
  }

  /**
   * @brief Gets the upper bound for future cost variable α
   * @return α upper bound in $ (default: 1e12)
   */
  [[nodiscard]] constexpr auto sddp_alpha_max() const
  {
    return m_options_.sddp_options.alpha_max.value_or(default_sddp_alpha_max);
  }

  /**
   * @brief Whether to hot-start from previously saved cuts (default: false)
   */
  [[nodiscard]] constexpr auto sddp_hot_start() const
  {
    return m_options_.sddp_options.hot_start.value_or(false);
  }

  /**
   * @brief Whether to save cuts after each iteration (default: true)
   */
  [[nodiscard]] constexpr auto sddp_save_per_iteration() const
  {
    return m_options_.sddp_options.save_per_iteration.value_or(true);
  }

  /**
   * @brief Gets the input cut file for SDDP hot-start
   * @return Cut file path or empty string for cold start
   */
  [[nodiscard]] auto sddp_cuts_input_file() const -> Name
  {
    return m_options_.sddp_options.cuts_input_file.value_or("");
  }

  /**
   * @brief Gets the sentinel file path for graceful SDDP stop
   * @return Sentinel file path or empty string (no sentinel)
   */
  [[nodiscard]] auto sddp_sentinel_file() const -> Name
  {
    return m_options_.sddp_options.sentinel_file.value_or("");
  }

  /**
   * @brief Gets the elastic filter mode string
   * @return "single_cut" (default), "multi_cut", or "backpropagate"
   */
  [[nodiscard]] constexpr auto sddp_elastic_mode() const
  {
    return m_options_.sddp_options.elastic_mode.value_or(
        default_sddp_elastic_mode);
  }

  /**
   * @brief Gets the multi_cut threshold
   * @return Forward-pass infeasibility count before auto-switching to
   *         multi_cut (default: 10; 0 = never auto-switch)
   */
  [[nodiscard]] constexpr auto sddp_multi_cut_threshold() const
  {
    return m_options_.sddp_options.multi_cut_threshold.value_or(
        default_sddp_multi_cut_threshold);
  }

  [[nodiscard]] constexpr auto sddp_num_apertures() const
  {
    return m_options_.sddp_options.num_apertures.value_or(0);
  }

  /// Directory for aperture-specific scenario data (empty = use
  /// input_directory)
  [[nodiscard]] auto sddp_aperture_directory() const -> Name
  {
    return m_options_.sddp_options.aperture_directory.value_or(Name {});
  }

  /// CSV file with boundary (future-cost) cuts for the last phase.
  /// Empty = no boundary cuts.
  [[nodiscard]] auto sddp_boundary_cuts_file() const -> Name
  {
    return m_options_.sddp_options.boundary_cuts_file.value_or(Name {});
  }

  /// Boundary cuts load mode: "noload", "separated" (default), or "combined".
  [[nodiscard]] auto sddp_boundary_cuts_mode() const -> Name
  {
    return m_options_.sddp_options.boundary_cuts_mode.value_or(
        Name {"separated"});
  }

  /// Maximum boundary cut iterations to load (0 = all).
  [[nodiscard]] auto sddp_boundary_max_iterations() const -> int
  {
    return m_options_.sddp_options.boundary_max_iterations.value_or(0);
  }

  /// CSV file with named-variable cuts for hot-start across all phases.
  [[nodiscard]] auto sddp_named_cuts_file() const -> Name
  {
    return m_options_.sddp_options.named_cuts_file.value_or(Name {});
  }

  // ── Enum-typed accessors ──────────────────────────────────────────────────
  // These return proper enum types, converting from the underlying OptName
  // string fields.  Use these in preference to the string-returning
  // accessors above for type-safe comparisons.

  /// Solver type as an enum (SolverType::monolithic or SolverType::sddp).
  [[nodiscard]] auto solver_type_enum() const -> SolverType
  {
    return solver_type_from_name(
               m_options_.solver_type.value_or(default_sddp_solver_type))
        .value_or(SolverType::monolithic);
  }

  /// Input data format as an enum (DataFormat::parquet or DataFormat::csv).
  [[nodiscard]] constexpr auto input_format_enum() const -> DataFormat
  {
    return data_format_from_name(
               m_options_.input_format.value_or(default_input_format))
        .value_or(DataFormat::parquet);
  }

  /// Output data format as an enum (DataFormat::parquet or DataFormat::csv).
  [[nodiscard]] constexpr auto output_format_enum() const -> DataFormat
  {
    return data_format_from_name(
               m_options_.output_format.value_or(default_output_format))
        .value_or(DataFormat::parquet);
  }

  /// Output compression codec as an enum.
  [[nodiscard]] constexpr auto output_compression_enum() const
      -> CompressionCodec
  {
    return compression_codec_from_name(m_options_.output_compression.value_or(
                                           default_output_compression))
        .value_or(CompressionCodec::zstd);
  }

  /// SDDP cut sharing mode as an enum.
  [[nodiscard]] constexpr auto sddp_cut_sharing_mode_enum() const
      -> CutSharingMode
  {
    return cut_sharing_mode_from_name(
               m_options_.sddp_options.cut_sharing_mode.value_or(
                   default_sddp_cut_sharing_mode))
        .value_or(CutSharingMode::none);
  }

  /// SDDP elastic filter mode as an enum.
  [[nodiscard]] constexpr auto sddp_elastic_mode_enum() const
      -> ElasticFilterMode
  {
    return elastic_filter_mode_from_name(
               m_options_.sddp_options.elastic_mode.value_or(
                   default_sddp_elastic_mode))
        .value_or(ElasticFilterMode::single_cut);
  }

  /// SDDP boundary cuts mode as an enum.
  [[nodiscard]] auto sddp_boundary_cuts_mode_enum() const -> BoundaryCutsMode
  {
    return boundary_cuts_mode_from_name(
               m_options_.sddp_options.boundary_cuts_mode.value_or("separated"))
        .value_or(BoundaryCutsMode::separated);
  }

  /// Monolithic solve mode as an enum.
  [[nodiscard]] auto monolithic_solve_mode_enum() const -> SolveMode
  {
    return solve_mode_from_name(
               m_options_.monolithic_options.solve_mode.value_or("monolithic"))
        .value_or(SolveMode::monolithic);
  }

  /// Monolithic boundary cuts mode as an enum.
  [[nodiscard]] auto monolithic_boundary_cuts_mode_enum() const
      -> BoundaryCutsMode
  {
    return boundary_cuts_mode_from_name(
               m_options_.monolithic_options.boundary_cuts_mode.value_or(
                   "separated"))
        .value_or(BoundaryCutsMode::separated);
  }

  /**
   * @brief Gets the variable scale map built from variable_scales entries.
   *
   * The map provides `lookup(class_name, variable, uid)` to resolve scale
   * factors with per-element > per-class > default (1.0) priority.
   *
   * Note: per-element fields (`Battery::energy_scale`, `Reservoir::vol_scale`)
   * and global options (`scale_theta`) take precedence over this map.
   * Use this for variables not covered by dedicated fields.
   */
  [[nodiscard]] const auto& variable_scale_map() const noexcept
  {
    return m_variable_scale_map_;
  }

private:
  /** @brief The wrapped Options object */
  Options m_options_;
  /** @brief Variable scale map built from Options::variable_scales */
  VariableScaleMap m_variable_scale_map_;
};

}  // namespace gtopt
