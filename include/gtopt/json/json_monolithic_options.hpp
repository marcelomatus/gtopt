/**
 * @file      json_monolithic_options.hpp
 * @brief     JSON serialization for MonolithicOptions
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_enum_option.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/monolithic_options.hpp>

namespace daw::json
{
using gtopt::BoundaryCutSharingMode;
using gtopt::BoundaryCutsMode;
using gtopt::MipStartEffort;
using gtopt::MipStartMethod;
using gtopt::MipStartOptions;
using gtopt::MonolithicOptions;
using gtopt::RelaxInfeasibleAction;
using gtopt::SolveMode;
using gtopt::SolverOptions;

/// Custom constructor: converts JSON strings → typed enums
struct MipStartOptionsConstructor
{
  [[nodiscard]] MipStartOptions operator()(
      OptName method_str,
      OptReal round_threshold,
      OptName effort_str,
      OptName file,
      OptBool relax_check,
      OptName on_infeasible_str,
      OptBool report_saturated,
      std::optional<SolverOptions> relax_solver_options) const
  {
    MipStartOptions opts;
    if (method_str) {
      opts.method = gtopt::require_enum<MipStartMethod>("method", *method_str);
    }
    opts.round_threshold = round_threshold;
    if (effort_str) {
      opts.effort = gtopt::require_enum<MipStartEffort>("effort", *effort_str);
    }
    opts.file = std::move(file);
    opts.relax_check = relax_check;
    if (on_infeasible_str) {
      opts.on_infeasible = gtopt::require_enum<RelaxInfeasibleAction>(
          "on_infeasible", *on_infeasible_str);
    }
    opts.report_saturated = report_saturated;
    opts.relax_solver_options = std::move(relax_solver_options);
    return opts;
  }
};

template<>
struct json_data_contract<MipStartOptions>
{
  using constructor_t = MipStartOptionsConstructor;

  using type =
      json_member_list<json_string_null<"method", OptName>,
                       json_number_null<"round_threshold", OptReal>,
                       json_string_null<"effort", OptName>,
                       json_string_null<"file", OptName>,
                       json_bool_null<"relax_check", OptBool>,
                       json_string_null<"on_infeasible", OptName>,
                       json_bool_null<"report_saturated", OptBool>,
                       json_class_null<"relax_solver_options", SolverOptions>>;

  static auto to_json_data(MipStartOptions const& opt)
  {
    return std::make_tuple(detail::enum_to_opt_name(opt.method),
                           opt.round_threshold,
                           detail::enum_to_opt_name(opt.effort),
                           opt.file,
                           opt.relax_check,
                           detail::enum_to_opt_name(opt.on_infeasible),
                           opt.report_saturated,
                           opt.relax_solver_options);
  }
};

/// Custom constructor: converts JSON strings → typed enums
struct MonolithicOptionsConstructor
{
  [[nodiscard]] MonolithicOptions operator()(
      OptName solve_mode_str,
      OptName boundary_cuts_file,
      OptName boundary_cuts_mode_str,
      OptName boundary_cut_sharing_mode_str,
      OptInt boundary_max_iterations,
      std::optional<SolverOptions> solver_options,
      std::optional<MipStartOptions> mip_start) const
  {
    MonolithicOptions opts;
    if (solve_mode_str) {
      opts.solve_mode =
          gtopt::require_enum<SolveMode>("solve_mode", *solve_mode_str);
    }
    opts.boundary_cuts_file = std::move(boundary_cuts_file);
    if (boundary_cuts_mode_str) {
      opts.boundary_cuts_mode = gtopt::require_enum<BoundaryCutsMode>(
          "boundary_cuts_mode", *boundary_cuts_mode_str);
    }
    if (boundary_cut_sharing_mode_str) {
      opts.boundary_cut_sharing_mode =
          gtopt::require_enum<BoundaryCutSharingMode>(
              "boundary_cut_sharing_mode", *boundary_cut_sharing_mode_str);
    }
    opts.boundary_max_iterations = boundary_max_iterations;
    opts.solver_options = std::move(solver_options);
    opts.mip_start = std::move(mip_start);
    return opts;
  }
};

template<>
struct json_data_contract<MonolithicOptions>
{
  using constructor_t = MonolithicOptionsConstructor;

  using type =
      json_member_list<json_string_null<"solve_mode", OptName>,
                       json_string_null<"boundary_cuts_file", OptName>,
                       json_string_null<"boundary_cuts_mode", OptName>,
                       json_string_null<"boundary_cut_sharing_mode", OptName>,
                       json_number_null<"boundary_max_iterations", OptInt>,
                       json_class_null<"solver_options", SolverOptions>,
                       json_class_null<"mip_start", MipStartOptions>>;

  static auto to_json_data(MonolithicOptions const& opt)
  {
    return std::make_tuple(
        detail::enum_to_opt_name(opt.solve_mode),
        opt.boundary_cuts_file,
        detail::enum_to_opt_name(opt.boundary_cuts_mode),
        detail::enum_to_opt_name(opt.boundary_cut_sharing_mode),
        opt.boundary_max_iterations,
        opt.solver_options,
        opt.mip_start);
  }
};

}  // namespace daw::json
