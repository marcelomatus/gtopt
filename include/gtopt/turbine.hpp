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
 * ```
 * power [MW] = conversion_rate [MW·s/m³] × flow [m³/s]
 * ```
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "t1",
 *   "waterway": "w1_2",
 *   "generator": "g_hydro",
 *   "conversion_rate": 0.0025,
 *   "capacity": 100
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
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief Hydroelectric turbine converting water flow into electrical power
 *
 * A turbine draws water from a waterway, converts it at `conversion_rate`
 * into electrical power at the linked generator, and passes the remaining
 * flow to the downstream junction of the waterway.
 *
 * @see Waterway for the water channel
 * @see Generator for the power output representation
 * @see TurbineLP for the LP formulation
 */
struct Turbine
{
  Uid uid {unknown_uid};   ///< Unique identifier
  Name name {};            ///< Human-readable name
  OptActive active {};     ///< Activation status (default: active)

  SingleId waterway {unknown_uid};   ///< ID of the connected waterway
  SingleId generator {unknown_uid};  ///< ID of the connected electrical generator

  OptBool drain {};  ///< If true, turbine can spill water without generating power

  OptTRealFieldSched conversion_rate {}; ///< Water-to-power conversion factor [MW·s/m³]
  OptTRealFieldSched capacity {};        ///< Maximum turbine power output [MW]
};

}  // namespace gtopt
