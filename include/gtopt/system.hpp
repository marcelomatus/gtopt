/**
 * @file      system.hpp
 * @brief     Header of System class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the System class, which contains all the physical
 * elements of the power system: buses, generators, demands, lines, storage,
 * reserves, and hydro components.
 *
 * ### Element categories
 *
 * | Category | Elements |
 * |----------|---------|
 * | Electrical network | Bus, Generator, Demand, Line |
 * | Profiles | GeneratorProfile, DemandProfile |
 * | Energy storage | Battery, Converter |
 * | Reserve | ReserveZone, ReserveProvision |
 * | Hydro cascade | Junction, Waterway, Flow, Reservoir, Filtration, Turbine |
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
 * @brief Complete physical power system model
 *
 * Contains all component arrays that define the electrical network and
 * hydro cascade system. Multiple System objects can be merged with
 * `System::merge()`, allowing the system to be split across JSON files.
 */
struct System
{
  Name name {};      ///< System name (used in output filenames)
  String version {}; ///< Optional schema version string

  // ── Electrical network ──────────────────────────────────────────────────
  Array<Bus> bus_array {};             ///< Electrical buses (nodes)
  Array<Demand> demand_array {};       ///< Electrical demands (loads)
  Array<Generator> generator_array {}; ///< Generation units
  Array<Line> line_array {};           ///< Transmission lines

  // ── Time-varying profiles ───────────────────────────────────────────────
  Array<GeneratorProfile> generator_profile_array {}; ///< Capacity-factor profiles for generators
  Array<DemandProfile> demand_profile_array {};        ///< Load-shape profiles for demands

  // ── Energy storage ──────────────────────────────────────────────────────
  Array<Battery> battery_array {};     ///< Battery energy storage systems
  Array<Converter> converter_array {}; ///< Battery ↔ generator/demand couplings

  // ── Reserve modeling ────────────────────────────────────────────────────
  Array<ReserveZone> reserve_zone_array {};         ///< Spinning-reserve requirement zones
  Array<ReserveProvision> reserve_provision_array {};///< Generator → reserve zone links

  // ── Hydro cascade ───────────────────────────────────────────────────────
  Array<Junction> junction_array {};     ///< Hydraulic nodes
  Array<Waterway> waterway_array {};     ///< Water channels between junctions
  Array<Flow> flow_array {};             ///< Exogenous inflows / mandatory releases
  Array<Reservoir> reservoir_array {};   ///< Water storage reservoirs
  Array<Filtration> filtration_array {}; ///< Waterway → reservoir seepage links
  Array<Turbine> turbine_array {};       ///< Hydro turbines (waterway → generator)

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
