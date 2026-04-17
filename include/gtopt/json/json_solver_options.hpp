/**
 * @file      json_solver_options.hpp
 * @brief     JSON serialization for LP solver options
 * @date      Fri May  3 12:30:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module provides JSON serialization/deserialization for the SolverOptions
 * structure, allowing LP solver options to be saved and loaded in JSON format.
 *
 * All fields are nullable in JSON (missing ≡ use C++ default).  A custom
 * constructor_t converts the std::optional parse results back to non-optional
 * SolverOptions members via value_or().
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/solver_options.hpp>

namespace daw::json
{
using gtopt::LPAlgo;
using gtopt::OptReal;
using gtopt::SolverLogMode;
using gtopt::SolverOptions;
using gtopt::SolverScaling;

using OptLPAlgo = std::optional<LPAlgo>;
using OptSolverLogMode = std::optional<SolverLogMode>;
using OptSolverScaling = std::optional<SolverScaling>;
using OptInt = std::optional<int>;
using OptBool = std::optional<bool>;

/// Constructs SolverOptions from nullable JSON fields, applying defaults.
struct SolverOptionsConstructor
{
  SolverOptions operator()(OptLPAlgo algorithm,
                           OptInt threads,
                           OptBool presolve,
                           OptReal optimal_eps,
                           OptReal feasible_eps,
                           OptReal barrier_eps,
                           OptInt log_level,
                           OptSolverLogMode log_mode,
                           OptReal time_limit,
                           OptSolverScaling scaling,
                           OptInt max_fallbacks) const
  {
    return SolverOptions {
        .algorithm = algorithm.value_or(LPAlgo::barrier),
        .threads = threads.value_or(2),
        .presolve = presolve.value_or(true),
        .optimal_eps = optimal_eps,
        .feasible_eps = feasible_eps,
        .barrier_eps = barrier_eps,
        .log_level = log_level.value_or(0),
        .log_mode = log_mode,
        .time_limit = time_limit,
        .scaling = scaling.has_value()
            ? scaling
            : OptSolverScaling {SolverScaling::automatic},
        .max_fallbacks = max_fallbacks.value_or(2),
    };
  }
};

template<>
struct json_data_contract<SolverOptions>
{
  using constructor_t = SolverOptionsConstructor;

  using type = json_member_list<json_number_null<"algorithm", OptLPAlgo>,
                                json_number_null<"threads", OptInt>,
                                json_bool_null<"presolve", OptBool>,
                                json_number_null<"optimal_eps", OptReal>,
                                json_number_null<"feasible_eps", OptReal>,
                                json_number_null<"barrier_eps", OptReal>,
                                json_number_null<"log_level", OptInt>,
                                json_number_null<"log_mode", OptSolverLogMode>,
                                json_number_null<"time_limit", OptReal>,
                                json_number_null<"scaling", OptSolverScaling>,
                                json_number_null<"max_fallbacks", OptInt>>;

  static constexpr auto to_json_data(SolverOptions const& opt)
  {
    return std::make_tuple(OptLPAlgo {opt.algorithm},
                           OptInt {opt.threads},
                           OptBool {opt.presolve},
                           opt.optimal_eps,
                           opt.feasible_eps,
                           opt.barrier_eps,
                           OptInt {opt.log_level},
                           opt.log_mode,
                           opt.time_limit,
                           opt.scaling,
                           OptInt {opt.max_fallbacks});
  }
};

}  // namespace daw::json
