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
using gtopt::BoundaryCutsMode;
using gtopt::MonolithicOptions;
using gtopt::SolveMode;
using gtopt::SolverOptions;

/// Custom constructor: converts JSON strings → typed enums
struct MonolithicOptionsConstructor
{
  [[nodiscard]] MonolithicOptions operator()(
      OptName solve_mode_str,
      OptName boundary_cuts_file,
      OptName boundary_cuts_mode_str,
      OptInt boundary_max_iterations,
      std::optional<SolverOptions> solver_options) const
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
    opts.boundary_max_iterations = boundary_max_iterations;
    opts.solver_options = solver_options;
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
                       json_number_null<"boundary_max_iterations", OptInt>,
                       json_class_null<"solver_options", SolverOptions>>;

  static auto to_json_data(MonolithicOptions const& opt)
  {
    return std::make_tuple(detail::enum_to_opt_name(opt.solve_mode),
                           opt.boundary_cuts_file,
                           detail::enum_to_opt_name(opt.boundary_cuts_mode),
                           opt.boundary_max_iterations,
                           opt.solver_options);
  }
};

}  // namespace daw::json
