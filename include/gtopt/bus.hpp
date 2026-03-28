/**
 * @file      bus.hpp
 * @brief     Busbar electrical model definition
 * @date      Tue Mar 18 13:31:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Bus struct representing an electrical busbar (node) in a
 * power system network. Buses serve as connection points for generators,
 * demands, and transmission lines.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "b1",
 *   "voltage": 220.0,
 *   "use_kirchhoff": true
 * }
 * ```
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Electrical busbar (node) in the power network
 *
 * Represents a busbar in power system analysis with electrical properties
 * and operational status. In DC power flow (Kirchhoff) mode a voltage-angle
 * variable θ is created for every bus; the reference bus is fixed at θ = 0.
 *
 * @see Line for branch connections between buses
 * @see Generator, Demand for injections at a bus
 */
struct Bus
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)
  OptName type {};  ///< Optional bus type tag (e.g. "pq", "pv", "slack")
  OptReal voltage {};  ///< Nominal voltage level [kV].
                       ///< Used by needs_kirchhoff() to filter low-V buses.
  OptReal reference_theta {};  ///< Fixed voltage angle for reference bus [rad]
  OptBool use_kirchhoff {};  ///< Override global Kirchhoff setting for this bus

  /**
   * @brief Determines if Kirchhoff's law should be applied
   * @param v_threshold Minimum voltage threshold for application
   * @return true if Kirchhoff's law should be applied
   */
  [[nodiscard]] constexpr bool needs_kirchhoff(const double v_threshold) const
  {
    return use_kirchhoff.value_or(true)
        && voltage
               .transform([v_threshold](double v) { return v > v_threshold; })
               .value_or(true);
  }
};

}  // namespace gtopt
