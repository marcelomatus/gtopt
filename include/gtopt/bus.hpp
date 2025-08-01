/**
 * @file      bus.hpp
 * @brief     Busbar electrical model definition
 * @date      Tue Mar 18 13:31:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Bus class representing an electrical busbar in power system
 * modeling.
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Electrical busbar model
 *
 * Represents a busbar in power system analysis with electrical properties
 * and operational status.
 */
struct Bus
{
  Uid uid {unknown_uid};            ///< Unique identifier
  Name name {};                     ///< Human-readable name
  OptActive active {};              ///< Operational status
  OptReal voltage {};               ///< Voltage magnitude (KV)
  OptReal reference_theta {};       ///< Voltage angle reference (radians)
  OptBool use_kirchhoff {true};     ///< Flag for Kirchhoff's law application

  /// Default constructor
  constexpr Bus() noexcept = default;

  /// Parameterized constructor
  constexpr Bus(Uid uid, Name name) noexcept 
    : uid(uid), name(std::move(name)) {}

  /**
   * @brief Determines if Kirchhoff's law should be applied
   * @param v_threshold Minimum voltage threshold for application
   * @return true if Kirchhoff's law should be applied
   */
  [[nodiscard]] constexpr bool needs_kirchhoff(
      const double v_threshold) const noexcept
  {
    return use_kirchhoff.value_or(true) && voltage.value_or(1.0) > v_threshold;
  }
};

}  // namespace gtopt
