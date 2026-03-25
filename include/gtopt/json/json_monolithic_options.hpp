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
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/monolithic_options.hpp>

namespace daw::json
{
using gtopt::MonolithicOptions;
using gtopt::SolverOptions;

template<>
struct json_data_contract<MonolithicOptions>
{
  using type =
      json_member_list<json_string_null<"solve_mode", OptName>,
                       json_string_null<"boundary_cuts_file", OptName>,
                       json_string_null<"boundary_cuts_mode", OptName>,
                       json_number_null<"boundary_max_iterations", OptInt>,
                       json_class_null<"solver_options", SolverOptions>>;

  constexpr static auto to_json_data(MonolithicOptions const& opt)
  {
    return std::forward_as_tuple(opt.solve_mode,
                                 opt.boundary_cuts_file,
                                 opt.boundary_cuts_mode,
                                 opt.boundary_max_iterations,
                                 opt.solver_options);
  }
};

}  // namespace daw::json
