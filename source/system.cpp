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

System& System::setup_reference_bus(const OptionsLP& options)
{
  if (needs_ref_theta(bus_array, options)) {
    auto& bus = bus_array.front();
    bus.reference_theta = 0;
    SPDLOG_INFO(fmt::format(
        "Setting bus '{}' as reference bus (reference_theta=0)", bus.name));
  }
  return *this;
}

}  // namespace gtopt
