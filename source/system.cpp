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
#include <spdlog/spdlog.h>

namespace
{

template<typename T>
constexpr void append_vector(std::vector<T>& a, std::vector<T>& b)
{
  a.insert(a.end(),
           std::make_move_iterator(b.begin()),
           std::make_move_iterator(b.end()));
}

/**
 * @brief Determines if the system needs a reference bus for voltage angle
 *
 * This function checks if the system requires setting a reference bus with a
 * fixed voltage angle (theta) for power flow calculations. A reference bus is
 * needed if:
 * - There are multiple buses
 * - Single-bus mode is not active
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
  if (buses.size() <= 1 || options.use_single_bus.value_or(false)
      || !options.use_kirchhoff.value_or(true))
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
  const auto kirchhoff_threshold = options.kirchhoff_threshold.value_or(0.0);
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

void System::setup_reference_bus(const Options& options)
{
  if (needs_ref_theta(bus_array, options)) {
    auto& bus = bus_array.front();
    bus.reference_theta = 0;
    const auto msg = std::format(
        "Setting bus '{}' as reference bus (reference_theta=0)", bus.name);
    SPDLOG_WARN(msg);
  }
}

}  // namespace gtopt
