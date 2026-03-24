/**
 * @file      json_options.hpp
 * @brief     JSON serialization for Options and SddpOptions
 * @date      Sun Apr 20 16:01:20 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/json/json_variable_scale.hpp>
#include <gtopt/options.hpp>

namespace daw::json
{
using gtopt::CascadeLevel;
using gtopt::CascadeLevelSolver;
using gtopt::CascadeOptions;
using gtopt::CascadeTransition;
using gtopt::ModelOptions;
using gtopt::MonolithicOptions;
using gtopt::Options;
using gtopt::SddpOptions;
using gtopt::SolverOptions;

template<>
struct json_data_contract<MonolithicOptions>
{
  using type =
      json_member_list<json_string_null<"solve_mode", OptName>,
                       json_string_null<"boundary_cuts_file", OptName>,
                       json_string_null<"boundary_cuts_mode", OptName>,
                       json_number_null<"boundary_max_iterations", OptInt>,
                       json_number_null<"solve_timeout", OptReal>>;

  constexpr static auto to_json_data(MonolithicOptions const& opt)
  {
    return std::forward_as_tuple(opt.solve_mode,
                                 opt.boundary_cuts_file,
                                 opt.boundary_cuts_mode,
                                 opt.boundary_max_iterations,
                                 opt.solve_timeout);
  }
};

template<>
struct json_data_contract<SddpOptions>
{
  using type =
      json_member_list<json_string_null<"cut_sharing_mode", OptName>,
                       json_string_null<"cut_directory", OptName>,
                       json_bool_null<"api_enabled", OptBool>,
                       json_number_null<"update_lp_skip", OptInt>,
                       json_number_null<"max_iterations", OptInt>,
                       json_number_null<"min_iterations", OptInt>,
                       json_number_null<"convergence_tol", OptReal>,
                       json_number_null<"elastic_penalty", OptReal>,
                       json_number_null<"alpha_min", OptReal>,
                       json_number_null<"alpha_max", OptReal>,
                       json_bool_null<"hot_start", OptBool>,
                       json_string_null<"hot_start_mode", OptName>,
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
                       json_number_null<"solve_timeout", OptReal>,
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
                       json_number_null<"stationary_window", OptInt>>;

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
                                 opt.hot_start,
                                 opt.hot_start_mode,
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
                                 opt.solve_timeout,
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
                                 opt.stationary_window);
  }
};

template<>
struct json_data_contract<ModelOptions>
{
  using type =
      json_member_list<json_bool_null<"use_single_bus", OptBool>,
                       json_bool_null<"use_kirchhoff", OptBool>,
                       json_bool_null<"use_line_losses", OptBool>,
                       json_number_null<"kirchhoff_threshold", OptReal>,
                       json_number_null<"loss_segments", OptInt>,
                       json_number_null<"scale_objective", OptReal>,
                       json_number_null<"scale_theta", OptReal>,
                       json_number_null<"demand_fail_cost", OptReal>,
                       json_number_null<"reserve_fail_cost", OptReal>,
                       json_number_null<"annual_discount_rate", OptReal>>;

  constexpr static auto to_json_data(ModelOptions const& opt)
  {
    return std::forward_as_tuple(opt.use_single_bus,
                                 opt.use_kirchhoff,
                                 opt.use_line_losses,
                                 opt.kirchhoff_threshold,
                                 opt.loss_segments,
                                 opt.scale_objective,
                                 opt.scale_theta,
                                 opt.demand_fail_cost,
                                 opt.reserve_fail_cost,
                                 opt.annual_discount_rate);
  }
};

template<>
struct json_data_contract<CascadeTransition>
{
  using type =
      json_member_list<json_number_null<"inherit_optimality_cuts", OptInt>,
                       json_number_null<"inherit_feasibility_cuts", OptInt>,
                       json_number_null<"inherit_targets", OptInt>,
                       json_number_null<"target_rtol", OptReal>,
                       json_number_null<"target_min_atol", OptReal>,
                       json_number_null<"target_penalty", OptReal>,
                       json_number_null<"optimality_dual_threshold", OptReal>>;

  constexpr static auto to_json_data(CascadeTransition const& opt)
  {
    return std::forward_as_tuple(opt.inherit_optimality_cuts,
                                 opt.inherit_feasibility_cuts,
                                 opt.inherit_targets,
                                 opt.target_rtol,
                                 opt.target_min_atol,
                                 opt.target_penalty,
                                 opt.optimality_dual_threshold);
  }
};

template<>
struct json_data_contract<CascadeLevelSolver>
{
  using type = json_member_list<json_number_null<"max_iterations", OptInt>,
                                json_number_null<"min_iterations", OptInt>,
                                json_array_null<"apertures",
                                                std::optional<Array<Uid>>,
                                                json_number_no_name<Uid>>,
                                json_number_null<"convergence_tol", OptReal>>;

  constexpr static auto to_json_data(CascadeLevelSolver const& opt)
  {
    return std::forward_as_tuple(opt.max_iterations,
                                 opt.min_iterations,
                                 opt.apertures,
                                 opt.convergence_tol);
  }
};

template<>
struct json_data_contract<CascadeLevel>
{
  using type =
      json_member_list<json_number_null<"uid", OptUid>,
                       json_string_null<"name", OptName>,
                       json_class_null<"model_options", ModelOptions>,
                       json_class_null<"sddp_options", CascadeLevelSolver>,
                       json_class_null<"transition", CascadeTransition>>;

  constexpr static auto to_json_data(CascadeLevel const& opt)
  {
    return std::forward_as_tuple(
        opt.uid, opt.name, opt.model_options, opt.sddp_options, opt.transition);
  }
};

template<>
struct json_data_contract<CascadeOptions>
{
  using type = json_member_list<
      json_class_null<"model_options", ModelOptions>,
      json_class_null<"sddp_options", SddpOptions>,
      json_array_null<"level_array", gtopt::Array<CascadeLevel>, CascadeLevel>>;

  constexpr static auto to_json_data(CascadeOptions const& opt)
  {
    return std::forward_as_tuple(
        opt.model_options, opt.sddp_options, opt.level_array);
  }
};

template<>
struct json_data_contract<Options>
{
  using type =
      json_member_list<json_string_null<"input_directory", OptName>,
                       json_string_null<"input_format", OptName>,
                       json_number_null<"demand_fail_cost", OptReal>,
                       json_number_null<"reserve_fail_cost", OptReal>,
                       json_bool_null<"use_line_losses", OptBool>,
                       json_number_null<"loss_segments", OptInt>,
                       json_bool_null<"use_kirchhoff", OptBool>,
                       json_bool_null<"use_single_bus", OptBool>,
                       json_number_null<"kirchhoff_threshold", OptReal>,
                       json_number_null<"scale_objective", OptReal>,
                       json_number_null<"scale_theta", OptReal>,
                       json_number_null<"annual_discount_rate", OptReal>,

                       json_string_null<"output_directory", OptName>,
                       json_string_null<"output_format", OptName>,
                       json_string_null<"output_compression", OptName>,
                       json_number_null<"use_lp_names", OptInt>,
                       json_bool_null<"use_uid_fname", OptBool>,

                       json_number_null<"lp_algorithm", OptInt>,
                       json_number_null<"lp_threads", OptInt>,
                       json_bool_null<"lp_presolve", OptBool>,

                       json_string_null<"method", OptName>,
                       json_string_null<"log_directory", OptName>,
                       json_bool_null<"lp_debug", OptBool>,
                       json_string_null<"lp_compression", OptName>,
                       json_bool_null<"build_lp", OptBool>,
                       json_number_null<"lp_coeff_ratio_threshold", OptReal>,

                       json_class_null<"model_options", ModelOptions>,
                       json_class_null<"monolithic_options", MonolithicOptions>,
                       json_class_null<"sddp_options", SddpOptions>,
                       json_class_null<"cascade_options", CascadeOptions>,
                       json_class_null<"solver_options", SolverOptions>,
                       json_array_null<"variable_scales",
                                       gtopt::Array<VariableScale>,
                                       VariableScale>>;

  constexpr static auto to_json_data(Options const& opt)
  {
    return std::forward_as_tuple(opt.input_directory,
                                 opt.input_format,
                                 opt.demand_fail_cost,
                                 opt.reserve_fail_cost,
                                 opt.use_line_losses,
                                 opt.loss_segments,
                                 opt.use_kirchhoff,
                                 opt.use_single_bus,
                                 opt.kirchhoff_threshold,
                                 opt.scale_objective,
                                 opt.scale_theta,
                                 opt.annual_discount_rate,

                                 opt.output_directory,
                                 opt.output_format,
                                 opt.output_compression,
                                 opt.use_lp_names,
                                 opt.use_uid_fname,

                                 opt.lp_algorithm,
                                 opt.lp_threads,
                                 opt.lp_presolve,

                                 opt.method,
                                 opt.log_directory,
                                 opt.lp_debug,
                                 opt.lp_compression,
                                 opt.build_lp,
                                 opt.lp_coeff_ratio_threshold,

                                 opt.model_options,
                                 opt.monolithic_options,
                                 opt.sddp_options,
                                 opt.cascade_options,
                                 opt.solver_options,
                                 opt.variable_scales);
  }
};

}  // namespace daw::json
