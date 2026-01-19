/**
 * @file      system.hpp
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
#include <gtopt/filtration.hpp>
#include <gtopt/flow.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/generator_profile.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/line.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/turbine.hpp>
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
  Array<Reservoir> reservoir_array {};
  Array<Filtration> filtration_array {};
  Array<Turbine> turbine_array {};

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

  void merge(System&& sys);

  void setup_reference_bus(const class OptionsLP& options);
};

}  // namespace gtopt
