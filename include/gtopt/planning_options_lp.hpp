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
#include <gtopt/line_enums.hpp>
#include <gtopt/phase_range_set.hpp>
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
  /** @brief Default setting for line loss modeling (deprecated) */
  static constexpr Bool default_use_line_losses = true;
  /** @brief Default line losses mode */
  static constexpr LineLossesMode default_line_losses_mode =
      LineLossesMode::adaptive;
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

  // Default values for output settings
  /** @brief Default output directory path */
  static constexpr auto default_output_directory = "output";
  /** @brief Default output file format */
  static constexpr DataFormat default_output_format = DataFormat::parquet;
  /** @brief Default compression codec for output files */
  static constexpr CompressionCodec default_output_compression =
      CompressionCodec::zstd;
  /** @brief Default setting for using UIDs in filenames */
  static constexpr Bool default_use_uid_fname = true;
  /** @brief Default annual discount rate for multi-year planning */
  static constexpr Real default_annual_discount_rate = 0.0;

  // ── Fallback helpers ─────────────────────────────────────────────────
  // Three-tier resolution: flat → model_options → compiled default.

  template<typename T>
  [[nodiscard]] static constexpr auto fallback_3(const std::optional<T>& flat,
                                                 const std::optional<T>& model,
                                                 const T& def) noexcept -> T
  {
    if (flat.has_value()) {
      return *flat;
    }
    return model.value_or(def);
  }

  /**
   * @brief Constructs a PlanningOptionsLP wrapper around a PlanningOptions
   * object
   * @param poptions The PlanningOptions object to wrap (defaults to empty)
   */
  explicit PlanningOptionsLP(PlanningOptions poptions = {})
      : m_options_(
            (poptions.migrate_flat_to_model_options(), std::move(poptions)))
      , m_variable_scale_map_(populate_variable_scales(m_options_))
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

  /// @brief Gets the demand failure cost from model_options.
  [[nodiscard]] constexpr auto demand_fail_cost() const
  {
    return m_options_.model_options.demand_fail_cost;
  }

  /// @brief Gets the hydro failure cost from model_options.
  [[nodiscard]] constexpr auto hydro_fail_cost() const
  {
    return m_options_.model_options.hydro_fail_cost;
  }

  /// @brief Gets the hydro use value (benefit per m³) from model_options.
  [[nodiscard]] constexpr auto hydro_use_value() const
  {
    return m_options_.model_options.hydro_use_value;
  }

  /// @brief Gets the reserve failure cost from model_options.
  [[nodiscard]] constexpr auto reserve_fail_cost() const
  {
    return m_options_.model_options.reserve_fail_cost;
  }

  /// @brief Gets the state failure cost from model_options [$/MWh].
  [[nodiscard]] constexpr auto state_fail_cost() const
  {
    return m_options_.model_options.state_fail_cost;
  }

  /// @brief Gets the system-wide emission cost [$/tCO2].
  [[nodiscard]] constexpr const auto& emission_cost() const
  {
    return m_options_.model_options.emission_cost;
  }

  /// @brief Gets the system-wide emission cap [tCO2/year] per stage.
  [[nodiscard]] constexpr const auto& emission_cap() const
  {
    return m_options_.model_options.emission_cap;
  }

  /// @brief Whether a given phase should use continuous LP relaxation.
  /// Parses the `continuous_phases` string from model_options using
  /// PhaseRangeSet.
  [[nodiscard]] bool is_phase_continuous(int phase_index) const
  {
    const auto& cp = m_options_.model_options.continuous_phases;
    if (!cp.has_value()) {
      return false;
    }
    return PhaseRangeSet(*cp).contains(phase_index);
  }

  /// @brief Gets the line losses mode, with backward-compat fallback.
  ///
  /// Priority: model_options.line_losses_mode (string) →
  ///           model_options.use_line_losses (bool, deprecated) →
  ///           default_line_losses_mode (adaptive).
  [[nodiscard]] constexpr LineLossesMode line_losses_mode() const
  {
    if (m_options_.model_options.line_losses_mode.has_value()) {
      return enum_from_name<LineLossesMode>(
                 *m_options_.model_options.line_losses_mode)
          .value_or(default_line_losses_mode);
    }
    if (m_options_.model_options.use_line_losses.has_value()) {
      return *m_options_.model_options.use_line_losses
          ? default_line_losses_mode
          : LineLossesMode::none;
    }
    return default_line_losses_mode;
  }

  /// @brief Gets the line loss modeling flag from model_options.
  /// @deprecated Use line_losses_mode() instead.
  [[nodiscard]] constexpr auto use_line_losses() const
  {
    return line_losses_mode() != LineLossesMode::none;
  }

  /// @brief Gets the number of piecewise-linear loss segments.
  [[nodiscard]] constexpr auto loss_segments() const
  {
    return m_options_.model_options.loss_segments.value_or(
        default_loss_segments);
  }

  /// @brief Gets the Kirchhoff constraints flag.
  [[nodiscard]] constexpr auto use_kirchhoff() const
  {
    return m_options_.model_options.use_kirchhoff.value_or(
        default_use_kirchhoff);
  }

  /// @brief Gets the single-bus modeling flag.
  [[nodiscard]] constexpr auto use_single_bus() const
  {
    return m_options_.model_options.use_single_bus.value_or(
        default_use_single_bus);
  }

  /// @brief Gets the objective function scaling factor.
  [[nodiscard]] constexpr auto scale_objective() const
  {
    return m_options_.model_options.scale_objective.value_or(
        default_scale_objective);
  }

  /// @brief Gets the Kirchhoff threshold.
  [[nodiscard]] constexpr auto kirchhoff_threshold() const
  {
    return m_options_.model_options.kirchhoff_threshold.value_or(
        default_kirchhoff_threshold);
  }

  /// @brief Gets the voltage angle scaling factor.
  [[nodiscard]] constexpr auto scale_theta() const
  {
    return m_options_.model_options.scale_theta.value_or(1.0);
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
  /// @brief Gets the annual discount rate (deprecated flat field fallback).
  /// Canonical location is simulation.annual_discount_rate.
  [[nodiscard]] constexpr auto annual_discount_rate() const
  {
    return m_options_.annual_discount_rate.value_or(
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

  /// Minimum scene UID for selective LP debug saving (nullopt = no filter).
  [[nodiscard]] constexpr auto lp_debug_scene_min() const -> OptInt
  {
    return m_options_.lp_debug_scene_min;
  }
  /// Maximum scene UID for selective LP debug saving (nullopt = no filter).
  [[nodiscard]] constexpr auto lp_debug_scene_max() const -> OptInt
  {
    return m_options_.lp_debug_scene_max;
  }
  /// Minimum phase UID for selective LP debug saving (nullopt = no filter).
  [[nodiscard]] constexpr auto lp_debug_phase_min() const -> OptInt
  {
    return m_options_.lp_debug_phase_min;
  }
  /// Maximum phase UID for selective LP debug saving (nullopt = no filter).
  [[nodiscard]] constexpr auto lp_debug_phase_max() const -> OptInt
  {
    return m_options_.lp_debug_phase_max;
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
   * @brief Gets the lp_only flag, using default if not set.
   *
   * When true, the solver builds all scene×phase LP matrices but skips
   * solving entirely.  Applies uniformly to both the monolithic solver and
   * the SDDP solver: exit right after LP assembly with no solve at all.
   * Combine with lp_debug=true to save every scene/phase LP file to disk.
   *
   * @return Whether to stop after LP building
   */
  [[nodiscard]] constexpr auto lp_only() const
  {
    return m_options_.lp_only.value_or(false);
  }

  /**
   * @brief Gets the lp_fingerprint flag, using default if not set.
   *
   * When true, write LP fingerprint JSON to the output directory after
   * LP assembly.  The fingerprint captures the structural template
   * (which types of variables/constraints exist) for regression detection.
   *
   * @return Whether to write LP fingerprint files
   */
  [[nodiscard]] constexpr auto lp_fingerprint() const
  {
    return m_options_.lp_fingerprint.value_or(false);
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
    return m_options_.lp_matrix_options.lp_coeff_ratio_threshold.value_or(
        default_lp_coeff_ratio_threshold);
  }

  /// The matrix equilibration method to use.
  ///
  /// Default is `ruiz` when Kirchhoff constraints are active on a multi-bus
  /// network (heterogeneous row/column scales from reactances and loss
  /// segments produce high coefficient ratios), otherwise `row_max`.
  /// A user-supplied value always takes precedence.
  [[nodiscard]] constexpr auto equilibration_method() const noexcept
  {
    if (m_options_.lp_matrix_options.equilibration_method.has_value()) {
      return *m_options_.lp_matrix_options.equilibration_method;
    }
    const bool kirchhoff_multi_bus = use_kirchhoff() && !use_single_bus();
    return kirchhoff_multi_bus ? LpEquilibrationMethod::ruiz
                               : LpEquilibrationMethod::row_max;
  }

  /// Controls error handling for user constraint resolution.
  /// Default is `strict` — fail on any unresolved reference.
  [[nodiscard]] constexpr auto constraint_mode() const noexcept
  {
    return m_options_.constraint_mode.value_or(ConstraintMode::strict);
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
    auto opts = m_options_.solver_options;
    if (m_options_.sddp_options.forward_solver_options.has_value()) {
      opts.merge(*m_options_.sddp_options.forward_solver_options);
    }
    opts.max_fallbacks = m_options_.sddp_options.forward_max_fallbacks.value_or(
        opts.max_fallbacks);
    return opts;
  }

  /**
   * @brief Gets the effective SDDP backward-pass solver options.
   *
   * Merges the per-pass backward solver options (if set) with the global
   * solver_options.  Backward-pass-specific options take precedence.
   * Applies backward_max_fallbacks (default: 0).
   *
   * @return Resolved SolverOptions for SDDP backward pass
   */
  [[nodiscard]] auto sddp_backward_solver_options() const -> SolverOptions
  {
    auto opts = m_options_.solver_options;
    if (m_options_.sddp_options.backward_solver_options.has_value()) {
      opts.merge(*m_options_.sddp_options.backward_solver_options);
    }
    opts.max_fallbacks =
        m_options_.sddp_options.backward_max_fallbacks.value_or(0);
    return opts;
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
  static constexpr Real default_sddp_elastic_penalty = 1e3;
  /** @brief Default lower bound for future cost variable α */
  static constexpr Real default_sddp_alpha_min = 0.0;
  /** @brief Default upper bound for future cost variable α */
  static constexpr Real default_sddp_alpha_max = 1e12;
  /** @brief Default cut coefficient epsilon for filtering tiny coefficients.
   *
   * Raised from 1e-12 to 1e-6 (P1-2) so that Benders cuts drop
   * near-zero coefficients that the LP solver cannot distinguish from
   * noise anyway. Keeping micro-coefficients around inflates the basis
   * condition number (kappa) by several orders of magnitude on large
   * GTEP cases without changing the optimal value.  Users solving
   * academic-scale instances can still lower this in JSON.
   */
  static constexpr Real default_sddp_cut_coeff_eps = 1e-6;
  /** @brief Default max coefficient threshold for cut rescaling */
  static constexpr Real default_sddp_cut_coeff_max = 1e6;
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
  static constexpr Int default_sddp_stationary_window = 4;
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
  /// Returns the user-provided scale_alpha, or 0 for auto-scale
  /// (computed at runtime as max state-variable var_scale).
  [[nodiscard]] constexpr auto sddp_scale_alpha() const
  {
    return m_options_.sddp_options.scale_alpha.value_or(0.0);
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
   * @brief Gets the cut coefficient tolerance for filtering tiny coefficients
   * @return Absolute tolerance (default: 0.0 = no filtering)
   */
  [[nodiscard]] constexpr auto sddp_cut_coeff_eps() const -> double
  {
    return m_options_.sddp_options.cut_coeff_eps.value_or(
        default_sddp_cut_coeff_eps);
  }

  /**
   * @brief Gets the max coefficient threshold for cut rescaling
   * @return Max threshold (default: 1e6)
   */
  [[nodiscard]] constexpr auto sddp_cut_coeff_max() const -> double
  {
    return m_options_.sddp_options.cut_coeff_max.value_or(
        default_sddp_cut_coeff_max);
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

  /// Low memory mode: off, snapshot, or compress.
  [[nodiscard]] constexpr auto sddp_low_memory() const
  {
    return m_options_.sddp_options.low_memory_mode.value_or(LowMemoryMode::off);
  }

  /// In-memory compression codec for low_memory level 2.
  [[nodiscard]] constexpr auto sddp_memory_codec() const
  {
    return m_options_.sddp_options.memory_codec.value_or(
        CompressionCodec::auto_select);
  }

  /** @brief Maximum async iteration spread (0 = synchronous, default). */
  [[nodiscard]] constexpr auto sddp_max_async_spread() const
  {
    return m_options_.sddp_options.max_async_spread.value_or(0);
  }

  /** @brief SDDP work pool CPU over-commit factor (default: 4.0). */
  [[nodiscard]] constexpr auto sddp_pool_cpu_factor() const
  {
    return m_options_.sddp_options.pool_cpu_factor.value_or(4.0);
  }

  /** @brief LP-build work-pool CPU over-commit factor (default: 2.0).
   *
   * Currently shares the `sddp_options.pool_cpu_factor` field with the
   * SDDP pool so that a single `--cpu-factor` CLI flag controls both
   * pools uniformly.  When the user does **not** set `--cpu-factor`,
   * the LP-build pool falls back to a more conservative 2.0× factor
   * (vs the SDDP pool's 4.0×) — reflecting that LP build is memory-
   * heavy (constructing full SystemLP trees) while SDDP solving is
   * CPU-bound (solver calls).  When the CLI flag is set, both pools
   * pick it up and the user can force a 1-thread serial baseline via
   * `--cpu-factor 0.025` on a typical 20-core box.
   */
  [[nodiscard]] constexpr auto build_pool_cpu_factor() const
  {
    return m_options_.sddp_options.pool_cpu_factor.value_or(2.0);
  }

  /** @brief SDDP work pool memory limit in MB (0 = no limit). */
  [[nodiscard]] constexpr auto sddp_pool_memory_limit_mb() const
  {
    return m_options_.sddp_options.pool_memory_limit_mb.value_or(0.0);
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

  /// LP-build mode as an enum.  Defaults to `scene_parallel` — the
  /// pre-00c605d7 per-scene work-pool submission (coarse granularity,
  /// lower pool/malloc-arena contention).  Users may opt into
  /// `full_parallel` via `--build-mode full-parallel` for maximum
  /// concurrency, or `serial` for a genuine in-thread baseline.
  [[nodiscard]] constexpr auto build_mode_enum() const -> BuildMode
  {
    return m_options_.build_mode.value_or(BuildMode::scene_parallel);
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
  [[nodiscard]] static auto validate_enum_options() -> std::vector<std::string>
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
   * Global scales (`scale_theta`, `scale_alpha`) are auto-injected into
   * this map by `populate_variable_scales()` at construction time.
   * Per-element fields (`Battery::energy_scale`, `Reservoir::energy_scale`)
   * still take precedence over map entries.
   */
  [[nodiscard]] const auto& variable_scale_map() const noexcept
  {
    return m_variable_scale_map_;
  }

private:
  /// @brief Populate variable_scales from dedicated scale options.
  ///
  /// Injects Bus.theta (from scale_theta) and Sddp.alpha (from scale_alpha)
  /// into the variable_scales array unless the user already provided them.
  /// Convention: `physical = LP × scale` for all entries.
  static auto populate_variable_scales(PlanningOptions& opts)
      -> std::span<const VariableScale>
  {
    auto has_entry = [&](std::string_view cls, std::string_view var) -> bool
    {
      return std::ranges::any_of(opts.variable_scales,
                                 [&](const VariableScale& vs)
                                 {
                                   return vs.class_name == cls
                                       && vs.variable == var
                                       && vs.uid == unknown_uid;
                                 });
    };

    // Inject Bus.theta — scale_theta already follows physical = LP × scale
    if (!has_entry("Bus", "theta")) {
      const auto st =
          fallback_3(opts.scale_theta, opts.model_options.scale_theta, 1.0);
      opts.variable_scales.push_back(VariableScale {
          .class_name = "Bus",
          .variable = "theta",
          .scale = st,
      });
    }

    // Inject Sddp.alpha — only when user provided an explicit scale_alpha.
    // When scale_alpha is unset (auto-scale), the SDDP method computes
    // it at runtime from max(var_scale) and sets col_scale directly.
    if (!has_entry("Sddp", "alpha")
        && opts.sddp_options.scale_alpha.has_value())
    {
      opts.variable_scales.push_back(VariableScale {
          .class_name = "Sddp",
          .variable = "alpha",
          .scale = *opts.sddp_options.scale_alpha,
      });
    }

    return opts.variable_scales;
  }

  /** @brief The wrapped PlanningOptions object */
  PlanningOptions m_options_;
  /** @brief Variable scale map built from PlanningOptions::variable_scales */
  VariableScaleMap m_variable_scale_map_;
};

/// @brief Backward-compatibility alias (deprecated — use PlanningOptionsLP)
using OptionsLP = PlanningOptionsLP;

}  // namespace gtopt
