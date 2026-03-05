/**
 * @file      junction.hpp
 * @brief     Hydraulic junction node for hydro cascade systems
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Junction structure representing a hydraulic node
 * where one or more waterways and reservoirs meet. The water balance at each
 * junction is: inflows − outflows − reservoir_change = 0.
 *
 * ### JSON Example
 * ```json
 * {"uid": 1, "name": "j1"}
 * ```
 *
 * @see Waterway for hydraulic branches between junctions
 * @see Reservoir for storage connected to a junction
 * @see Flow for exogenous inflows/outflows at a junction
 */

#pragma once

#include <gtopt/field_sched.hpp>

namespace gtopt
{

/**
 * @struct Junction
 * @brief Hydraulic node in a hydro cascade network
 *
 * A junction is the analogue of an electrical bus in the hydraulic network.
 * Water can arrive from upstream waterways (flows in), leave to downstream
 * waterways (flows out), be stored in a reservoir, or leave the system
 * through a `drain` junction (e.g., the sea or an evaporation sink).
 *
 * @see Waterway for hydraulic branch connections
 * @see JunctionLP for the LP water-balance constraint
 */
struct Junction
{
  Uid uid {unknown_uid};   ///< Unique identifier
  Name name {};            ///< Human-readable junction name
  OptActive active {};     ///< Activation status (default: active)

  OptBool drain {};  ///< If true, excess water at this junction can leave the system freely
};

}  // namespace gtopt
