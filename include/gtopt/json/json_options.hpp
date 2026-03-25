/**
 * @file      json_options.hpp
 * @brief     JSON serialization for Options
 * @date      Sun Apr 20 16:01:20 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_cascade_options.hpp>
#include <gtopt/json/json_lp_build_options.hpp>
#include <gtopt/json/json_model_options.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/json/json_sddp_options.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/json/json_variable_scale.hpp>
#include <gtopt/options.hpp>

namespace daw::json
{
using gtopt::LpBuildOptions;
using gtopt::Options;
using gtopt::SolverOptions;

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
                       json_bool_null<"use_uid_fname", OptBool>,

                       json_string_null<"method", OptName>,
                       json_string_null<"log_directory", OptName>,
                       json_bool_null<"lp_debug", OptBool>,
                       json_string_null<"lp_compression", OptName>,
                       json_bool_null<"lp_build", OptBool>,

                       json_class_null<"model_options", ModelOptions>,
                       json_class_null<"monolithic_options", MonolithicOptions>,
                       json_class_null<"sddp_options", SddpOptions>,
                       json_class_null<"cascade_options", CascadeOptions>,
                       json_class_null<"solver_options", SolverOptions>,
                       json_class_null<"lp_build_options", LpBuildOptions>,
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
                                 opt.use_uid_fname,

                                 opt.method,
                                 opt.log_directory,
                                 opt.lp_debug,
                                 opt.lp_compression,
                                 opt.lp_build,

                                 opt.model_options,
                                 opt.monolithic_options,
                                 opt.sddp_options,
                                 opt.cascade_options,
                                 opt.solver_options,
                                 opt.lp_build_options,
                                 opt.variable_scales);
  }
};

}  // namespace daw::json
