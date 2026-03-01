/**
 * @file      battery.hpp
 * @brief     Header for battery energy storage components
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing battery energy storage
 * systems in power system planning models. Batteries are modeled with
 * energy storage capabilities, efficiency parameters, and expansion options.
 *
 * @details Batteries in power systems provide services like energy arbitrage,
 * reserves, and grid support. This implementation models both operational
 * characteristics (state of charge limits, losses) and investment decisions
 * (expansion capacity).
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{
/**
 * @struct Battery
 * @brief Represents a battery energy storage system in a power system
 * planning model
 *
 * @details This structure defines a complete battery entity with
 * identification, technical parameters, and economic attributes.
 *
 * A battery can charge (absorb energy) and discharge (deliver energy) within
 * its operational constraints. The state of charge is tracked between time
 * periods, accounting for efficiency losses.
 *
 * @see BatteryLP for planning model formulation
 */
struct Battery
{
  Uid uid {unknown_uid};  ///< Unique identifier for database references
  Name name {};  ///< Human-readable battery name
  OptActive active {};  ///< Activation status (whether the battery is modeled)

  OptTRealFieldSched
      input_efficiency {};  ///< Input (charging) efficiency (fraction)

  OptTRealFieldSched
      output_efficiency {};  ///< Output (discharging) efficiency (fraction)

  OptTRealFieldSched
      annual_loss {};  ///< Annual energy loss rate (fraction per year)

  OptTRealFieldSched
      emin {};  ///< Minimum energy storage level (fraction of capacity)
  OptTRealFieldSched
      emax {};  ///< Maximum energy storage level (fraction of capacity)
  OptTRealFieldSched vcost {};  ///< Storage usage cost (per unit energy stored)
  OptReal vini {};  ///< Initial state of charge (initial condition)
  OptReal vfin {};  ///< Final state of charge (terminal condition)

  OptTRealFieldSched capacity {};  ///< Installed capacity (energy units)
  OptTRealFieldSched expcap {};  ///< Expansion capacity (potential additions)
  OptTRealFieldSched expmod {};  ///< Expansion module size (minimum increment)
  OptTRealFieldSched capmax {};  ///< Maximum capacity limit (total upper bound)
  OptTRealFieldSched
      annual_capcost {};  ///< Annual capacity cost (per unit per year)
  OptTRealFieldSched
      annual_derating {};  ///< Annual derating factor (degradation rate)
};

}  // namespace gtopt
