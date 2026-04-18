/**
 * @file      json_cascade_options.hpp
 * @brief     JSON serialization for CascadeTransition, CascadeLevelMethod,
 *            CascadeLevel, and CascadeOptions
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/cascade_options.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_model_options.hpp>
#include <gtopt/json/json_sddp_options.hpp>

namespace daw::json
{
using gtopt::CascadeLevel;
using gtopt::CascadeLevelMethod;
using gtopt::CascadeOptions;
using gtopt::CascadeTransition;

template<>
struct json_data_contract<CascadeTransition>
{
  using type =
      json_member_list<json_number_null<"inherit_optimality_cuts", OptInt>,
                       json_number_null<"inherit_targets", OptInt>,
                       json_number_null<"target_rtol", OptReal>,
                       json_number_null<"target_min_atol", OptReal>,
                       json_number_null<"target_penalty", OptReal>,
                       json_number_null<"optimality_dual_threshold", OptReal>>;

  constexpr static auto to_json_data(CascadeTransition const& opt)
  {
    return std::forward_as_tuple(opt.inherit_optimality_cuts,
                                 opt.inherit_targets,
                                 opt.target_rtol,
                                 opt.target_min_atol,
                                 opt.target_penalty,
                                 opt.optimality_dual_threshold);
  }
};

template<>
struct json_data_contract<CascadeLevelMethod>
{
  using type = json_member_list<json_number_null<"max_iterations", OptInt>,
                                json_number_null<"min_iterations", OptInt>,
                                json_array_null<"apertures",
                                                std::optional<Array<Uid>>,
                                                json_number_no_name<Uid>>,
                                json_number_null<"convergence_tol", OptReal>>;

  constexpr static auto to_json_data(CascadeLevelMethod const& opt)
  {
    return std::forward_as_tuple(opt.max_iterations,
                                 opt.min_iterations,
                                 opt.apertures,
                                 opt.convergence_tol);
  }
};

template<>
struct json_data_contract<CascadeLevel>
{
  using type =
      json_member_list<json_number_null<"uid", OptUid>,
                       json_string_null<"name", OptName>,
                       json_class_null<"model_options", ModelOptions>,
                       json_class_null<"sddp_options", CascadeLevelMethod>,
                       json_class_null<"transition", CascadeTransition>>;

  constexpr static auto to_json_data(CascadeLevel const& opt)
  {
    return std::forward_as_tuple(
        opt.uid, opt.name, opt.model_options, opt.sddp_options, opt.transition);
  }
};

template<>
struct json_data_contract<CascadeOptions>
{
  using type = json_member_list<
      json_class_null<"model_options", ModelOptions>,
      json_class_null<"sddp_options", SddpOptions>,
      json_array_null<"level_array", gtopt::Array<CascadeLevel>, CascadeLevel>>;

  constexpr static auto to_json_data(CascadeOptions const& opt)
  {
    return std::forward_as_tuple(
        opt.model_options, opt.sddp_options, opt.level_array);
  }
};

}  // namespace daw::json
