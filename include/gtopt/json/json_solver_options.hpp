/**
 * @file      json_solver_options.hpp
 * @brief     JSON serialization for LP solver options
 * @date      Fri May  3 12:30:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module provides JSON serialization/deserialization for the SolverOptions
 * structure, allowing LP solver options to be saved and loaded in JSON format.
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/solver_options.hpp>

namespace daw::json
{
using gtopt::LPAlgo;
using gtopt::SolverOptions;

template<>
struct json_data_contract<SolverOptions>
{
  using type = json_member_list<json_number<"algorithm", int>,
                                json_number<"threads", int>,
                                json_bool<"presolve", bool>,
                                json_number<"optimal_eps", double>,
                                json_number<"feasible_eps", double>,
                                json_number<"barrier_eps", double>,
                                json_number<"log_level", int>>;

  constexpr static auto to_json_data(SolverOptions const& opt)
  {
    return std::forward_as_tuple(opt.algorithm,
                                 opt.threads,
                                 opt.presolve,
                                 opt.optimal_eps,
                                 opt.feasible_eps,
                                 opt.barrier_eps,
                                 opt.log_level);
  }
};

}  // namespace daw::json
