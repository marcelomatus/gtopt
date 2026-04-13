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
 * | Hydro cascade | Junction, Waterway, Flow, Reservoir, ReservoirSeepage,
 * ReservoirDischargeLimit, Turbine |
 */

#pragma once

#include <gtopt/battery.hpp>
#include <gtopt/bus.hpp>
#include <gtopt/commitment.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/demand_profile.hpp>
#include <gtopt/dispatch_commitment.hpp>
#include <gtopt/flow.hpp>
#include <gtopt/flow_right.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/generator_profile.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/line.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/reservoir_discharge_limit.hpp>
#include <gtopt/reservoir_production_factor.hpp>
#include <gtopt/reservoir_seepage.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/user_constraint.hpp>
#include <gtopt/user_param.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/volume_right.hpp>
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
  Name name {};  ///< System name (used in output filenames)
  String version {};  ///< Optional schema version string

  // ── Electrical network ──────────────────────────────────────────────────
  Array<Bus> bus_array {};  ///< Electrical buses (nodes)
  Array<Demand> demand_array {};  ///< Electrical demands (loads)
  Array<Generator> generator_array {};  ///< Generation units
  Array<Line> line_array {};  ///< Transmission lines

  // ── Time-varying profiles ───────────────────────────────────────────────
  Array<GeneratorProfile>
      generator_profile_array {};  ///< Capacity-factor profiles for generators
  Array<DemandProfile>
      demand_profile_array {};  ///< Load-shape profiles for demands

  // ── Energy storage ──────────────────────────────────────────────────────
  Array<Battery> battery_array {};  ///< Battery energy storage systems
  Array<Converter>
      converter_array {};  ///< Battery ↔ generator/demand couplings

  // ── Reserve modeling ────────────────────────────────────────────────────
  Array<ReserveZone>
      reserve_zone_array {};  ///< Spinning-reserve requirement zones
  Array<ReserveProvision>
      reserve_provision_array {};  ///< Generator → reserve zone links

  // ── Unit commitment ────────────────────────────────────────────────────
  Array<Commitment>
      commitment_array {};  ///< Generator unit commitment parameters
  Array<DispatchCommitment>
      dispatch_commitment_array {};  ///< Simplified dispatch commitments

  // ── Hydro cascade ───────────────────────────────────────────────────────
  Array<Junction> junction_array {};  ///< Hydraulic nodes
  Array<Waterway> waterway_array {};  ///< Water channels between junctions
  Array<Flow> flow_array {};  ///< Exogenous inflows / mandatory releases
  Array<Reservoir> reservoir_array {};  ///< Water storage reservoirs
  Array<ReservoirSeepage>
      reservoir_seepage_array {};  ///< Waterway → reservoir seepage links
  Array<ReservoirDischargeLimit>
      reservoir_discharge_limit_array {};  ///< Volume-dependent discharge
                                           ///< limits
  Array<Turbine> turbine_array {};  ///< Hydro turbines (waterway → generator)
  Array<ReservoirProductionFactor>
      reservoir_production_factor_array {};  ///< Volume-dependent turbine
                                             ///< efficiency

  // ── Water rights (NOT part of hydro topology) ───────────────────────────
  Array<FlowRight> flow_right_array {};  ///< Flow-based water rights (m³/s)
  Array<VolumeRight>
      volume_right_array {};  ///< Volume-based water rights (hm³)

  // ── User parameters and constraints ─────────────────────────────────────
  Array<UserParam> user_param_array {};  ///< Named parameters for constraints
  Array<UserConstraint>
      user_constraint_array {};  ///< User-defined LP constraints
  OptName
      user_constraint_file {};  ///< External JSON/PAMPL file with constraints
  std::vector<Name>
      user_constraint_files {};  ///< Multiple external constraint files

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

  /**
   * @brief Expands unified battery definitions into separate elements
   *
   * For each Battery that has the optional `bus` field set, this method
   * auto-generates:
   * - A Generator for the discharge path (name = battery.name + "_gen")
   * - A Demand for the charge path (name = battery.name + "_dem")
   * - A Converter linking battery, generator, and demand
   *   (name = battery.name + "_conv")
   *
   * ### Standalone battery (bus only)
   * Both the discharge Generator and charge Demand are connected to the
   * external `bus`. This is the default mode.
   *
   * ### Generation-coupled battery (bus + source_generator)
   * When `source_generator` is also set, an internal bus is created
   * (name = battery.name + "_int_bus"):
   * - The discharge Generator connects to the external `bus`
   * - The charge Demand connects to the internal bus
   * - The source generator's `bus` is set to the internal bus
   *
   * After expansion, both `bus` and `source_generator` fields on the battery
   * are cleared so that re-expansion is idempotent.
   *
   * This is called automatically by PlanningLP before LP construction.
   */
  void expand_batteries();

  /**
   * @brief Extracts inline reservoir constraints into flat system arrays
   *
   * For each Reservoir with non-empty `seepage`, `discharge_limit`, or
   * `production_factor` inline arrays, this method moves entries into the
   * corresponding system-level arrays (reservoir_seepage_array, etc.),
   * auto-generating uids and setting the `reservoir` field from the parent.
   *
   * After extraction, the inline arrays on each Reservoir are cleared so
   * that re-expansion is idempotent.
   */
  void expand_reservoir_constraints();

  void setup_reference_bus(const class PlanningOptionsLP& options);
};

}  // namespace gtopt
