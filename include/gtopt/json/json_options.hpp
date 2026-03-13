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
#include <gtopt/options.hpp>

namespace daw::json
{
using gtopt::Options;
using gtopt::SddpOptions;

template<>
struct json_data_contract<SddpOptions>
{
  using type =
      json_member_list<json_string_null<"sddp_solver_type", OptName>,
                       json_string_null<"sddp_cut_sharing_mode", OptName>,
                       json_string_null<"sddp_cut_directory", OptName>,
                       json_bool_null<"sddp_api_enabled", OptBool>,
                       json_number_null<"sddp_efficiency_update_skip", OptInt>,
                       json_number_null<"sddp_max_iterations", OptInt>,
                       json_number_null<"sddp_convergence_tol", OptReal>,
                       json_number_null<"sddp_elastic_penalty", OptReal>,
                       json_number_null<"sddp_alpha_min", OptReal>,
                       json_number_null<"sddp_alpha_max", OptReal>,
                       json_string_null<"sddp_cuts_input_file", OptName>,
                       json_string_null<"sddp_sentinel_file", OptName>,
                       json_string_null<"sddp_elastic_mode", OptName>,
                       json_number_null<"sddp_multi_cut_threshold", OptInt>,
                       json_number_null<"sddp_num_apertures", OptInt>>;

  constexpr static auto to_json_data(SddpOptions const& opt)
  {
    return std::forward_as_tuple(opt.sddp_solver_type,
                                 opt.sddp_cut_sharing_mode,
                                 opt.sddp_cut_directory,
                                 opt.sddp_api_enabled,
                                 opt.sddp_efficiency_update_skip,
                                 opt.sddp_max_iterations,
                                 opt.sddp_convergence_tol,
                                 opt.sddp_elastic_penalty,
                                 opt.sddp_alpha_min,
                                 opt.sddp_alpha_max,
                                 opt.sddp_cuts_input_file,
                                 opt.sddp_sentinel_file,
                                 opt.sddp_elastic_mode,
                                 opt.sddp_multi_cut_threshold,
                                 opt.sddp_num_apertures);
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
                       json_bool_null<"use_lp_names", OptBool>,
                       json_bool_null<"use_uid_fname", OptBool>,

                       json_number_null<"lp_algorithm", OptInt>,
                       json_number_null<"lp_threads", OptInt>,
                       json_bool_null<"lp_presolve", OptBool>,

                       json_string_null<"log_directory", OptName>,
                       json_bool_null<"lp_debug", OptBool>,

                       json_class_null<"sddp_options", SddpOptions>>;

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

                                 opt.log_directory,
                                 opt.lp_debug,

                                 opt.sddp_options);
  }
};

}  // namespace daw::json
