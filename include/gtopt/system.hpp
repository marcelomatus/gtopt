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

#include <gtopt/allowance_pool.hpp>
#include <gtopt/ammonia_node.hpp>
#include <gtopt/ammonia_storage.hpp>
#include <gtopt/battery.hpp>
#include <gtopt/bus.hpp>
#include <gtopt/capacity_profile.hpp>
#include <gtopt/carrier_converter.hpp>
#include <gtopt/commitment.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/decision_variable.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/demand_profile.hpp>
#include <gtopt/emission.hpp>
#include <gtopt/emission_source.hpp>
#include <gtopt/emission_zone.hpp>
#include <gtopt/flow.hpp>
#include <gtopt/flow_right.hpp>
#include <gtopt/fuel.hpp>
#include <gtopt/future_cost.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/generator_profile.hpp>
#include <gtopt/hydrogen_node.hpp>
#include <gtopt/hydrogen_storage.hpp>
#include <gtopt/inertia_provision.hpp>
#include <gtopt/inertia_zone.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_commitment.hpp>
#include <gtopt/lng_terminal.hpp>
#include <gtopt/plant.hpp>
#include <gtopt/pump.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/reservoir_discharge_limit.hpp>
#include <gtopt/reservoir_production_factor.hpp>
#include <gtopt/reservoir_seepage.hpp>
#include <gtopt/simple_commitment.hpp>
#include <gtopt/thermal_node.hpp>
#include <gtopt/thermal_storage.hpp>
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
  /// Unified kind-tagged capacity-factor profiles (Commit 1 of the
  /// legacy → unified migration).  Parses from `"capacity_profile_array"` in
  /// JSON; per-owner-kind dispatch via `CapacityProfile::owner_kind`.
  Array<CapacityProfile> capacity_profile_array {};

  // ── Energy storage ──────────────────────────────────────────────────────
  Array<Battery> battery_array {};  ///< Battery energy storage systems
  Array<Converter>
      converter_array {};  ///< Battery ↔ generator/demand couplings

  // ── Thermal carrier (CSP / district-heat) ────────────────────────────────
  Array<ThermalNode>
      thermal_node_array {};  ///< Carrier-tagged balance nodes (MW_th).
  Array<ThermalStorage>
      thermal_storage_array {};  ///< Molten-salt / sensible-heat TES peers
                                 ///< of ``Battery`` on ``StorageLP<>``.

  // ── Hydrogen carrier (electrolyser / fuel cell / salt cavern) ────────────
  Array<HydrogenNode>
      hydrogen_node_array {};  ///< Carrier-tagged balance nodes (MWh_LHV).
  Array<HydrogenStorage>
      hydrogen_storage_array {};  ///< Salt cavern / LH₂ / LOHC peers of
                                  ///< ``Battery`` on ``StorageLP<>``.

  // ── Ammonia carrier (long-term H₂ via Haber-Bosch / cracker) ─────────────
  Array<AmmoniaNode>
      ammonia_node_array {};  ///< Carrier-tagged balance nodes (MWh_LHV).
  Array<AmmoniaStorage>
      ammonia_storage_array {};  ///< Refrigerated NH₃ tanks; canonical
                                 ///< seasonal-storage carrier.

  // ── Multi-carrier converters (electrolyser, fuel cell, HB, cracker, …) ──
  Array<CarrierConverter>
      carrier_converter_array {};  ///< One-stage flow converters between
                                   ///< any two carrier-typed balance nodes.

  // ── CO₂ / pollutant allowance pools (cap-and-trade) ─────────────────────
  Array<AllowancePool>
      allowance_pool_array {};  ///< Tradable emission-allowance pools
                                ///< (EU ETS / California C&T / RGGI).
                                ///< Banking + free allocation today;
                                ///< auction (Phase 4) + EmissionZone
                                ///< coupling (Phase 3) come later.

  // ── Fuel storage ────────────────────────────────────────────────────────
  Array<LngTerminal> lng_terminal_array {};  ///< LNG storage terminals

  // ── Reserve modeling ────────────────────────────────────────────────────
  Array<ReserveZone>
      reserve_zone_array {};  ///< Spinning-reserve requirement zones
  Array<ReserveProvision>
      reserve_provision_array {};  ///< Generator → reserve zone links

  // ── Fuels ──────────────────────────────────────────────────────────────
  /// Time-schedulable fuel price + combustion / upstream emission
  /// factors referenced by `Generator.fuel`.  See `fuel.hpp`.
  Array<Fuel> fuel_array {};

  // ── Emissions (pollutants) ─────────────────────────────────────────────
  /// First-class pollutant entities (e.g. `co2`, `so2`, `nox`).  Carry
  /// optional per-stage `price` (tax / permit cost in $/ton) and `cap`
  /// (tons / stage, soft constraint with `cap_cost` slack).  Passive in
  /// Commit 1 — wiring of cap row and price coefficient lands in
  /// Commit 4 alongside the per-fuel `emission_factors[]` table on
  /// `Fuel`.  See `emission.hpp`.
  Array<Emission> emission_array {};

  /// Constraint-owning zones (cap / soft-cap / price) for each
  /// pollutant.  Always builds a per-(scenario, stage, block)
  /// production column + balance row aggregating all
  /// `EmissionSource` rows pointing at this zone.  Mirrors
  /// `InertiaZone` / `ReserveZone`.  Passive in Commit 2 — see
  /// `emission_zone.hpp`.
  Array<EmissionZone> emission_zone_array {};

  /// Generator → EmissionZone bridge rows.  Each row says "this
  /// generator contributes `rate` tons / MWh of dispatch to this
  /// zone's balance".  Also accepted inline on
  /// `Generator.emissions[]` and expanded here by
  /// `System::expand_emission_sources()`.  Mirrors `InertiaProvision`
  /// / `ReserveProvision`.  Passive in Commit 2 — see
  /// `emission_source.hpp`.
  Array<EmissionSource> emission_source_array {};

  // ── Unit commitment ────────────────────────────────────────────────────
  Array<Commitment>
      commitment_array {};  ///< Generator unit commitment parameters
  Array<SimpleCommitment>
      simple_commitment_array {};  ///< Simplified dispatch commitments

  // ── Optimal transmission switching (OTS, issue #509) ───────────────────
  /// Per-line OTS opt-in records.  Presence of a LineCommitment row
  /// marks the referenced Line as a switching candidate (binary
  /// ``u_l ∈ {0, 1}`` decided by the MIP); absence keeps the line as
  /// pure-LP continuous flow.  See ``line_commitment.hpp`` for the
  /// per-record field set and ``LineCommitmentLP`` for the LP
  /// formulation (capacity gating + KVL big-M disjunction).
  ///
  /// Rejected on the SDDP / cascade methods at
  /// ``validate_planning.cpp`` time per issue #509 §"Why monolithic
  /// only, not SDDP".
  Array<LineCommitment> line_commitment_array {};

  // ── Inertia modeling ───────────────────────────────────────────────────
  Array<InertiaZone>
      inertia_zone_array {};  ///< System inertia requirement zones
  Array<InertiaProvision>
      inertia_provision_array {};  ///< Generator → inertia zone links

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
  Array<Pump> pump_array {};  ///< Hydro pumps (demand → waterway upstream)
  Array<ReservoirProductionFactor>
      reservoir_production_factor_array {};  ///< Volume-dependent turbine
                                             ///< efficiency

  // ── Water rights (NOT part of hydro topology) ───────────────────────────
  Array<FlowRight> flow_right_array {};  ///< Flow-based water rights (m³/s)
  Array<VolumeRight>
      volume_right_array {};  ///< Volume-based water rights (hm³)

  // ── User parameters and constraints ─────────────────────────────────────
  Array<UserParam> user_param_array {};  ///< Named parameters for constraints
  Array<DecisionVariable>
      decision_variable_array {};  ///< Free continuous decision vars referenced
                                   ///< by user constraints (PLEXOS DV maps
                                   ///< here)
  Array<FutureCost>
      future_cost_array {};  ///< Cost-to-go (FCF / α) planning elements
  Array<Plant>
      plant_array {};  ///< Plant primitive: groups generator config variants
                       ///< (combined-cycle TGA/TGB/TV, fuel-band families)
                       ///< and emits Σ-capacity / Σ-commit / Σ-uniq rows
                       ///< natively (replaces PlantCap_* / *_Uniq UCs)
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

  /**
   * @brief Move inline `Generator.emissions[]` entries into the flat
   *        `emission_source_array`, stamping `generator = <this gen>`
   *        and auto-allocating uids/names where missing.
   *
   * Mirrors `expand_reservoir_constraints()` / `expand_batteries()` —
   * the inline JSON form is user ergonomics; the LP iterates the
   * top-level flat array.  Idempotent: a second call sees empty
   * `Generator.emissions` and is a no-op.
   *
   * Called from PlanningLP setup alongside the other expand methods.
   */
  void expand_emission_sources();

  /**
   * @brief Auto-fold the legacy single-pollutant
   * `Fuel.combustion_emission_factor` and `Fuel.upstream_emission_factor` (both
   * CO₂-only) into the new multi-pollutant `Fuel.emission_factors[]` table.
   *
   * For each fuel with either legacy field set: synthesize (or update)
   * a `FuelEmissionFactor` row keyed by the CO₂ `Emission` (creating
   * the CO₂ tag if absent).  Both legacy fields are cleared so a
   * second call is a no-op.  Mirrors `fold_legacy_emission_rate()`.
   */
  void fold_legacy_fuel_emission_factors();

  /**
   * @brief Synthesize `EmissionSource` rows from
   *        `Generator.fuel × Generator.heat_rate × Fuel.emission_factors[]`.
   *
   * For each `Generator` with `fuel` + scalar `heat_rate` set, for
   * each entry in the referenced `Fuel.emission_factors[]`, for each
   * `EmissionZone` covering that pollutant: append an
   * `EmissionSource{rate = heat_rate × factor.combustion,
   *                  upstream_rate = heat_rate × factor.upstream}`
   * row to `emission_source_array`.  The synthesized row name is
   * `<gen>_<pollutant>_via_fuel`.
   *
   * Time-varying heat_rate (vector / FileSched) or time-varying
   * fuel factors emit a one-shot WARN and skip the row — manual
   * migration to explicit `EmissionSource` rows is required.
   * Idempotent on the scalar path: a re-run produces the same rows
   * with the same auto-generated names, so duplicates would collide
   * and skip.
   *
   * Mirrors `expand_emission_sources()` (the inline-on-generator
   * unwrap) — both produce the same flat `emission_source_array`
   * that the LP layer iterates.
   */
  void expand_fuel_emission_sources();

  /**
   * @brief Auto-fold the legacy `Generator.emission_rate` (scalar
   *        per-MWh CO₂ rate) into a synthetic CO₂ `Emission` +
   *        `EmissionZone` + `EmissionSource` triple, then clear the
   *        legacy field on the generator.
   *
   * For each `Generator` with a non-null scalar `emission_rate`:
   *   - If no `Emission{name="co2"}` exists, create one.
   *   - If no `EmissionZone` covers that pollutant, create
   *     `EmissionZone{name="default_co2", emissions=[{co2, 1.0}]}`
   *     (pure-reporting zone — no cap, no price).
   *   - Append an `EmissionSource{generator: <this>, zone:
   *     default_co2_zone, emission: co2, rate: emission_rate}`.
   *
   * Vector and FileSched legacy factors are NOT downgraded (warning
   * emitted and field left untouched); the user should migrate them
   * manually to the new schema.  Idempotent on already-folded
   * generators (skips when `emission_rate` is null).
   *
   * Mirrors `fold_legacy_profiles()` — same migration idiom we used
   * for `GeneratorProfile` / `DemandProfile` → `CapacityProfile`.
   */
  void fold_legacy_emission_rate();

  /**
   * @brief Fold legacy `generator_profile_array` / `demand_profile_array`
   *        entries into the unified `capacity_profile_array`.
   *
   * Each legacy entry becomes a `CapacityProfile` with the matching
   * `owner_kind` (Generator / Demand) and `owner = legacy.generator` /
   * `legacy.demand`.  The legacy arrays are cleared after the move so
   * the LP layer sees only `capacity_profile_array`, making the
   * unified path the single source of truth for downstream code.
   *
   * Idempotent: calling twice is safe (second call is a no-op on
   * already-cleared arrays).
   */
  void fold_legacy_profiles();

  void setup_reference_bus(const class PlanningOptionsLP& options);
};

}  // namespace gtopt
