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

#include <gtopt/json/json_battery.hpp>
#include <gtopt/json/json_block.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/json/json_converter.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_demand_profile.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_generator_profile.hpp>
#include <gtopt/json/json_line.hpp>
#include <gtopt/json/json_reserve_provision.hpp>
#include <gtopt/json/json_reserve_zone.hpp>
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
      json_array_null<"line_array", Array<Line>, Line>,
      json_array_null<"generator_profile_array",
                      Array<GeneratorProfile>,
                      GeneratorProfile>,
      json_array_null<"demand_profile_array",
                      Array<DemandProfile>,
                      DemandProfile>,
      json_array_null<"battery_array", Array<Battery>, Battery>,
      json_array_null<"converter_array", Array<Converter>, Converter>,
      json_array_null<"reserve_zone_array", Array<ReserveZone>, ReserveZone>,
      json_array_null<"reserve_provision_array",
                      Array<ReserveProvision>,
                      ReserveProvision> >;

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
                                 system.line_array,
                                 system.generator_profile_array,
                                 system.demand_profile_array,
                                 system.battery_array,
                                 system.converter_array,
                                 system.reserve_zone_array,
                                 system.reserve_provision_array);
  }
};

}  // namespace daw::json
