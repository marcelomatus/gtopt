/**
 * @file      line.hpp
 * @brief     Header for transmission line components
 * @date      Sun Apr 20 02:12:30 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing transmission lines
 * in power system optimization models.
 */

#pragma once

#include <concepts>
#include <expected>

#include <gtopt/capacity.hpp>

namespace gtopt
{

/**
 * @struct LineAttrs
 * @brief Contains the core attributes for a transmission line
 */
struct LineAttrs
{
  SingleId bus_a {};  ///< From-bus ID
  SingleId bus_b {};  ///< To-bus ID
  OptTRealFieldSched voltage {};  ///< Line voltage level
  OptTRealFieldSched resistance {};  ///< Line resistance
  OptTRealFieldSched reactance {};  ///< Line reactance
  OptTRealFieldSched lossfactor {};  ///< Line loss factor
  OptTBRealFieldSched tmin {};  ///< Minimum power flow limit
  OptTBRealFieldSched tmax {};  ///< Maximum power flow limit
  OptTRealFieldSched tcost {};  ///< Transmission cost
};

/**
 * @struct Line
 * @brief Represents a transmission line in a power system optimization model
 */
struct Line
{
  Uid uid {};  ///< Unique identifier
  Name name {};  ///< Line name
  OptActive active {};  ///< Line active status

  SingleId bus_a {};  ///< From-bus ID
  SingleId bus_b {};  ///< To-bus ID
  OptTRealFieldSched voltage {};  ///< Line voltage level
  OptTRealFieldSched resistance {};  ///< Line resistance
  OptTRealFieldSched reactance {};  ///< Line reactance
  OptTRealFieldSched lossfactor {};  ///< Line loss factor
  OptTBRealFieldSched tmin {};  ///< Minimum power flow limit
  OptTBRealFieldSched tmax {};  ///< Maximum power flow limit
  OptTRealFieldSched tcost {};  ///< Transmission cost

  OptTRealFieldSched capacity {};  ///< Installed capacity
  OptTRealFieldSched expcap {};  ///< Expansion capacity
  OptTRealFieldSched expmod {};  ///< Expansion module size
  OptTRealFieldSched capmax {};  ///< Maximum capacity limit
  OptTRealFieldSched annual_capcost {};  ///< Annual capacity cost
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor

  /**
   * @brief Sets line attributes from a LineAttrs object
   * @param attrs Line attributes to be set
   * @return Reference to this Line object
   */
  template<std::movable T>
  auto& set_attrs(T&& attrs)
  {
    bus_a = std::exchange(attrs.bus_a, {});
    bus_b = std::exchange(attrs.bus_b, {});
    voltage = std::exchange(attrs.voltage, {});
    resistance = std::exchange(attrs.resistance, {});
    reactance = std::exchange(attrs.reactance, {});
    lossfactor = std::exchange(attrs.lossfactor, {});
    tmin = std::exchange(attrs.tmin, {});
    tmax = std::exchange(attrs.tmax, {});
    tcost = std::exchange(attrs.tcost, {});

    return *this;
  }

  /**
   * @brief Validate the line parameters for consistency
   * @return Expected with success (true) or error message
   */
  [[nodiscard]] std::expected<bool, std::string> validate() const
  {
    // Bus connections must be specified and must be different
    if (bus_a == SingleId {} || bus_b == SingleId {}) {
      return std::unexpected("Line must connect two buses");
    }

    if (bus_a == bus_b) {
      return std::unexpected("Line cannot connect a bus to itself");
    }

    // Reactance must be specified for power flow calculations
    if (reactance.has_value()) {
      if (auto* reactance_real = std::get_if<Real>(&reactance.value())) {
        if (*reactance_real <= 0.0) {
          return std::unexpected("Line reactance must be positive");
        }
      }
    }

    // Flow limits check
    if (tmin.has_value() && tmax.has_value()) {
      if (auto* tmin_real = std::get_if<Real>(&tmin.value())) {
        if (auto* tmax_real = std::get_if<Real>(&tmax.value())) {
          if (*tmin_real > *tmax_real) {
            return std::unexpected(
                "Line minimum flow limit greater than maximum");
          }
        }
      }
    }

    return true;
  }
};

/**
 * Concept that defines what makes a valid line type
 */
template<typename T>
concept LineLike = requires(T line) {
  { line.uid } -> std::convertible_to<Uid>;
  { line.name } -> std::convertible_to<Name>;
  { line.bus_a } -> std::convertible_to<SingleId>;
  { line.bus_b } -> std::convertible_to<SingleId>;
};

static_assert(LineLike<Line>, "Line must satisfy LineLike concept");

using LineVar = std::variant<Uid, Name, LineAttrs>;
using OptLineVar = std::optional<LineVar>;

}  // namespace gtopt
