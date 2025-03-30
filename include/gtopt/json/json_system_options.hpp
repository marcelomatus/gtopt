#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_line.hpp>
#include <gtopt/system_options.hpp>

namespace daw::json
{
using gtopt::SystemOptions;

template<typename Object>
using Array = gtopt::Array<Object>;

template<>
struct json_data_contract<SystemOptions>
{
  using type =
      json_member_list<json_string_null<"input_directory", OptName>,
                       json_string_null<"input_format", OptName>,
                       json_string_null<"output_directory", OptName>,
                       json_string_null<"output_format", OptName>,
                       json_string_null<"compression_format", OptName>,
                       json_number_null<"annual_discount_rate", OptReal>,
                       json_number_null<"demand_fail_cost", OptReal>,
                       json_number_null<"reserve_fail_cost", OptReal>,
                       json_bool_null<"use_line_losses", OptBool>,
                       json_bool_null<"use_kirchhoff", OptBool>,
                       json_bool_null<"use_single_bus", OptBool>,
                       json_number_null<"kirchhoff_threshold", OptReal>,
                       json_number_null<"scale_objective", OptReal>,
                       json_number_null<"scale_theta", OptReal>,
                       json_bool_null<"use_lp_names", OptBool>,
                       json_bool_null<"use_uid_fname ", OptBool>>;

  constexpr static auto to_json_data(SystemOptions const& opt)
  {
    return std::forward_as_tuple(opt.input_directory,
                                 opt.input_format,
                                 opt.output_directory,
                                 opt.output_format,
                                 opt.compression_format,
                                 opt.annual_discount_rate,
                                 opt.demand_fail_cost,
                                 opt.reserve_fail_cost,
                                 opt.use_line_losses,
                                 opt.use_kirchhoff,
                                 opt.use_single_bus,
                                 opt.kirchhoff_threshold,
                                 opt.scale_objective,
                                 opt.scale_theta,
                                 opt.use_lp_names,
                                 opt.use_uid_fname);
  }
};

}  // namespace daw::json
