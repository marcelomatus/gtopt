/**c
 * @file      system.hpp<gtopt>
 * @brief     Header of System class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the System class, which contains all the system elements.
 */

#pragma once

#include <gtopt/battery.hpp>
#include <gtopt/bus.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/demand_profile.hpp>
#include <gtopt/flow.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/generator_profile.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/line.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/waterway.hpp>

namespace gtopt
{

/**
 * @brief Represents a complete power system model
 */
struct System
{
  Name name {};
  String version {};

  Array<Bus> bus_array {};
  Array<Demand> demand_array {};
  Array<Generator> generator_array {};
  Array<Line> line_array {};

  Array<GeneratorProfile> generator_profile_array {};
  Array<DemandProfile> demand_profile_array {};

  Array<Battery> battery_array {};
  Array<Converter> converter_array {};

  Array<ReserveZone> reserve_zone_array {};
  Array<ReserveProvision> reserve_provision_array {};

  Array<Junction> junction_array {};
  Array<Waterway> waterway_array {};
  Array<Flow> flow_array {};

  /**
   * @brief Merges another system into this one
   *
   * This is a unified template method that handles both lvalue and rvalue
   * references. When merging from an rvalue reference, move semantics are used
   * automatically.
   *
   * @tparam T System reference type (can be lvalue or rvalue reference)
   * @param sys The system to merge from (will be moved from if it's an rvalue)
   * @return Reference to this system after merge
   */
  template<typename T>
  constexpr System& merge(T&& sys)
  {
    if (!sys.name.empty()) {
      name = std::forward<T>(sys).name;
    }

    if (!sys.version.empty()) {
      version = std::forward<T>(sys).version;
    }

    // Using std::forward to preserve value category (lvalue vs rvalue)
    gtopt::merge(bus_array, std::forward<T>(sys).bus_array);
    gtopt::merge(demand_array, std::forward<T>(sys).demand_array);
    gtopt::merge(generator_array, std::forward<T>(sys).generator_array);
    gtopt::merge(line_array, std::forward<T>(sys).line_array);
    gtopt::merge(generator_profile_array,
                 std::forward<T>(sys).generator_profile_array);
    gtopt::merge(demand_profile_array,
                 std::forward<T>(sys).demand_profile_array);
    gtopt::merge(battery_array, std::forward<T>(sys).battery_array);
    gtopt::merge(converter_array, std::forward<T>(sys).converter_array);
    gtopt::merge(reserve_zone_array, std::forward<T>(sys).reserve_zone_array);
    gtopt::merge(reserve_provision_array,
                 std::forward<T>(sys).reserve_provision_array);

    gtopt::merge(junction_array, std::forward<T>(sys).junction_array);
    gtopt::merge(waterway_array, std::forward<T>(sys).waterway_array);
    gtopt::merge(flow_array, std::forward<T>(sys).flow_array);

    return *this;
  }

  System& setup_reference_bus(const class OptionsLP& options);
};

}  // namespace gtopt
