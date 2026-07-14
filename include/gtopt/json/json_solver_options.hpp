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
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/solver_options.hpp>

namespace daw::json
{
using gtopt::CrossoverMode;
using gtopt::LPAlgo;
using gtopt::OptName;
using gtopt::OptReal;
using gtopt::SolverLogMode;
using gtopt::SolverOptions;
using gtopt::SolverScaling;

using OptSolverLogMode = std::optional<SolverLogMode>;
using OptSolverScaling = std::optional<SolverScaling>;
using OptInt = std::optional<int>;
using OptBool = std::optional<bool>;

/// Constructs SolverOptions from nullable JSON fields, applying defaults.
struct SolverOptionsConstructor
{
  SolverOptions operator()(const OptName& algorithm_str,
                           OptInt threads,
                           OptBool presolve,
                           OptReal optimal_eps,
                           OptReal feasible_eps,
                           OptReal barrier_eps,
                           OptReal mip_gap,
                           OptReal mip_gap_abs,
                           OptInt log_level,
                           OptSolverLogMode log_mode,
                           OptReal time_limit,
                           OptSolverScaling scaling,
                           const OptName& crossover_str,
                           OptInt max_fallbacks,
                           OptBool memory_emphasis,
                           const OptName& param_file,
                           OptBool advanced_basis) const
  {
    // Defaults MUST equal the SolverOptions struct defaults (default_algo /
    // threads 0 / scaling unset), NOT a tuned solve config.  These sentinels
    // are what overlay() and apply_sddp_pass_defaults() key on: an injected
    // non-sentinel (e.g. barrier / threads 2 / scaling automatic) would
    // masquerade as a user choice and override a .prm or the per-method
    // policy.  The "SDDP wants barrier / threads 1 / scaling auto" policy is
    // owned by apply_sddp_pass_defaults(); the backend / .prm owns it for the
    // monolithic solve.
    LPAlgo algorithm = LPAlgo::default_algo;
    if (algorithm_str.has_value()) {
      algorithm = gtopt::require_enum<LPAlgo>("algorithm", *algorithm_str);
    }
    CrossoverMode crossover = CrossoverMode::automatic;
    if (crossover_str.has_value()) {
      crossover =
          gtopt::require_enum<CrossoverMode>("crossover", *crossover_str);
    }
    return SolverOptions {
        .algorithm = algorithm,
        .threads = threads.value_or(0),
        .presolve = presolve.value_or(true),
        .optimal_eps = optimal_eps,
        .feasible_eps = feasible_eps,
        .barrier_eps = barrier_eps,
        .mip_gap = mip_gap,
        .mip_gap_abs = mip_gap_abs,
        .log_level = log_level.value_or(0),
        .log_mode = log_mode,
        .time_limit = time_limit,
        .scaling = scaling,
        .crossover = crossover,
        .advanced_basis = advanced_basis.value_or(false),
        .param_file = param_file,
        .max_fallbacks = max_fallbacks.value_or(2),
        .memory_emphasis = memory_emphasis,
    };
  }
};

template<>
struct json_data_contract<SolverOptions>
{
  using constructor_t = SolverOptionsConstructor;

  using type = json_member_list<json_string_null<"algorithm", OptName>,
                                json_number_null<"threads", OptInt>,
                                json_bool_null<"presolve", OptBool>,
                                json_number_null<"optimal_eps", OptReal>,
                                json_number_null<"feasible_eps", OptReal>,
                                json_number_null<"barrier_eps", OptReal>,
                                json_number_null<"mip_gap", OptReal>,
                                json_number_null<"mip_gap_abs", OptReal>,
                                json_number_null<"log_level", OptInt>,
                                json_number_null<"log_mode", OptSolverLogMode>,
                                json_number_null<"time_limit", OptReal>,
                                json_number_null<"scaling", OptSolverScaling>,
                                json_string_null<"crossover", OptName>,
                                json_number_null<"max_fallbacks", OptInt>,
                                json_bool_null<"memory_emphasis", OptBool>,
                                json_string_null<"param_file", OptName>,
                                json_bool_null<"advanced_basis", OptBool>>;

  static constexpr auto to_json_data(SolverOptions const& opt)
  {
    return std::make_tuple(
        OptName {std::string {gtopt::enum_name(opt.algorithm)}},
        OptInt {opt.threads},
        OptBool {opt.presolve},
        opt.optimal_eps,
        opt.feasible_eps,
        opt.barrier_eps,
        opt.mip_gap,
        opt.mip_gap_abs,
        OptInt {opt.log_level},
        opt.log_mode,
        opt.time_limit,
        opt.scaling,
        OptName {gtopt::enum_name(opt.crossover)},
        OptInt {opt.max_fallbacks},
        opt.memory_emphasis,
        opt.param_file,
        OptBool {opt.advanced_basis});
  }
};

}  // namespace daw::json
