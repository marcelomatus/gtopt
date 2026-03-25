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
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/sddp_options.hpp>

namespace daw::json
{
using gtopt::SddpOptions;
using gtopt::SolverOptions;

template<>
struct json_data_contract<SddpOptions>
{
  using type = json_member_list<
      json_string_null<"cut_sharing_mode", OptName>,
      json_string_null<"cut_directory", OptName>,
      json_bool_null<"api_enabled", OptBool>,
      json_number_null<"update_lp_skip", OptInt>,
      json_number_null<"max_iterations", OptInt>,
      json_number_null<"min_iterations", OptInt>,
      json_number_null<"convergence_tol", OptReal>,
      json_number_null<"elastic_penalty", OptReal>,
      json_number_null<"alpha_min", OptReal>,
      json_number_null<"alpha_max", OptReal>,
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
      json_bool_null<"warm_start", OptBool>,
      json_string_null<"boundary_cuts_file", OptName>,
      json_string_null<"boundary_cuts_mode", OptName>,
      json_number_null<"boundary_max_iterations", OptInt>,
      json_string_null<"named_cuts_file", OptName>,
      json_number_null<"max_cuts_per_phase", OptInt>,
      json_number_null<"cut_prune_interval", OptInt>,
      json_number_null<"prune_dual_threshold", OptReal>,
      json_bool_null<"single_cut_storage", OptBool>,
      json_number_null<"max_stored_cuts", OptInt>,
      json_bool_null<"use_clone_pool", OptBool>,
      json_bool_null<"simulation_mode", OptBool>,
      json_number_null<"stationary_tol", OptReal>,
      json_number_null<"stationary_window", OptInt>,
      json_class_null<"solver_options", SolverOptions>,
      json_class_null<"forward_solver_options", SolverOptions>,
      json_class_null<"backward_solver_options", SolverOptions>>;

  constexpr static auto to_json_data(SddpOptions const& opt)
  {
    return std::forward_as_tuple(opt.cut_sharing_mode,
                                 opt.cut_directory,
                                 opt.api_enabled,
                                 opt.update_lp_skip,
                                 opt.max_iterations,
                                 opt.min_iterations,
                                 opt.convergence_tol,
                                 opt.elastic_penalty,
                                 opt.alpha_min,
                                 opt.alpha_max,
                                 opt.cut_recovery_mode,
                                 opt.recovery_mode,
                                 opt.save_per_iteration,
                                 opt.cuts_input_file,
                                 opt.sentinel_file,
                                 opt.elastic_mode,
                                 opt.multi_cut_threshold,
                                 opt.apertures,
                                 opt.aperture_directory,
                                 opt.aperture_timeout,
                                 opt.save_aperture_lp,
                                 opt.warm_start,
                                 opt.boundary_cuts_file,
                                 opt.boundary_cuts_mode,
                                 opt.boundary_max_iterations,
                                 opt.named_cuts_file,
                                 opt.max_cuts_per_phase,
                                 opt.cut_prune_interval,
                                 opt.prune_dual_threshold,
                                 opt.single_cut_storage,
                                 opt.max_stored_cuts,
                                 opt.use_clone_pool,
                                 opt.simulation_mode,
                                 opt.stationary_tol,
                                 opt.stationary_window,
                                 opt.solver_options,
                                 opt.forward_solver_options,
                                 opt.backward_solver_options);
  }
};

}  // namespace daw::json
