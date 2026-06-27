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
#include <gtopt/json/json_lp_matrix_options.hpp>
#include <gtopt/json/json_model_options.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/json/json_sddp_options.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/json/json_variable_scale.hpp>
#include <gtopt/planning_options.hpp>
#include <spdlog/spdlog.h>

namespace daw::json
{
using gtopt::BuildMode;
using gtopt::CompressionCodec;
using gtopt::ConstraintMode;
using gtopt::DataFormat;
using gtopt::LpMatrixOptions;
using gtopt::MethodType;
using gtopt::PlanningOptions;
using gtopt::SolverOptions;

/// Custom constructor: converts JSON strings → typed enums for PlanningOptions.
///
/// The 11 legacy top-level mirror fields (`demand_fail_cost`,
/// `reserve_fail_cost`, `hydro_fail_cost`, `hydro_use_value`,
/// `use_line_losses`, `loss_segments`, `use_kirchhoff`, `use_single_bus`,
/// `kirchhoff_threshold`, `scale_objective`, `scale_theta`) were removed
/// 2026-05-17 per §11.  JSON files placing those keys at the top level
/// are now rejected by `StrictParsePolicy`; place them inside
/// `options.model_options.*` (test fixtures were migrated by
/// `tools/migrate_legacy_options_to_model_options.py`).
struct PlanningOptionsConstructor
{
  [[nodiscard]] PlanningOptions operator()(
      OptName input_directory,
      OptName input_format_str,
      OptReal annual_discount_rate,
      OptName output_directory,
      OptName output_format_str,
      OptName output_compression_str,
      OptInt output_round_decimals,
      OptBool use_uid_fname,
      OptName method_str,
      OptName build_mode_str,
      OptName log_directory,
      OptBool lp_debug,
      OptName lp_compression_str,
      OptBool lp_only,
      OptBool lp_fingerprint,
      OptInt lp_debug_scene_min,
      OptInt lp_debug_scene_max,
      OptInt lp_debug_phase_min,
      OptInt lp_debug_phase_max,
      ModelOptions model_options,
      MonolithicOptions monolithic_options,
      SddpOptions sddp_options,
      CascadeOptions cascade_options,
      SolverOptions solver_options,
      LpMatrixOptions lp_matrix_options,
      gtopt::Array<VariableScale> variable_scales,
      OptName constraint_mode_str,
      OptName write_out_str) const
  {
    PlanningOptions opts;
    opts.input_directory = std::move(input_directory);
    if (input_format_str) {
      opts.input_format =
          gtopt::require_enum<DataFormat>("input_format", *input_format_str);
    }

    // annual_discount_rate: canonical location is `simulation`, not
    // `options` — keep the deprecation warning even after the §11
    // top-level deletion (the field still exists on PlanningOptions
    // for back-compat).
    if (annual_discount_rate) {
      spdlog::warn(
          "deprecated option 'annual_discount_rate': "
          "use 'simulation.annual_discount_rate' instead");
    }
    opts.annual_discount_rate = annual_discount_rate;

    opts.output_directory = std::move(output_directory);
    if (output_format_str) {
      opts.output_format =
          gtopt::require_enum<DataFormat>("output_format", *output_format_str);
    }
    if (output_compression_str) {
      opts.output_compression = gtopt::require_enum<CompressionCodec>(
          "output_compression", *output_compression_str);
    }
    opts.output_round_decimals = output_round_decimals;
    opts.use_uid_fname = use_uid_fname;
    if (method_str) {
      opts.method = gtopt::require_enum<MethodType>("method", *method_str);
    }
    if (build_mode_str) {
      opts.build_mode =
          gtopt::require_enum<BuildMode>("build_mode", *build_mode_str);
    }
    opts.log_directory = std::move(log_directory);
    opts.lp_debug = lp_debug;
    if (lp_compression_str) {
      opts.lp_compression = gtopt::require_enum<CompressionCodec>(
          "lp_compression", *lp_compression_str);
    }
    opts.lp_only = lp_only;
    opts.lp_fingerprint = lp_fingerprint;
    opts.lp_debug_scene_min = lp_debug_scene_min;
    opts.lp_debug_scene_max = lp_debug_scene_max;
    opts.lp_debug_phase_min = lp_debug_phase_min;
    opts.lp_debug_phase_max = lp_debug_phase_max;
    opts.model_options = model_options;
    opts.monolithic_options = std::move(monolithic_options);
    opts.sddp_options = std::move(sddp_options);
    opts.cascade_options = std::move(cascade_options);
    opts.solver_options = solver_options;
    opts.lp_matrix_options = std::move(lp_matrix_options);
    opts.variable_scales = std::move(variable_scales);
    if (constraint_mode_str) {
      opts.constraint_mode = gtopt::require_enum<ConstraintMode>(
          "constraint_mode", *constraint_mode_str);
    }
    if (write_out_str) {
      opts.write_out = gtopt::parse_output_selection(*write_out_str);
    }
    return opts;
  }
};

template<>
struct json_data_contract<PlanningOptions>
{
  using constructor_t = PlanningOptionsConstructor;

  // The 11 deprecated top-level ModelOptions-mirror keys
  // (`demand_fail_cost`, `reserve_fail_cost`, `hydro_fail_cost`,
  // `hydro_use_value`, `use_line_losses`, `loss_segments`,
  // `use_kirchhoff`, `use_single_bus`, `kirchhoff_threshold`,
  // `scale_objective`, `scale_theta`) were removed from the contract
  // 2026-05-17 per §11.  Place them inside `options.model_options.*`.
  using type =
      json_member_list<json_string_null<"input_directory", OptName>,
                       json_string_null<"input_format", OptName>,
                       json_number_null<"annual_discount_rate", OptReal>,

                       json_string_null<"output_directory", OptName>,
                       json_string_null<"output_format", OptName>,
                       json_string_null<"output_compression", OptName>,
                       json_number_null<"output_round_decimals", OptInt>,
                       json_bool_null<"use_uid_fname", OptBool>,

                       json_string_null<"method", OptName>,
                       json_string_null<"build_mode", OptName>,
                       json_string_null<"log_directory", OptName>,
                       json_bool_null<"lp_debug", OptBool>,
                       json_string_null<"lp_compression", OptName>,
                       json_bool_null<"lp_only", OptBool>,
                       json_bool_null<"lp_fingerprint", OptBool>,
                       json_number_null<"lp_debug_scene_min", OptInt>,
                       json_number_null<"lp_debug_scene_max", OptInt>,
                       json_number_null<"lp_debug_phase_min", OptInt>,
                       json_number_null<"lp_debug_phase_max", OptInt>,

                       json_class_null<"model_options", ModelOptions>,
                       json_class_null<"monolithic_options", MonolithicOptions>,
                       json_class_null<"sddp_options", SddpOptions>,
                       json_class_null<"cascade_options", CascadeOptions>,
                       json_class_null<"solver_options", SolverOptions>,
                       json_class_null<"lp_matrix_options", LpMatrixOptions>,
                       json_array_null<"variable_scales",
                                       gtopt::Array<VariableScale>,
                                       VariableScale>,
                       json_string_null<"constraint_mode", OptName>,
                       json_string_null<"write_out", OptName>>;

  static auto to_json_data(PlanningOptions const& opt)
  {
    return std::make_tuple(
        opt.input_directory,
        detail::enum_to_opt_name(opt.input_format),
        opt.annual_discount_rate,

        opt.output_directory,
        detail::enum_to_opt_name(opt.output_format),
        detail::enum_to_opt_name(opt.output_compression),
        opt.output_round_decimals,
        opt.use_uid_fname,

        detail::enum_to_opt_name(opt.method),
        detail::enum_to_opt_name(opt.build_mode),
        opt.log_directory,
        opt.lp_debug,
        detail::enum_to_opt_name(opt.lp_compression),
        opt.lp_only,
        opt.lp_fingerprint,
        opt.lp_debug_scene_min,
        opt.lp_debug_scene_max,
        opt.lp_debug_phase_min,
        opt.lp_debug_phase_max,

        opt.model_options,
        opt.monolithic_options,
        opt.sddp_options,
        opt.cascade_options,
        opt.solver_options,
        opt.lp_matrix_options,
        opt.variable_scales,
        detail::enum_to_opt_name(opt.constraint_mode),
        opt.write_out.has_value()
            ? std::optional<std::string> {gtopt::output_selection_to_string(
                  *opt.write_out)}
            : std::optional<std::string> {});
  }
};

}  // namespace daw::json
