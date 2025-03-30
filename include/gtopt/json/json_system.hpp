/**
 * @file      json_system.hpp
 * @brief     Header of
 * @date      Wed Mar 19 22:42:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_block.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_line.hpp>
#include <gtopt/json/json_scenery.hpp>
#include <gtopt/json/json_stage.hpp>
#include <gtopt/json/json_system_options.hpp>
#include <gtopt/system.hpp>

namespace daw::json
{

using gtopt::System;

template<>
struct json_data_contract<System>
{
  using type = json_member_list<
      json_string_null<"name", Name>,
      json_string_null<"version", Name>,
      json_class_null<"options", SystemOptions>,
      json_array_null<"block_array", Array<Block>, Block>,
      json_array_null<"stage_array", Array<Stage>, Stage>,
      json_array_null<"scenery_array", Array<Scenery>, Scenery>,
      json_array_null<"bus_array", Array<Bus>, Bus>,
      json_array_null<"demand_array", Array<Demand>, Demand>,
      json_array_null<"generator_array", Array<Generator>, Generator>,
      json_array_null<"line_array", Array<Line>, Line> >;

  constexpr static auto to_json_data(System const& system)
  {
    return std::forward_as_tuple(system.name,
                                 system.version,
                                 system.options,
                                 system.block_array,
                                 system.stage_array,
                                 system.scenery_array,
                                 system.bus_array,
                                 system.demand_array,
                                 system.generator_array,
                                 system.line_array);
  }
};

}  // namespace daw::json
