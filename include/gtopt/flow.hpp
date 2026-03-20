/**
 * @file      flow.hpp
 * @brief     Exogenous water flow (inflow/outflow) at a hydraulic junction
 * @date      Wed Jul 30 15:52:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Flow structure representing an exogenous water inflow (natural
 * river runoff) or outflow (minimum environmental discharge) at a junction.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "inflow_j1",
 *   "junction": "j1",
 *   "direction": 1,
 *   "discharge": "inflow"
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 3-D inline array indexed by `[scenario][stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Flow/`
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Exogenous water flow at a hydraulic junction
 *
 * Positive direction (direction = +1) models natural river inflow; negative
 * direction (direction = -1) models mandatory minimum discharge releases or
 * evaporation losses.
 *
 * @see Junction for the connected node
 * @see FlowLP for the LP water-balance contribution
 */
struct Flow
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  OptInt direction {
      1};  ///< Flow direction: +1 = inflow, −1 = outflow [dimensionless]

  OptSingleId junction {};  ///< ID of the connected junction (optional for
                            ///< flow-turbine mode)
  STBRealFieldSched discharge {};  ///< Water discharge schedule [m³/s]

  /// @return true if flow is directed into the junction (inflow)
  [[nodiscard]] constexpr bool is_input() const noexcept
  {
    return direction.value_or(1) >= 0;
  }
};

}  // namespace gtopt
