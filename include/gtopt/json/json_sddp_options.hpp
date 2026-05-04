/**
 * @file      json_sddp_options.hpp
 * @brief     JSON serialization for SddpOptions
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_enum_option.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/sddp_options.hpp>

namespace daw::json
{
using gtopt::BoundaryCutsMode;
using gtopt::CompressionCodec;
using gtopt::ConvergenceMode;
using gtopt::CutSharingMode;
using gtopt::ElasticFilterMode;
using gtopt::HotStartMode;
using gtopt::LowMemoryMode;
using gtopt::MissingCutVarMode;
using gtopt::RecoveryMode;
using gtopt::SddpOptions;
using gtopt::SolverOptions;
using gtopt::StateVariableLookupMode;

/// Custom constructor: converts JSON strings → typed enums
struct SddpOptionsConstructor
{
  [[nodiscard]] SddpOptions operator()(
      OptName cut_sharing_mode_str,
      OptName cut_directory,
      OptBool api_enabled,
      OptInt update_lp_skip,
      OptInt max_iterations,
      OptInt min_iterations,
      OptReal convergence_tol,
      OptReal elastic_penalty,
      OptReal scale_alpha,
      OptName cut_recovery_mode_str,
      OptName recovery_mode_str,
      OptBool save_per_iteration,
      OptName cuts_input_file,
      OptName sentinel_file,
      OptName elastic_mode_str,
      OptInt multi_cut_threshold,
      std::optional<Array<Uid>> apertures,
      OptName aperture_directory,
      OptReal aperture_timeout,
      OptBool save_aperture_lp,
      OptBool aperture_use_manual_clone,
      OptName boundary_cuts_file,
      OptName boundary_cuts_mode_str,
      OptInt boundary_max_iterations,
      OptName missing_cut_var_mode_str,
      OptName named_cuts_file,
      OptInt max_cuts_per_phase,
      OptInt cut_prune_interval,
      OptReal prune_dual_threshold,
      OptBool single_cut_storage,
      OptInt max_stored_cuts,
      OptBool simulation_mode,
      OptName low_memory_str,
      OptName memory_codec_str,
      OptReal cut_coeff_eps,
      OptName convergence_mode_str,
      OptName state_variable_lookup_mode_str,
      OptReal stationary_tol,
      OptInt stationary_window,
      OptReal convergence_confidence,
      OptReal stationary_gap_ceiling,
      OptInt terminal_failure_threshold,
      OptInt forward_max_fallbacks,
      OptBool forward_fail_stop,
      OptBool forward_infeas_rollback,
      OptBool backward_resolve_target,
      OptInt backward_max_fallbacks,
      OptInt max_async_spread,
      std::optional<SolverOptions> forward_solver_options,
      std::optional<SolverOptions> backward_solver_options) const
  {
    SddpOptions opts;
    if (cut_sharing_mode_str) {
      opts.cut_sharing_mode = gtopt::require_enum<CutSharingMode>(
          "cut_sharing_mode", *cut_sharing_mode_str);
    }
    opts.cut_directory = std::move(cut_directory);
    opts.api_enabled = api_enabled;
    opts.update_lp_skip = update_lp_skip;
    opts.max_iterations = max_iterations;
    opts.min_iterations = min_iterations;
    opts.convergence_tol = convergence_tol;
    opts.elastic_penalty = elastic_penalty;
    opts.scale_alpha = scale_alpha;
    if (cut_recovery_mode_str) {
      opts.cut_recovery_mode = gtopt::require_enum<HotStartMode>(
          "cut_recovery_mode", *cut_recovery_mode_str);
    }
    if (recovery_mode_str) {
      opts.recovery_mode = gtopt::require_enum<RecoveryMode>(
          "recovery_mode", *recovery_mode_str);
    }
    opts.save_per_iteration = save_per_iteration;
    opts.cuts_input_file = std::move(cuts_input_file);
    opts.sentinel_file = std::move(sentinel_file);
    if (elastic_mode_str) {
      opts.elastic_mode = gtopt::require_enum<ElasticFilterMode>(
          "elastic_mode", *elastic_mode_str);
    }
    opts.multi_cut_threshold = multi_cut_threshold;
    opts.apertures = std::move(apertures);
    opts.aperture_directory = std::move(aperture_directory);
    opts.aperture_timeout = aperture_timeout;
    opts.save_aperture_lp = save_aperture_lp;
    opts.aperture_use_manual_clone = aperture_use_manual_clone;
    opts.boundary_cuts_file = std::move(boundary_cuts_file);
    if (boundary_cuts_mode_str) {
      opts.boundary_cuts_mode = gtopt::require_enum<BoundaryCutsMode>(
          "boundary_cuts_mode", *boundary_cuts_mode_str);
    }
    opts.boundary_max_iterations = boundary_max_iterations;
    if (missing_cut_var_mode_str) {
      opts.missing_cut_var_mode = gtopt::require_enum<MissingCutVarMode>(
          "missing_cut_var_mode", *missing_cut_var_mode_str);
    }
    opts.named_cuts_file = std::move(named_cuts_file);
    opts.max_cuts_per_phase = max_cuts_per_phase;
    opts.cut_prune_interval = cut_prune_interval;
    opts.prune_dual_threshold = prune_dual_threshold;
    opts.single_cut_storage = single_cut_storage;
    opts.max_stored_cuts = max_stored_cuts;
    opts.simulation_mode = simulation_mode;
    if (low_memory_str) {
      opts.low_memory_mode = gtopt::require_enum<LowMemoryMode>(
          "low_memory_mode", *low_memory_str);
    }
    if (memory_codec_str) {
      opts.memory_codec = gtopt::require_enum<CompressionCodec>(
          "memory_codec", *memory_codec_str);
    }
    opts.cut_coeff_eps = cut_coeff_eps;
    if (convergence_mode_str) {
      opts.convergence_mode = gtopt::require_enum<ConvergenceMode>(
          "convergence_mode", *convergence_mode_str);
    }
    if (state_variable_lookup_mode_str) {
      opts.state_variable_lookup_mode =
          gtopt::require_enum<StateVariableLookupMode>(
              "state_variable_lookup_mode", *state_variable_lookup_mode_str);
    }
    opts.stationary_tol = stationary_tol;
    opts.stationary_window = stationary_window;
    opts.convergence_confidence = convergence_confidence;
    opts.stationary_gap_ceiling = stationary_gap_ceiling;
    opts.terminal_failure_threshold = terminal_failure_threshold;
    opts.forward_max_fallbacks = forward_max_fallbacks;
    opts.forward_fail_stop = forward_fail_stop;
    opts.forward_infeas_rollback = forward_infeas_rollback;
    opts.backward_resolve_target = backward_resolve_target;
    opts.backward_max_fallbacks = backward_max_fallbacks;
    opts.max_async_spread = max_async_spread;
    opts.forward_solver_options = forward_solver_options;
    opts.backward_solver_options = backward_solver_options;
    return opts;
  }
};

template<>
struct json_data_contract<SddpOptions>
{
  using constructor_t = SddpOptionsConstructor;

  using type = json_member_list<
      json_string_null<"cut_sharing_mode", OptName>,
      json_string_null<"cut_directory", OptName>,
      json_bool_null<"api_enabled", OptBool>,
      json_number_null<"update_lp_skip", OptInt>,
      json_number_null<"max_iterations", OptInt>,
      json_number_null<"min_iterations", OptInt>,
      json_number_null<"convergence_tol", OptReal>,
      json_number_null<"elastic_penalty", OptReal>,
      json_number_null<"scale_alpha", OptReal>,
      json_string_null<"cut_recovery_mode", OptName>,
      json_string_null<"recovery_mode", OptName>,
      json_bool_null<"save_per_iteration", OptBool>,
      json_string_null<"cuts_input_file", OptName>,
      json_string_null<"sentinel_file", OptName>,
      json_string_null<"elastic_mode", OptName>,
      json_number_null<"multi_cut_threshold", OptInt>,
      json_array_null<"apertures",
                      std::optional<Array<Uid>>,
                      json_number_no_name<Uid>>,
      json_string_null<"aperture_directory", OptName>,
      json_number_null<"aperture_timeout", OptReal>,
      json_bool_null<"save_aperture_lp", OptBool>,
      json_bool_null<"aperture_use_manual_clone", OptBool>,
      json_string_null<"boundary_cuts_file", OptName>,
      json_string_null<"boundary_cuts_mode", OptName>,
      json_number_null<"boundary_max_iterations", OptInt>,
      json_string_null<"missing_cut_var_mode", OptName>,
      json_string_null<"named_cuts_file", OptName>,
      json_number_null<"max_cuts_per_phase", OptInt>,
      json_number_null<"cut_prune_interval", OptInt>,
      json_number_null<"prune_dual_threshold", OptReal>,
      json_bool_null<"single_cut_storage", OptBool>,
      json_number_null<"max_stored_cuts", OptInt>,
      json_bool_null<"simulation_mode", OptBool>,
      json_string_null<"low_memory_mode", OptName>,
      json_string_null<"memory_codec", OptName>,
      json_number_null<"cut_coeff_eps", OptReal>,
      json_string_null<"convergence_mode", OptName>,
      json_string_null<"state_variable_lookup_mode", OptName>,
      json_number_null<"stationary_tol", OptReal>,
      json_number_null<"stationary_window", OptInt>,
      json_number_null<"convergence_confidence", OptReal>,
      json_number_null<"stationary_gap_ceiling", OptReal>,
      json_number_null<"terminal_failure_threshold", OptInt>,
      json_number_null<"forward_max_fallbacks", OptInt>,
      json_bool_null<"forward_fail_stop", OptBool>,
      json_bool_null<"forward_infeas_rollback", OptBool>,
      json_bool_null<"backward_resolve_target", OptBool>,
      json_number_null<"backward_max_fallbacks", OptInt>,
      json_number_null<"max_async_spread", OptInt>,
      json_class_null<"forward_solver_options", SolverOptions>,
      json_class_null<"backward_solver_options", SolverOptions>>;

  static auto to_json_data(SddpOptions const& opt)
  {
    return std::make_tuple(
        detail::enum_to_opt_name(opt.cut_sharing_mode),
        opt.cut_directory,
        opt.api_enabled,
        opt.update_lp_skip,
        opt.max_iterations,
        opt.min_iterations,
        opt.convergence_tol,
        opt.elastic_penalty,
        opt.scale_alpha,
        detail::enum_to_opt_name(opt.cut_recovery_mode),
        detail::enum_to_opt_name(opt.recovery_mode),
        opt.save_per_iteration,
        opt.cuts_input_file,
        opt.sentinel_file,
        detail::enum_to_opt_name(opt.elastic_mode),
        opt.multi_cut_threshold,
        opt.apertures,
        opt.aperture_directory,
        opt.aperture_timeout,
        opt.save_aperture_lp,
        opt.aperture_use_manual_clone,
        opt.boundary_cuts_file,
        detail::enum_to_opt_name(opt.boundary_cuts_mode),
        opt.boundary_max_iterations,
        detail::enum_to_opt_name(opt.missing_cut_var_mode),
        opt.named_cuts_file,
        opt.max_cuts_per_phase,
        opt.cut_prune_interval,
        opt.prune_dual_threshold,
        opt.single_cut_storage,
        opt.max_stored_cuts,
        opt.simulation_mode,
        detail::enum_to_opt_name(opt.low_memory_mode),
        detail::enum_to_opt_name(opt.memory_codec),
        opt.cut_coeff_eps,
        detail::enum_to_opt_name(opt.convergence_mode),
        detail::enum_to_opt_name(opt.state_variable_lookup_mode),
        opt.stationary_tol,
        opt.stationary_window,
        opt.convergence_confidence,
        opt.stationary_gap_ceiling,
        opt.terminal_failure_threshold,
        opt.forward_max_fallbacks,
        opt.forward_fail_stop,
        opt.forward_infeas_rollback,
        opt.backward_resolve_target,
        opt.backward_max_fallbacks,
        opt.max_async_spread,
        opt.forward_solver_options,
        opt.backward_solver_options);
  }
};

}  // namespace daw::json
