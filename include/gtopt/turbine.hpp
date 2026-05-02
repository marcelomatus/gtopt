/**
 * @file      turbine.hpp
 * @brief     Defines the Turbine structure representing a hydroelectric turbine
 * @date      Thu Jul 31 01:50:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Turbine structure which couples a hydraulic
 * waterway to an electrical generator, converting water flow [m³/s] into
 * electrical power [MW].
 *
 * ### Conversion relationship
 * ```text
 * power [MW] = efficiency [p.u.] × production_factor [MW·s/m³] × flow [m³/s]
 * ```
 *
 * The effective conversion rate is `efficiency × production_factor`.
 * If `efficiency` is not set, it defaults to 1.0.
 *
 * ### Variable production factor (hydraulic head)
 * When `main_reservoir` is set, the turbine's conversion rate can vary
 * with the reservoir volume (hydraulic head).  The piecewise-linear
 * production factor curve is provided by a matching `ReservoirProductionFactor`
 * element.  During SDDP forward iterations the conversion-rate LP
 * coefficient is updated based on the current reservoir volume.
 *
 * ### Connection modes
 *
 * A turbine connects to the water system via **one** of:
 * - `waterway` — traditional mode: reads water flow from a waterway
 *   variable in the junction balance LP.
 * - `flow` — simplified mode: reads discharge directly from a Flow
 *   element's fixed schedule.  No junctions or waterways needed.
 *   Useful for simple run-of-river (pasada) units.
 *
 * When `flow` is set, `waterway` is ignored.  The turbine's power
 * constraint becomes: `power ≤ discharge[block] × efficiency ×
 * production_factor`. Aperture updates automatically change the flow discharge,
 * so the turbine power bound varies correctly across scenarios.
 *
 * ### JSON Example (waterway mode)
 * ```json
 * {
 *   "uid": 1,
 *   "name": "t1",
 *   "waterway": "w1_2",
 *   "generator": "g_hydro",
 *   "production_factor": 0.0025,
 *   "capacity": 100,
 *   "main_reservoir": "res1"
 * }
 * ```
 *
 * ### JSON Example (flow mode — pasada)
 * ```json
 * {
 *   "uid": 2,
 *   "name": "t_pasada",
 *   "flow": "f_river",
 *   "generator": "g_pasada",
 *   "production_factor": 1.0
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 1-D inline array indexed by `[stage]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Turbine/`
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief Hydroelectric turbine converting water flow into electrical power
 *
 * A turbine draws water from a waterway, converts it at `efficiency ×
 * production_factor` into electrical power at the linked generator, and passes
 * the remaining flow to the downstream junction of the waterway.
 *
 * When `main_reservoir` is specified, the turbine's conversion rate may be
 * updated dynamically by the SDDP solver using the piecewise-linear
 * production factor curve from the corresponding `ReservoirProductionFactor`
 * element.
 *
 * @see Waterway for the water channel
 * @see Generator for the power output representation
 * @see ReservoirProductionFactor for the piecewise-linear production factor
 * @see TurbineLP for the LP formulation
 */
struct Turbine
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `TurbineLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Turbine::class_name` directly (or
  /// `TurbineLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Turbine"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  OptSingleId
      waterway {};  ///< ID of the connected waterway (optional if flow set)
  OptSingleId flow {};  ///< ID of the connected flow (alternative to waterway)
  SingleId generator {
      unknown_uid};  ///< ID of the connected electrical generator

  OptBool
      drain {};  ///< If true, turbine can spill water without generating power

  OptTRealFieldSched
      production_factor {};  ///< Water-to-power production factor [MW·s/m³]
  OptTRealFieldSched
      efficiency {};  ///< Turbine efficiency [p.u.] (default 1.0)
  OptTRealFieldSched capacity {};  ///< Maximum turbine power output [MW]

  /// Optional ID of the main reservoir whose volume drives the turbine's
  /// conversion rate.  When set, the SDDP solver will update the
  /// conversion-rate LP coefficient at each forward-pass iteration based
  /// on the current reservoir volume and the matching ReservoirProductionFactor
  /// element's piecewise-linear curve.  The ReservoirProductionFactor element
  /// must reference this turbine's UID in its @c turbine field and the
  /// reservoir's UID in its @c reservoir field.
  OptSingleId main_reservoir {};
};

}  // namespace gtopt
