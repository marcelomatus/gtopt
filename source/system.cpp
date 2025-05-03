/**
 * @file      system.cpp<gtopt>
 * @brief     Header of
 * @date      Sun Mar 30 16:04:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/options_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace
{

/**
 * @brief Determines if the system needs a reference bus for voltage angle
 *
 * This function checks if the system requires setting a reference bus with a
 * fixed voltage angle (theta) for power flow calculations. A reference bus is
 * needed if:
 * - There are multiple buses
 * - Single-bus mode is not activeLa reu
 * - Kirchhoff's laws are being used
 * - No bus has already been designated as a reference
 * - At least one bus needs Kirchhoff constraints based on threshold
 *
 * @tparam BusContainer Type of container holding buses
 * @tparam OptionsType Type of system options
 * @param buses Container of system buses
 * @param options System options
 * @return True if a reference bus needs to be set, false otherwise
 */
template<typename BusContainer, typename OptionsType>
constexpr bool needs_ref_theta(const BusContainer& buses,
                               const OptionsType& options)
{
  // Early return conditions
  if (buses.size() <= 1 || options.use_single_bus() || !options.use_kirchhoff())
  {
    return false;
  }

  // Check if any bus already has reference theta set
  const bool has_reference_bus =
      std::any_of(buses.begin(),
                  buses.end(),
                  [](const auto& bus) { return bus.reference_theta; });

  if (has_reference_bus) {
    return false;
  }

  // Check if any bus needs Kirchhoff according to the threshold
  const auto kirchhoff_threshold = options.kirchhoff_threshold();
  return std::any_of(buses.begin(),
                     buses.end(),
                     [kirchhoff_threshold](const auto& bus)
                     { return bus.needs_kirchhoff(kirchhoff_threshold); });
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

  gtopt::merge(bus_array, sys.bus_array);
  gtopt::merge(demand_array, sys.demand_array);
  gtopt::merge(generator_array, sys.generator_array);
  gtopt::merge(line_array, sys.line_array);
  gtopt::merge(generator_profile_array, sys.generator_profile_array);
  gtopt::merge(demand_profile_array, sys.demand_profile_array);
  gtopt::merge(battery_array, sys.battery_array);
  gtopt::merge(converter_array, sys.converter_array);
  gtopt::merge(reserve_zone_array, sys.reserve_zone_array);
  gtopt::merge(reserve_provision_array, sys.reserve_provision_array);

#ifdef NONE
  gtopt::merge(converters, sys.converters);
  gtopt::merge(junctions, sys.junctions);
  gtopt::merge(waterways, sys.waterways);
  gtopt::merge(inflows, sys.inflows);
  gtopt::merge(outflows, sys.outflows);
  gtopt::merge(reservoirs, sys.reservoirs);
  gtopt::merge(filtrations, sys.filtrations);
  gtopt::merge(turbines, sys.turbines);
  gtopt::merge(emission_zones, sys.emission_zones);
  gtopt::merge(generator_emissions, sys.generator_emissions);
  gtopt::merge(demand_emissions, sys.demand_emissions);
#endif

  return *this;
}

System& System::setup_reference_bus(const OptionsLP& options)
{
  if (needs_ref_theta(bus_array, options)) {
    auto& bus = bus_array.front();
    bus.reference_theta = 0;
    SPDLOG_INFO(std::format(
        "Setting bus '{}' as reference bus (reference_theta=0)", bus.name));
  }
  return *this;
}

}  // namespace gtopt
