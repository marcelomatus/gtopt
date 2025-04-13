/**
 * @file      system.cpp<gtopt>
 * @brief     Header of
 * @date      Sun Mar 30 16:04:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/system.hpp>

namespace
{

template<typename T>
constexpr void append_vector(std::vector<T>& a, std::vector<T>& b)
{
  a.insert(a.end(),
           std::make_move_iterator(b.begin()),
           std::make_move_iterator(b.end()));
}
}  // namespace

namespace gtopt
{

System& System::merge(System& sys)
{
  if (!sys.name.empty()) {
    name = std::move(sys.name);
  }

  if (!sys.version.empty()) {
    version = std::move(sys.version);
  }

  options.merge(sys.options);

  append_vector(block_array, (sys.block_array));
  append_vector(stage_array, (sys.stage_array));
  append_vector(scenario_array, (sys.scenario_array));

  append_vector(bus_array, (sys.bus_array));
  append_vector(demand_array, (sys.demand_array));
  append_vector(generator_array, (sys.generator_array));
  append_vector(line_array, (sys.line_array));
  append_vector(generator_profile_array, (sys.generator_profile_array));
  append_vector(demand_profile_array, (sys.demand_profile_array));
  append_vector(battery_array, (sys.battery_array));
  append_vector(converter_array, (sys.converter_array));
  append_vector(reserve_zone_array, (sys.reserve_zone_array));
  append_vector(reserve_provision_array, (sys.reserve_provision_array));

#ifdef NONE
  append_vector(converters, (sys.converters));
  append_vector(junctions, (sys.junctions));
  append_vector(waterways, (sys.waterways));
  append_vector(inflows, (sys.inflows));
  append_vector(outflows, (sys.outflows));
  append_vector(reservoirs, (sys.reservoirs));
  append_vector(filtrations, (sys.filtrations));
  append_vector(turbines, (sys.turbines));
  append_vector(emission_zones, (sys.emission_zones));
  append_vector(generator_emissions, (sys.generator_emissions));
  append_vector(demand_emissions, (sys.demand_emissions));
#endif

  return *this;
}

}  // namespace gtopt
