/**
 * @file      json_model_options.hpp
 * @brief     JSON serialization for ModelOptions
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/model_options.hpp>

namespace daw::json
{
using gtopt::ModelOptions;

template<>
struct json_data_contract<ModelOptions>
{
  using type =
      json_member_list<json_bool_null<"use_single_bus", OptBool>,
                       json_bool_null<"use_kirchhoff", OptBool>,
                       json_bool_null<"use_line_losses", OptBool>,
                       json_number_null<"kirchhoff_threshold", OptReal>,
                       json_number_null<"loss_segments", OptInt>,
                       json_number_null<"scale_objective", OptReal>,
                       json_number_null<"scale_theta", OptReal>,
                       json_number_null<"demand_fail_cost", OptReal>,
                       json_number_null<"reserve_fail_cost", OptReal>,
                       json_number_null<"hydro_fail_cost", OptReal>,
                       json_number_null<"hydro_use_value", OptReal>,
                       json_number_null<"annual_discount_rate", OptReal>>;

  constexpr static auto to_json_data(ModelOptions const& opt)
  {
    return std::forward_as_tuple(opt.use_single_bus,
                                 opt.use_kirchhoff,
                                 opt.use_line_losses,
                                 opt.kirchhoff_threshold,
                                 opt.loss_segments,
                                 opt.scale_objective,
                                 opt.scale_theta,
                                 opt.demand_fail_cost,
                                 opt.reserve_fail_cost,
                                 opt.hydro_fail_cost,
                                 opt.hydro_use_value,
                                 opt.annual_discount_rate);
  }
};

}  // namespace daw::json
