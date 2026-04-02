/**
 * @file      json_options.hpp
 * @brief     JSON serialization for PlanningOptions
 * @date      Sun Apr 20 16:01:20 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_cascade_options.hpp>
#include <gtopt/json/json_enum_option.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_lp_build_options.hpp>
#include <gtopt/json/json_model_options.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/json/json_sddp_options.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/json/json_variable_scale.hpp>
#include <gtopt/planning_options.hpp>
#include <spdlog/spdlog.h>

namespace daw::json
{
using gtopt::CompressionCodec;
using gtopt::ConstraintMode;
using gtopt::DataFormat;
using gtopt::LpBuildOptions;
using gtopt::MethodType;
using gtopt::PlanningOptions;
using gtopt::SolverOptions;

/// Custom constructor: converts JSON strings → typed enums for PlanningOptions
struct PlanningOptionsConstructor
{
  [[nodiscard]] PlanningOptions operator()(
      OptName input_directory,
      OptName input_format_str,
      OptReal demand_fail_cost,
      OptReal reserve_fail_cost,
      OptReal hydro_fail_cost,
      OptReal hydro_use_value,
      OptBool use_line_losses,
      OptInt loss_segments,
      OptBool use_kirchhoff,
      OptBool use_single_bus,
      OptReal kirchhoff_threshold,
      OptReal scale_objective,
      OptReal scale_theta,
      OptReal annual_discount_rate,
      OptName output_directory,
      OptName output_format_str,
      OptName output_compression_str,
      OptInt use_lp_names,
      OptBool use_uid_fname,
      OptName method_str,
      OptName log_directory,
      OptBool lp_debug,
      OptName lp_compression_str,
      OptBool lp_build,
      ModelOptions model_options,
      MonolithicOptions monolithic_options,
      SddpOptions sddp_options,
      CascadeOptions cascade_options,
      SolverOptions solver_options,
      LpBuildOptions lp_build_options,
      gtopt::Array<VariableScale> variable_scales,
      OptName constraint_mode_str) const
  {
    PlanningOptions opts;
    opts.input_directory = std::move(input_directory);
    if (input_format_str) {
      opts.input_format = gtopt::enum_from_name<DataFormat>(*input_format_str);
    }
    opts.demand_fail_cost = demand_fail_cost;
    opts.reserve_fail_cost = reserve_fail_cost;
    opts.hydro_fail_cost = hydro_fail_cost;
    opts.hydro_use_value = hydro_use_value;
    opts.use_line_losses = use_line_losses;
    opts.loss_segments = loss_segments;
    opts.use_kirchhoff = use_kirchhoff;
    opts.use_single_bus = use_single_bus;
    opts.kirchhoff_threshold = kirchhoff_threshold;
    opts.scale_objective = scale_objective;
    opts.scale_theta = scale_theta;
    opts.annual_discount_rate = annual_discount_rate;
    opts.output_directory = std::move(output_directory);
    if (output_format_str) {
      opts.output_format =
          gtopt::enum_from_name<DataFormat>(*output_format_str);
    }
    if (output_compression_str) {
      opts.output_compression =
          gtopt::enum_from_name<CompressionCodec>(*output_compression_str);
    }
    // Backward compat: deprecated "use_lp_names" maps to
    // lp_build_options.names_level when the new field is not set.
    if (use_lp_names) {
      spdlog::warn(
          "deprecated option 'use_lp_names': "
          "use 'lp_build_options.names_level' instead");
      if (!lp_build_options.names_level) {
        lp_build_options.names_level =
            static_cast<gtopt::LpNamesLevel>(*use_lp_names);
      }
    }
    opts.use_uid_fname = use_uid_fname;
    if (method_str) {
      opts.method = gtopt::enum_from_name<MethodType>(*method_str);
    }
    opts.log_directory = std::move(log_directory);
    opts.lp_debug = lp_debug;
    if (lp_compression_str) {
      opts.lp_compression =
          gtopt::enum_from_name<CompressionCodec>(*lp_compression_str);
    }
    opts.lp_build = lp_build;
    opts.model_options = model_options;
    opts.monolithic_options = std::move(monolithic_options);
    opts.sddp_options = std::move(sddp_options);
    opts.cascade_options = std::move(cascade_options);
    opts.solver_options = solver_options;
    opts.lp_build_options = std::move(lp_build_options);
    opts.variable_scales = std::move(variable_scales);
    if (constraint_mode_str) {
      opts.constraint_mode =
          gtopt::enum_from_name<ConstraintMode>(*constraint_mode_str);
    }
    return opts;
  }
};

template<>
struct json_data_contract<PlanningOptions>
{
  using constructor_t = PlanningOptionsConstructor;

  using type =
      json_member_list<json_string_null<"input_directory", OptName>,
                       json_string_null<"input_format", OptName>,
                       json_number_null<"demand_fail_cost", OptReal>,
                       json_number_null<"reserve_fail_cost", OptReal>,
                       json_number_null<"hydro_fail_cost", OptReal>,
                       json_number_null<"hydro_use_value", OptReal>,
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
                                       VariableScale>,
                       json_string_null<"constraint_mode", OptName>>;

  static auto to_json_data(PlanningOptions const& opt)
  {
    return std::make_tuple(opt.input_directory,
                           detail::enum_to_opt_name(opt.input_format),
                           opt.demand_fail_cost,
                           opt.reserve_fail_cost,
                           opt.hydro_fail_cost,
                           opt.hydro_use_value,
                           opt.use_line_losses,
                           opt.loss_segments,
                           opt.use_kirchhoff,
                           opt.use_single_bus,
                           opt.kirchhoff_threshold,
                           opt.scale_objective,
                           opt.scale_theta,
                           opt.annual_discount_rate,

                           opt.output_directory,
                           detail::enum_to_opt_name(opt.output_format),
                           detail::enum_to_opt_name(opt.output_compression),
                           OptInt {},
                           opt.use_uid_fname,

                           detail::enum_to_opt_name(opt.method),
                           opt.log_directory,
                           opt.lp_debug,
                           detail::enum_to_opt_name(opt.lp_compression),
                           opt.lp_build,

                           opt.model_options,
                           opt.monolithic_options,
                           opt.sddp_options,
                           opt.cascade_options,
                           opt.solver_options,
                           opt.lp_build_options,
                           opt.variable_scales,
                           detail::enum_to_opt_name(opt.constraint_mode));
  }
};

}  // namespace daw::json
