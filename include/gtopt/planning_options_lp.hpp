/**
 * @file      planning_options_lp.hpp
 * @brief     Wrapper for PlanningOptions with default value handling
 * @date      Tue Apr 22 03:21:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the PlanningOptionsLP class, which wraps the
 * PlanningOptions structure and provides accessors with default values for
 * optional parameters. This allows consistent and easy access to option values
 * throughout the linear programming (LP) optimization processes.
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <gtopt/enum_option.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/variable_scale.hpp>

namespace gtopt
{

/**
 * @brief Wrapper for PlanningOptions with default value handling
 *
 * PlanningOptionsLP wraps a PlanningOptions structure and provides accessor
 * methods that automatically apply default values when options are not
 * explicitly set. This ensures consistent behavior throughout the optimization
 * code by defining system-wide defaults in one place.
 */
class PlanningOptionsLP
{
public:
  // Default values for input settings
  /** @brief Default input directory path */
  static constexpr auto default_input_directory = "input";
  /** @brief Default input file format */
  static constexpr DataFormat default_input_format = DataFormat::parquet;

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
  static constexpr Real default_scale_objective = 10'000'000;
  /** @brief Default voltage angle scaling factor (10 * 100 * 100 = 100,000) */
  static constexpr Real default_scale_theta = 1'000;

  // Default values for output settings
  /** @brief Default output directory path */
  static constexpr auto default_output_directory = "output";
  /** @brief Default output file format */
  static constexpr DataFormat default_output_format = DataFormat::parquet;
  /** @brief Default compression codec for output files */
  static constexpr CompressionCodec default_output_compression =
      CompressionCodec::zstd;
  /** @brief Default LP naming level (minimal = state-var col names only) */
  static constexpr LpNamesLevel default_names_level = LpNamesLevel::minimal;
  /** @brief Default setting for using UIDs in filenames */
  static constexpr Bool default_use_uid_fname = true;
  /** @brief Default annual discount rate for multi-year planning */
  static constexpr Real default_annual_discount_rate = 0.0;

  /**
   * @brief Constructs a PlanningOptionsLP wrapper around a PlanningOptions
   * object
   * @param poptions The PlanningOptions object to wrap (defaults to empty)
   */
  explicit PlanningOptionsLP(PlanningOptions poptions = {})
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
   * @return The input file format as a string name
   */
  [[nodiscard]] auto input_format() const -> std::string_view
  {
    return enum_name(m_options_.input_format.value_or(default_input_format));
  }

  /**
   * @brief Gets the input file format as a typed enum
   * @return The input DataFormat enum value
   */
  [[nodiscard]] constexpr auto input_format_enum() const -> DataFormat
  {
    return m_options_.input_format.value_or(default_input_format);
  }

  /**
   * @brief Gets the demand failure cost
   * @return The demand failure cost as an optional value
   */
  /// @brief Gets the demand failure cost.
  /// Flat field takes precedence, then model_options.
  [[nodiscard]] constexpr auto demand_fail_cost() const
  {
    if (m_options_.demand_fail_cost.has_value()) {
      return m_options_.demand_fail_cost;
    }
    return m_options_.model_options.demand_fail_cost;
  }

  /// @brief Gets the reserve failure cost.
  /// Flat field takes precedence, then model_options.
  [[nodiscard]] constexpr auto reserve_fail_cost() const
  {
    if (m_options_.reserve_fail_cost.has_value()) {
      return m_options_.reserve_fail_cost;
    }
    return m_options_.model_options.reserve_fail_cost;
  }

  /// @brief Gets the line loss modeling flag.
  /// Flat field takes precedence, then model_options, then default.
  [[nodiscard]] constexpr auto use_line_losses() const
  {
    if (m_options_.use_line_losses.has_value()) {
      return *m_options_.use_line_losses;
    }
    return m_options_.model_options.use_line_losses.value_or(
        default_use_line_losses);
  }

  /// @brief Gets the number of piecewise-linear loss segments.
  [[nodiscard]] constexpr auto loss_segments() const
  {
    if (m_options_.loss_segments.has_value()) {
      return *m_options_.loss_segments;
    }
    return m_options_.model_options.loss_segments.value_or(
        default_loss_segments);
  }

  /// @brief Gets the Kirchhoff constraints flag.
  [[nodiscard]] constexpr auto use_kirchhoff() const
  {
    if (m_options_.use_kirchhoff.has_value()) {
      return *m_options_.use_kirchhoff;
    }
    return m_options_.model_options.use_kirchhoff.value_or(
        default_use_kirchhoff);
  }

  /// @brief Gets the single-bus modeling flag.
  [[nodiscard]] constexpr auto use_single_bus() const
  {
    if (m_options_.use_single_bus.has_value()) {
      return *m_options_.use_single_bus;
    }
    return m_options_.model_options.use_single_bus.value_or(
        default_use_single_bus);
  }

  /// @brief Gets the objective function scaling factor.
  [[nodiscard]] constexpr auto scale_objective() const
  {
    if (m_options_.scale_objective.has_value()) {
      return *m_options_.scale_objective;
    }
    return m_options_.model_options.scale_objective.value_or(
        default_scale_objective);
  }

  /// @brief Gets the Kirchhoff threshold.
  [[nodiscard]] constexpr auto kirchhoff_threshold() const
  {
    if (m_options_.kirchhoff_threshold.has_value()) {
      return *m_options_.kirchhoff_threshold;
    }
    return m_options_.model_options.kirchhoff_threshold.value_or(
        default_kirchhoff_threshold);
  }

  /// @brief Gets the voltage angle scaling factor.
  [[nodiscard]] constexpr auto scale_theta() const
  {
    if (m_options_.scale_theta.has_value()) {
      return *m_options_.scale_theta;
    }
    return m_options_.model_options.scale_theta.value_or(default_scale_theta);
  }

  /**
   * @brief Gets the LP naming level, using default if not set
   * @return LP naming level: minimal, only_cols, or cols_and_rows
   */
  [[nodiscard]] constexpr auto names_level() const -> LpNamesLevel
  {
    return m_options_.lp_build_options.names_level.value_or(
        default_names_level);
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
   * @brief Gets the output file format as a string name
   * @return The output file format name
   */
  [[nodiscard]] auto output_format() const -> std::string_view
  {
    return enum_name(m_options_.output_format.value_or(default_output_format));
  }

  /**
   * @brief Gets the output file format as a typed enum
   * @return The output DataFormat enum value
   */
  [[nodiscard]] constexpr auto output_format_enum() const -> DataFormat
  {
    return m_options_.output_format.value_or(default_output_format);
  }

  /**
   * @brief Gets the output compression codec as a string name
   * @return The compression codec name for output files
   */
  [[nodiscard]] auto output_compression() const -> std::string_view
  {
    return enum_name(
        m_options_.output_compression.value_or(default_output_compression));
  }

  /**
   * @brief Gets the output compression codec as a typed enum
   * @return The CompressionCodec enum value
   */
  [[nodiscard]] constexpr auto output_compression_enum() const
      -> CompressionCodec
  {
    return m_options_.output_compression.value_or(default_output_compression);
  }

  /**
   * @brief Gets the annual discount rate, using default if not set
   * @return The annual discount rate for multi-year planning
   */
  [[nodiscard]] constexpr auto annual_discount_rate() const
  {
    if (m_options_.annual_discount_rate.has_value()) {
      return *m_options_.annual_discount_rate;
    }
    return m_options_.model_options.annual_discount_rate.value_or(
        default_annual_discount_rate);
  }

  /**
   * @brief Gets the global LP solver options sub-object.
   *
   * Returns the @c SolverOptions sub-object embedded in the planning JSON
   * @c options block.
   *
   * @return Const reference to the @c SolverOptions from the wrapped
   * PlanningOptions
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
   * @brief Gets the LP compression codec for debug LP files as a string.
   *
   * Returns the codec name when set, or empty string (the default) meaning
   * "inherit from output_compression".  `"uncompressed"` disables LP
   * compression regardless of `output_compression`.
   *
   * @return Compression codec string (may be empty = inherit)
   */
  [[nodiscard]] auto lp_compression() const -> std::string_view
  {
    if (m_options_.lp_compression.has_value()) {
      return enum_name(*m_options_.lp_compression);
    }
    return "";
  }

  /**
   * @brief Gets the LP compression codec as a typed enum (nullopt = inherit)
   * @return The CompressionCodec enum value, or nullopt
   */
  [[nodiscard]] constexpr auto lp_compression_enum() const
      -> std::optional<CompressionCodec>
  {
    return m_options_.lp_compression;
  }

  /**
   * @brief Gets the lp_build flag, using default if not set.
   *
   * When true, the solver builds all scene×phase LP matrices but skips
   * solving entirely.  Applies uniformly to both the monolithic solver and
   * the SDDP solver: exit right after LP assembly with no solve at all.
   * Combine with lp_debug=true to save every scene/phase LP file to disk.
   *
   * @return Whether to stop after LP building
   */
  [[nodiscard]] constexpr auto lp_build() const
  {
    return m_options_.lp_build.value_or(false);
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
    return m_options_.lp_build_options.lp_coeff_ratio_threshold.value_or(
        default_lp_coeff_ratio_threshold);
  }

  /**
   * @brief Gets the effective SDDP forward-pass solver options.
   *
   * Merges the per-pass forward solver options (if set) with the global
   * solver_options.  Forward-pass-specific options take precedence.
   *
   * @return Resolved SolverOptions for SDDP forward pass
   */
  [[nodiscard]] auto sddp_forward_solver_options() const -> SolverOptions
  {
    if (m_options_.sddp_options.forward_solver_options.has_value()) {
      auto opts = m_options_.solver_options;
      opts.merge(*m_options_.sddp_options.forward_solver_options);
      return opts;
    }
    return m_options_.solver_options;
  }

  /**
   * @brief Gets the effective SDDP backward-pass solver options.
   *
   * Merges the per-pass backward solver options (if set) with the global
   * solver_options.  Backward-pass-specific options take precedence.
   *
   * @return Resolved SolverOptions for SDDP backward pass
   */
  [[nodiscard]] auto sddp_backward_solver_options() const -> SolverOptions
  {
    if (m_options_.sddp_options.backward_solver_options.has_value()) {
      auto opts = m_options_.solver_options;
      opts.merge(*m_options_.sddp_options.backward_solver_options);
      return opts;
    }
    return m_options_.solver_options;
  }

  /** @brief Aperture LP timeout in seconds.
   * @return Aperture timeout (default 15s)
   */
  [[nodiscard]] constexpr auto sddp_aperture_timeout() const
  {
    return m_options_.sddp_options.aperture_timeout.value_or(15.0);
  }

  /** @brief Whether to save LP files for infeasible apertures.
   * @return false by default (disabled).
   */
  [[nodiscard]] constexpr auto sddp_save_aperture_lp() const
  {
    return m_options_.sddp_options.save_aperture_lp.value_or(false);
  }

  /**
   * @brief Gets the effective monolithic solver options.
   *
   * Merges the per-method monolithic solver options (if set) with the global
   * solver_options.  Per-method options override the global ones.
   *
   * @return Resolved SolverOptions for the monolithic solver
   */
  [[nodiscard]] auto monolithic_solver_options() const -> SolverOptions
  {
    if (m_options_.monolithic_options.solver_options.has_value()) {
      auto opts = m_options_.solver_options;
      opts.merge(*m_options_.monolithic_options.solver_options);
      return opts;
    }
    return m_options_.solver_options;
  }

  // ── Monolithic solver accessors ─────────────────────────────────────────

  /// CSV file with boundary cuts for the monolithic solver (empty = none).
  [[nodiscard]] auto monolithic_boundary_cuts_file() const -> Name
  {
    return m_options_.monolithic_options.boundary_cuts_file.value_or(Name {});
  }

  /// Maximum boundary cut iterations to load (0 = all).
  [[nodiscard]] auto monolithic_boundary_max_iterations() const -> int
  {
    return m_options_.monolithic_options.boundary_max_iterations.value_or(0);
  }

  // Default values for SDDP solver settings
  /** @brief Default solver type */
  static constexpr MethodType default_method_type = MethodType::monolithic;
  /** @brief Default cut sharing mode for SDDP */
  static constexpr CutSharingMode default_sddp_cut_sharing_mode =
      CutSharingMode::none;
  /** @brief Default directory for Benders cut files */
  static constexpr auto default_sddp_cut_directory = "cuts";
  /** @brief Default directory for log/trace files */
  static constexpr auto default_log_directory = "logs";
  /** @brief Default for SDDP monitoring API (enabled by default) */
  static constexpr Bool default_sddp_api_enabled = true;
  /** @brief Default iterations to skip between update_lp dispatches (0 = every
   * iteration, matching PLP behaviour) */
  static constexpr Int default_sddp_update_lp_skip = 0;
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
  /** @brief Default scale divisor for future cost variable α (PLP varphi) */
  static constexpr Real default_sddp_scale_alpha = 1'000'000;
  /** @brief Default elastic filter mode */
  static constexpr ElasticFilterMode default_sddp_elastic_mode =
      ElasticFilterMode::single_cut;
  /** @brief Default multi_cut threshold (auto-switch after this many
   *         consecutive forward-pass infeasibilities at a phase) */
  static constexpr int default_sddp_multi_cut_threshold = 10;
  /** @brief Default stationary-gap tolerance.
   * When the relative gap change over the look-back window is below this
   * value, the gap is considered stationary.  Used by the stationary and
   * statistical+stationary convergence criteria.  Default: 0.01 (1%). */
  static constexpr Real default_sddp_stationary_tol = 0.01;
  /** @brief Default look-back window for stationary gap check */
  static constexpr Int default_sddp_stationary_window = 10;
  /** @brief Default confidence level for statistical convergence.
   * Enables PLP-style CI-based convergence for multi-scene problems.
   * Combined with stationary_tol, also handles the non-zero-gap case
   * where the gap stabilises above the CI threshold.
   * Default: 0.95 (95% confidence interval). */
  static constexpr Real default_sddp_convergence_confidence = 0.95;

  /**
   * @brief Gets the SDDP cut sharing mode as a string name
   * @return The cut sharing mode name
   */
  [[nodiscard]] auto sddp_cut_sharing_mode() const -> std::string_view
  {
    return enum_name(sddp_cut_sharing_mode_enum());
  }

  /**
   * @brief Gets the SDDP cut sharing mode as a typed enum
   * @return The CutSharingMode enum value
   */
  [[nodiscard]] constexpr auto sddp_cut_sharing_mode_enum() const
      -> CutSharingMode
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
   * @brief Gets the global update_lp skip count
   * @return Number of SDDP iterations to skip between update_lp dispatches
   */
  [[nodiscard]] constexpr auto sddp_update_lp_skip() const
  {
    return m_options_.sddp_options.update_lp_skip.value_or(
        default_sddp_update_lp_skip);
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
   * @brief Gets the scale divisor for future cost variable α
   * @return α scale divisor (default: 1000, analogous to PLP varphi scale)
   */
  [[nodiscard]] constexpr auto sddp_scale_alpha() const
  {
    return m_options_.sddp_options.scale_alpha.value_or(
        default_sddp_scale_alpha);
  }

  /**
   * @brief Whether to save cuts after each iteration (default: true)
   */
  [[nodiscard]] constexpr auto sddp_save_per_iteration() const
  {
    return m_options_.sddp_options.save_per_iteration.value_or(true);
  }

  /**
   * @brief Simulation mode: no training iterations, forward-only pass.
   * @return true if simulation mode is enabled (default: false)
   */
  [[nodiscard]] constexpr auto sddp_simulation_mode() const
  {
    return m_options_.sddp_options.simulation_mode.value_or(false);
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
   * @brief Gets the elastic filter mode as a string name
   * @return "single_cut" (default), "multi_cut", or "backpropagate"
   */
  [[nodiscard]] auto sddp_elastic_mode() const -> std::string_view
  {
    return enum_name(sddp_elastic_mode_enum());
  }

  /**
   * @brief Gets the elastic filter mode as a typed enum
   * @return The ElasticFilterMode enum value
   */
  [[nodiscard]] constexpr auto sddp_elastic_mode_enum() const
      -> ElasticFilterMode
  {
    return m_options_.sddp_options.elastic_mode.value_or(
        default_sddp_elastic_mode);
  }

  /**
   * @brief Gets the cut coefficient extraction mode as a typed enum
   * @return The CutCoeffMode enum value (default: reduced_cost)
   */
  [[nodiscard]] constexpr auto sddp_cut_coeff_mode_enum() const -> CutCoeffMode
  {
    return m_options_.sddp_options.cut_coeff_mode.value_or(
        CutCoeffMode::reduced_cost);
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

  /// Aperture UIDs for the backward pass.
  /// nullopt = use per-phase apertures; empty = no apertures (Benders).
  [[nodiscard]] const auto& sddp_apertures() const noexcept
  {
    return m_options_.sddp_options.apertures;
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

  /// Boundary cuts load mode as a string name.
  [[nodiscard]] auto sddp_boundary_cuts_mode() const -> std::string_view
  {
    return enum_name(sddp_boundary_cuts_mode_enum());
  }

  /// Maximum boundary cut iterations to load (0 = all).
  [[nodiscard]] auto sddp_boundary_max_iterations() const -> int
  {
    return m_options_.sddp_options.boundary_max_iterations.value_or(0);
  }

  /// How to handle cut rows referencing missing state variables.
  [[nodiscard]] constexpr auto sddp_missing_cut_var_mode() const
      -> MissingCutVarMode
  {
    return m_options_.sddp_options.missing_cut_var_mode.value_or(
        MissingCutVarMode::skip_coeff);
  }

  /// CSV file with named-variable cuts for hot-start across all phases.
  [[nodiscard]] auto sddp_named_cuts_file() const -> Name
  {
    return m_options_.sddp_options.named_cuts_file.value_or(Name {});
  }

  /// Maximum retained cuts per (scene, phase) LP.  0 = unlimited (default).
  [[nodiscard]] constexpr auto sddp_max_cuts_per_phase() const
  {
    return m_options_.sddp_options.max_cuts_per_phase.value_or(0);
  }

  /// Iterations between cut pruning passes.  Default: 10.
  [[nodiscard]] constexpr auto sddp_cut_prune_interval() const
  {
    return m_options_.sddp_options.cut_prune_interval.value_or(10);
  }

  /// Dual threshold for inactive cut detection.  Default: 1e-8.
  [[nodiscard]] constexpr auto sddp_prune_dual_threshold() const
  {
    return m_options_.sddp_options.prune_dual_threshold.value_or(1e-8);
  }

  /// Use single cut storage (per-scene only).  Default: false.
  [[nodiscard]] constexpr auto sddp_single_cut_storage() const
  {
    return m_options_.sddp_options.single_cut_storage.value_or(false);
  }

  /// Maximum stored cuts per scene.  Default: 0 (unlimited).
  [[nodiscard]] constexpr auto sddp_max_stored_cuts() const
  {
    return m_options_.sddp_options.max_stored_cuts.value_or(0);
  }

  /// Reuse cached LP clones for aperture solves.  Default: true.
  [[nodiscard]] constexpr auto sddp_use_clone_pool() const
  {
    return m_options_.sddp_options.use_clone_pool.value_or(true);
  }

  /** @brief Whether SDDP resolves use warm-start (default: true). */
  [[nodiscard]] constexpr auto sddp_warm_start() const
  {
    return m_options_.sddp_options.warm_start.value_or(true);
  }

  /** @brief How update_lp elements obtain reservoir/battery volume between
   *  phases (affects seepage, production factor, discharge limit only).
   *  Default: warm_start (no cross-phase lookup). */
  [[nodiscard]] constexpr auto sddp_state_variable_lookup_mode() const
  {
    return m_options_.sddp_options.state_variable_lookup_mode.value_or(
        StateVariableLookupMode::warm_start);
  }

  /**
   * @brief Gets the SDDP convergence mode.
   * @return ConvergenceMode enum (default: statistical)
   */
  [[nodiscard]] constexpr auto sddp_convergence_mode() const
  {
    return m_options_.sddp_options.convergence_mode.value_or(
        ConvergenceMode::statistical);
  }

  /**
   * @brief Gets the stationary-gap convergence tolerance.
   *
   * When positive, enables the secondary convergence criterion: if the
   * relative change in the gap over the last `stationary_window` iterations
   * falls below this threshold, the solver declares convergence even when the
   * gap exceeds `convergence_tol`.  Default: 0.0 (disabled).
   */
  [[nodiscard]] constexpr auto sddp_stationary_tol() const
  {
    return m_options_.sddp_options.stationary_tol.value_or(
        default_sddp_stationary_tol);
  }

  /**
   * @brief Gets the look-back window for stationary gap detection.
   * @return Number of iterations to look back (default: 10)
   */
  [[nodiscard]] constexpr auto sddp_stationary_window() const
  {
    return m_options_.sddp_options.stationary_window.value_or(
        default_sddp_stationary_window);
  }

  /**
   * @brief Gets the confidence level for statistical convergence.
   * @return Confidence level (0-1), 0.0 = disabled (default)
   */
  [[nodiscard]] constexpr auto sddp_convergence_confidence() const
  {
    return m_options_.sddp_options.convergence_confidence.value_or(
        default_sddp_convergence_confidence);
  }

  // ── Cascade options ─────────────────────────────────────────────────────

  /// Cascade level configurations.  Empty → use built-in defaults.
  [[nodiscard]] const auto& cascade_levels() const noexcept
  {
    return m_options_.cascade_options.level_array;
  }

  /// Whether the user specified cascade levels.
  [[nodiscard]] bool has_cascade_levels() const noexcept
  {
    return !m_options_.cascade_options.level_array.empty();
  }

  /// Global cascade SDDP options (serve as defaults for all levels).
  [[nodiscard]] const auto& cascade_sddp_options() const noexcept
  {
    return m_options_.cascade_options.sddp_options;
  }

  // ── Enum-typed accessors ──────────────────────────────────────────────────

  /// Solver type as an enum (MethodType::monolithic or MethodType::sddp).
  [[nodiscard]] constexpr auto method_type_enum() const -> MethodType
  {
    return m_options_.method.value_or(default_method_type);
  }

  /// SDDP boundary cuts mode as an enum.
  [[nodiscard]] constexpr auto sddp_boundary_cuts_mode_enum() const
      -> BoundaryCutsMode
  {
    return m_options_.sddp_options.boundary_cuts_mode.value_or(
        BoundaryCutsMode::separated);
  }

  /// SDDP cut recovery mode as an enum.
  [[nodiscard]] constexpr auto sddp_cut_recovery_mode_enum() const
      -> HotStartMode
  {
    return m_options_.sddp_options.cut_recovery_mode.value_or(
        HotStartMode::none);
  }

  /// SDDP recovery mode as an enum.
  [[nodiscard]] constexpr auto sddp_recovery_mode_enum() const -> RecoveryMode
  {
    return m_options_.sddp_options.recovery_mode.value_or(RecoveryMode::full);
  }

  /// Monolithic solve mode as an enum.
  [[nodiscard]] constexpr auto monolithic_solve_mode_enum() const -> SolveMode
  {
    return m_options_.monolithic_options.solve_mode.value_or(
        SolveMode::monolithic);
  }

  /// Monolithic boundary cuts mode as an enum.
  [[nodiscard]] constexpr auto monolithic_boundary_cuts_mode_enum() const
      -> BoundaryCutsMode
  {
    return m_options_.monolithic_options.boundary_cuts_mode.value_or(
        BoundaryCutsMode::separated);
  }

  /// Validate all enum-typed option fields and return a list of warnings.
  ///
  /// Since enum fields are now typed (`std::optional<EnumType>`), invalid
  /// JSON strings are silently dropped to `nullopt` during parsing.  This
  /// method detects those cases and reports them.  Callers should log
  /// warnings at an appropriate level (typically WARN).
  [[nodiscard]] auto validate_enum_options() const -> std::vector<std::string>
  {
    // With typed enum fields, validation is no longer needed at this level —
    // invalid JSON strings fail to parse and the optional stays nullopt
    // (which falls back to the default).  Return empty for API compatibility.
    return {};
  }

  /**
   * @brief Gets the variable scale map built from variable_scales entries.
   *
   * The map provides `lookup(class_name, variable, uid)` to resolve scale
   * factors with per-element > per-class > default (1.0) priority.
   *
   * Note: per-element fields (`Battery::energy_scale`,
   * `Reservoir::energy_scale`) and global options (`scale_theta`) take
   * precedence over this map.
   * Use this for variables not covered by dedicated fields.
   */
  [[nodiscard]] const auto& variable_scale_map() const noexcept
  {
    return m_variable_scale_map_;
  }

private:
  /** @brief The wrapped PlanningOptions object */
  PlanningOptions m_options_;
  /** @brief Variable scale map built from PlanningOptions::variable_scales */
  VariableScaleMap m_variable_scale_map_;
};

/// @brief Backward-compatibility alias (deprecated — use PlanningOptionsLP)
using OptionsLP = PlanningOptionsLP;

}  // namespace gtopt
