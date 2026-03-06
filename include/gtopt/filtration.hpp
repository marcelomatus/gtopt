/**
 * @file      filtration.hpp
 * @brief     Water filtration (seepage) from a waterway to a reservoir
 * @date      Thu Jul 31 23:22:44 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Filtration structure modeling water seepage from a waterway
 * into an adjacent reservoir. The seepage flow is modelled as a linear
 * function of the waterway's flow rate:
 *
 * ```
 * seepage [m³/s] = slope [m³/s / (m³/s)] × waterway_flow [m³/s] + constant
 * [m³/s]
 * ```
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "filt1",
 *   "waterway": "w1_2",
 *   "reservoir": "r1",
 *   "slope": 0.02,
 *   "constant": 0.5
 * }
 * ```
 */

#pragma once

#include <gtopt/object.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief Linear seepage model between a waterway and a reservoir
 *
 * Models water loss from a waterway that accumulates in an adjacent
 * reservoir.  The seepage volume per block equals
 * `(slope × flow + constant) × duration × flow_conversion_rate`.
 *
 * @see Waterway for the source channel
 * @see Reservoir for the receiving storage
 * @see FiltrationLP for the LP formulation
 */
struct Filtration
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)

  SingleId waterway {unknown_uid};  ///< ID of the source waterway
  SingleId reservoir {unknown_uid};  ///< ID of the receiving reservoir
  Real slope {0.0};  ///< Seepage rate proportional to waterway flow
                     ///< [dimensionless; seepage_flow/waterway_flow]
  Real constant {0.0};  ///< Constant seepage rate independent of flow [m³/s]
};

}  // namespace gtopt
