/**
 * @file      battery.hpp
 * @brief     Header for battery energy storage components
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing battery energy storage
 * systems (BESS) in power system planning models. A battery stores and
 * releases electrical energy between time blocks, subject to capacity and
 * efficiency constraints. It is coupled to the electrical network through a
 * @ref Converter that links it to a discharge @ref Generator and a charge
 * @ref Demand.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "bess1",
 *   "input_efficiency": 0.95,
 *   "output_efficiency": 0.95,
 *   "emin": 0,
 *   "emax": 100,
 *   "capacity": 100
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 1-D inline array indexed by `[stage]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Battery/`
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{
/**
 * @struct Battery
 * @brief Represents a battery energy storage system (BESS)
 *
 * @details A battery can charge (absorb energy) and discharge (deliver
 * energy) within its operational constraints. The state of charge (SoC)
 * is tracked between time blocks, accounting for round-trip efficiency
 * losses.
 *
 * The energy balance per block is:
 * ```
 * SoC[t+1] = SoC[t] × (1 − annual_loss/8760) + input_efficiency × charge
 *            − discharge / output_efficiency
 * ```
 *
 * @see Converter for the generator/demand coupling
 * @see BatteryLP for the LP formulation
 */
struct Battery
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable battery name
  OptActive active {};  ///< Activation status (default: active)

  OptTRealFieldSched
      input_efficiency {};  ///< Charging (round-trip in) efficiency [p.u.]
  OptTRealFieldSched output_efficiency {};  ///< Discharging efficiency [p.u.]
  OptTRealFieldSched
      annual_loss {};  ///< Annual self-discharge rate [p.u./year]

  OptTRealFieldSched emin {};  ///< Minimum state of charge [MWh]
  OptTRealFieldSched
      emax {};  ///< Maximum state of charge (usable capacity) [MWh]
  OptTRealFieldSched
      vcost {};  ///< Storage usage cost (penalty for SoC) [$/MWh]
  OptReal eini {};  ///< Initial state of charge [MWh]
  OptReal efin {};  ///< Terminal state of charge (end condition) [MWh]

  OptTRealFieldSched capacity {};  ///< Installed energy capacity [MWh]
  OptTRealFieldSched expcap {};  ///< Energy capacity per expansion module [MWh]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched capmax {};  ///< Absolute maximum energy capacity [MWh]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MWh-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]
};

}  // namespace gtopt
