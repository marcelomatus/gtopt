/**
 * @file      waterway.hpp
 * @brief     Header for waterway components in power systems
 * @date      Wed Jul 30 11:40:33 2025  
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing waterways in power
 * system models. Waterways model connections between junctions with flow
 * constraints and loss characteristics.
 *
 * @details Waterways represent transmission paths that can carry flow between
 * junctions while accounting for capacity limits, minimum/maximum flow bounds,
 * and energy losses.
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @struct Waterway  
 * @brief Represents a waterway connection between two junctions
 *
 * @details This structure defines a transmission path with:
 * - Connection endpoints (junction_a and junction_b)
 * - Flow capacity constraints
 * - Minimum/maximum operating flow bounds
 * - Loss characteristics
 * - Activation status for scenario modeling
 *
 * @see Junction for connection point definitions
 * @see WaterwayLP for linear programming formulation
 */
struct Waterway
{
  Uid uid {unknown_uid};  ///< Unique identifier for database references
  Name name {};           ///< Human-readable waterway name
  OptActive active {};    ///< Activation status (whether waterway is modeled)

  SingleId junction_a {}; ///< Upstream junction identifier
  SingleId junction_b {}; ///< Downstream junction identifier

  OptTRealFieldSched capacity {};    ///< Maximum flow capacity
  OptTRealFieldSched lossfactor {};  ///< Energy loss coefficient (fraction per unit flow)

  OptTBRealFieldSched fmin {}; ///< Minimum required flow (may be negative for bidirectional)
  OptTBRealFieldSched fmax {}; ///< Maximum allowed flow
};

}  // namespace gtopt
