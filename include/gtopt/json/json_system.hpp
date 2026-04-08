/**
 * @file      json_system.hpp
 * @brief     JSON serialization/deserialization for the System class
 * @date      Wed Mar 19 22:42:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides JSON data contract definitions for the System class,
 * enabling serialization and deserialization of system objects with all their
 * components including buses, generators, lines, etc.
 */

#pragma once

#include <gtopt/json/json_battery.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/json/json_commitment.hpp>
#include <gtopt/json/json_converter.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_demand_profile.hpp>
#include <gtopt/json/json_flow.hpp>
#include <gtopt/json/json_flow_right.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_generator_profile.hpp>
#include <gtopt/json/json_junction.hpp>
#include <gtopt/json/json_line.hpp>
#include <gtopt/json/json_reserve_provision.hpp>
#include <gtopt/json/json_reserve_zone.hpp>
#include <gtopt/json/json_reservoir.hpp>
#include <gtopt/json/json_reservoir_discharge_limit.hpp>
#include <gtopt/json/json_reservoir_production_factor.hpp>
#include <gtopt/json/json_reservoir_seepage.hpp>
#include <gtopt/json/json_turbine.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/json/json_user_param.hpp>
#include <gtopt/json/json_volume_right.hpp>
#include <gtopt/json/json_waterway.hpp>
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
                      ReserveProvision>,
      json_array_null<"commitment_array", Array<Commitment>, Commitment>,
      json_array_null<"junction_array", Array<Junction>, Junction>,
      json_array_null<"waterway_array", Array<Waterway>, Waterway>,
      json_array_null<"flow_array", Array<Flow>, Flow>,
      json_array_null<"reservoir_array", Array<Reservoir>, Reservoir>,
      json_array_null<"reservoir_seepage_array",
                      Array<ReservoirSeepage>,
                      ReservoirSeepage>,
      json_array_null<"reservoir_discharge_limit_array",
                      Array<ReservoirDischargeLimit>,
                      ReservoirDischargeLimit>,
      json_array_null<"turbine_array", Array<Turbine>, Turbine>,
      json_array_null<"reservoir_production_factor_array",
                      Array<ReservoirProductionFactor>,
                      ReservoirProductionFactor>,
      json_array_null<"flow_right_array", Array<FlowRight>, FlowRight>,
      json_array_null<"volume_right_array", Array<VolumeRight>, VolumeRight>,
      json_array_null<"user_param_array", Array<UserParam>, UserParam>,
      json_array_null<"user_constraint_array",
                      Array<UserConstraint>,
                      UserConstraint>,
      json_string_null<"user_constraint_file", OptName>,
      json_array_null<"user_constraint_files", std::vector<Name>, std::string>>;

  [[nodiscard]] constexpr static auto to_json_data(System const& system)
  {
    return std::forward_as_tuple(system.name,
                                 system.version,
                                 system.bus_array,
                                 system.demand_array,
                                 system.generator_array,
                                 system.line_array,
                                 system.generator_profile_array,
                                 system.demand_profile_array,
                                 system.battery_array,
                                 system.converter_array,
                                 system.reserve_zone_array,
                                 system.reserve_provision_array,
                                 system.commitment_array,
                                 system.junction_array,
                                 system.waterway_array,
                                 system.flow_array,
                                 system.reservoir_array,
                                 system.reservoir_seepage_array,
                                 system.reservoir_discharge_limit_array,
                                 system.turbine_array,
                                 system.reservoir_production_factor_array,
                                 system.flow_right_array,
                                 system.volume_right_array,
                                 system.user_param_array,
                                 system.user_constraint_array,
                                 system.user_constraint_file,
                                 system.user_constraint_files);
  }
};
}  // namespace daw::json
