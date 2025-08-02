/**
 * @file      flow.hpp
 * @brief     Flow model for network optimization problems
 * @date      Wed Jul 30 15:52:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Flow structure representing a directed flow between components
 * in an optimization network. Each flow has direction, junction association,
 * and discharge schedule.
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * Represents a directed flow between components in an optimization network.
 *
 * Each flow has:
 * - Unique identifier and name
 * - Direction (input/output)
 * - Associated junction
 * - Time-dependent discharge schedule
 */
struct Flow
{
  Uid uid {unknown_uid};  ///< Unique identifier for the flow
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status

  /// Flow direction: 1 for input, -1 for output
  Int direction {1};

  SingleId junction {unknown_uid};  ///< Connected junction identifier
  STBRealFieldSched discharge {};  ///< Discharge schedule

  /// @returns true if flow is directed into the junction
  [[nodiscard]] constexpr bool is_input() const noexcept
  {
    return direction > 0;
  }
};

}  // namespace gtopt
