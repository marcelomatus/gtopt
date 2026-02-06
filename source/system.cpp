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

void System::setup_reference_bus(const OptionsLP& options)
{
  if (needs_ref_theta(bus_array, options)) {
    auto& bus = bus_array.front();
    bus.reference_theta = 0;
    SPDLOG_INFO(std::format(
        "Setting bus '{}' as reference bus (reference_theta=0)", bus.name));
  }
}

void System::merge(System&& sys)  // NOLINT
{
  if (!sys.name.empty()) {
    name = std::move(sys.name);
  }

  if (!sys.version.empty()) {
    version = std::move(sys.version);
  }

  gtopt::merge(bus_array, std::move(sys.bus_array));
  gtopt::merge(demand_array, std::move(sys.demand_array));
  gtopt::merge(generator_array, std::move(sys.generator_array));
  gtopt::merge(line_array, std::move(sys.line_array));

  gtopt::merge(generator_profile_array, std::move(sys.generator_profile_array));
  gtopt::merge(demand_profile_array, std::move(sys.demand_profile_array));

  gtopt::merge(battery_array, std::move(sys.battery_array));
  gtopt::merge(converter_array, std::move(sys.converter_array));

  gtopt::merge(reserve_zone_array, std::move(sys.reserve_zone_array));
  gtopt::merge(reserve_provision_array, std::move(sys.reserve_provision_array));

  gtopt::merge(junction_array, std::move(sys.junction_array));
  gtopt::merge(waterway_array, std::move(sys.waterway_array));
  gtopt::merge(flow_array, std::move(sys.flow_array));
  gtopt::merge(reservoir_array, std::move(sys.reservoir_array));
  gtopt::merge(filtration_array, std::move(sys.filtration_array));
  gtopt::merge(turbine_array, std::move(sys.turbine_array));
}

}  // namespace gtopt
