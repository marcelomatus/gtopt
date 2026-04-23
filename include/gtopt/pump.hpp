/**
 * @file      pump.hpp
 * @brief     Defines the Pump structure representing a hydraulic pump unit
 * @date      Sat Apr 12 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Pump structure which couples a hydraulic
 * waterway to an electrical demand, converting electrical power [MW] into
 * water flow [m³/s] pumped upstream.
 *
 * ### Conversion relationship
 * ```text
 * pump_power [MW] = pump_factor [MW/(m³/s)] × pump_flow [m³/s]
 * ```
 *
 * The pump consumes electrical power at the linked demand to push water
 * through the waterway from its junction_a (downstream) to junction_b
 * (upstream).  This is the reverse of a Turbine, which converts water
 * flow into electrical power at a generator.
 *
 * ### Variable pump factor (hydraulic head)
 * When `main_reservoir` is set, the pump's conversion rate can vary
 * with the reservoir volume (hydraulic head).  The piecewise-linear
 * pump factor curve may be provided by a matching element (future).
 *
 * ### Connection mode
 *
 * A pump connects to the water system via a `waterway` whose
 * junction_a is the intake (downstream reservoir) and junction_b is
 * the discharge (upstream reservoir).  The pump's electrical load is
 * represented by the linked `demand`.
 *
 * ### Reversible units (e.g. HB Maule)
 *
 * A reversible turbine-pump unit is modeled as two separate elements:
 * - A `Turbine` on the generation waterway (upstream → downstream)
 * - A `Pump` on the pumping waterway (downstream → upstream)
 *
 * In LP/SDDP (no binary variables), simultaneous generation and pumping
 * is prevented by optimality: round-trip efficiency < 1.0 makes it
 * always suboptimal.  For MIP unit-commitment, explicit mode-exclusivity
 * constraints can be added via UserConstraint.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "HB_MAULE_PUMP",
 *   "waterway": "MACHICURA_pump_33_28",
 *   "demand": "HB_MAULE_DEMAND",
 *   "pump_factor": 1.88,
 *   "efficiency": 0.85,
 *   "capacity": 40.0
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 1-D inline array indexed by `[stage]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Pump/`
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief Hydraulic pump converting electrical power into upstream water flow
 *
 * A pump draws electrical power from a demand, converts it at
 * `pump_factor` into water flow through a waterway, pushing water from
 * the downstream junction (junction_a) to the upstream junction
 * (junction_b) of the waterway.
 *
 * When `main_reservoir` is specified, the pump's conversion rate may be
 * updated dynamically by the SDDP solver using a piecewise-linear
 * pump factor curve (future feature).
 *
 * @see Waterway for the water channel
 * @see Demand for the electrical load representation
 * @see Turbine for the reverse conversion (water → power)
 * @see PumpLP for the LP formulation
 */
struct Pump
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId waterway {unknown_uid};  ///< ID of the pumping waterway
  SingleId demand {unknown_uid};  ///< ID of the electrical demand (pump load)

  OptTRealFieldSched
      pump_factor {};  ///< Power consumed per unit flow [MW/(m³/s)]
  OptTRealFieldSched efficiency {};  ///< Pump efficiency [p.u.] (default 1.0)
  OptTRealFieldSched capacity {};  ///< Maximum pump flow [m³/s]

  /// Optional ID of the main reservoir whose volume drives the pump's
  /// conversion rate.  When set, the SDDP solver may update the
  /// pump-factor LP coefficient at each forward-pass iteration based
  /// on the current reservoir volume (future feature).
  OptSingleId main_reservoir {};
};

}  // namespace gtopt
