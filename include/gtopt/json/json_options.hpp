/**
 * @file      json_options.hpp
 * @brief     Header of
 * @date      Sun Apr 20 16:01:20 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/options.hpp>

namespace daw::json
{
using gtopt::Options;

template<>
struct json_data_contract<Options>
{
  using type =
      json_member_list<json_string_null<"input_directory", OptName>,
                       json_string_null<"input_format", OptName>,
                       json_number_null<"demand_fail_cost", OptReal>,
                       json_number_null<"reserve_fail_cost", OptReal>,
                       json_bool_null<"use_line_losses", OptBool>,
                       json_bool_null<"use_kirchhoff", OptBool>,
                       json_bool_null<"use_single_bus", OptBool>,
                       json_number_null<"kirchhoff_threshold", OptReal>,
                       json_number_null<"scale_objective", OptReal>,
                       json_number_null<"scale_theta", OptReal>,

                       json_string_null<"output_directory", OptName>,
                       json_string_null<"output_format", OptName>,
                       json_string_null<"output_compression", OptName>,
                       json_bool_null<"use_lp_names", OptBool>,
                       json_bool_null<"use_uid_fname", OptBool>,
                       json_number_null<"annual_discount_rate", OptReal> >;

  constexpr static auto to_json_data(Options const& opt)
  {
    return std::forward_as_tuple(opt.input_directory,
                                 opt.input_format,
                                 opt.demand_fail_cost,
                                 opt.reserve_fail_cost,
                                 opt.use_line_losses,
                                 opt.use_kirchhoff,
                                 opt.use_single_bus,
                                 opt.kirchhoff_threshold,
                                 opt.scale_objective,
                                 opt.scale_theta,

                                 opt.output_directory,
                                 opt.output_format,
                                 opt.output_compression,
                                 opt.use_lp_names,
                                 opt.use_uid_fname,
                                 opt.annual_discount_rate);
  }
};

}  // namespace daw::json
