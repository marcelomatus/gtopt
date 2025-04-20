/**
 * @file      battery.hpp
 * @brief     Header for battery energy storage components
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing battery energy storage
 * systems in power system optimization models.
 */

#pragma once

#include <concepts>
#include <expected>

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct BatteryAttrs
 * @brief Contains the core attributes for a battery energy storage system
 */
struct BatteryAttrs
{
  OptTRealFieldSched annual_loss {};  ///< Annual energy loss rate
  OptTRealFieldSched vmin {};  ///< Minimum state of charge
  OptTRealFieldSched vmax {};  ///< Maximum state of charge
  OptTRealFieldSched vcost {};  ///< Storage usage cost
  OptReal vini {};  ///< Initial state of charge
  OptReal vfin {};  ///< Final state of charge

  OptTRealFieldSched capacity {};  ///< Installed capacity
  OptTRealFieldSched expcap {};  ///< Expansion capacity
  OptTRealFieldSched expmod {};  ///< Expansion module size
  OptTRealFieldSched capmax {};  ///< Maximum capacity limit
  OptTRealFieldSched annual_capcost {};  ///< Annual capacity cost
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor
};

/**
 * @struct Battery
 * @brief Represents a battery energy storage system in a power system
 * optimization model
 */
struct Battery
{
  Uid uid {};  ///< Unique identifier
  Name name {};  ///< Battery name
  OptActive active {};  ///< Battery active status

  OptTRealFieldSched annual_loss {};  ///< Annual energy loss rate
  OptTRealFieldSched vmin {};  ///< Minimum state of charge
  OptTRealFieldSched vmax {};  ///< Maximum state of charge
  OptTRealFieldSched vcost {};  ///< Storage usage cost
  OptReal vini {};  ///< Initial state of charge
  OptReal vfin {};  ///< Final state of charge

  OptTRealFieldSched capacity {};  ///< Installed capacity
  OptTRealFieldSched expcap {};  ///< Expansion capacity
  OptTRealFieldSched expmod {};  ///< Expansion module size
  OptTRealFieldSched capmax {};  ///< Maximum capacity limit
  OptTRealFieldSched annual_capcost {};  ///< Annual capacity cost
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor

  /**
   * @brief Sets battery attributes from a BatteryAttrs object
   * @param attrs Battery attributes to be set
   * @return Reference to this Battery object
   *
   * @code{.cpp}
   * // Example usage:
   * Battery batt;
   * BatteryAttrs attrs;
   * attrs.vmax = 100.0;
   * attrs.capacity = 200.0;
   * batt.set_attrs(std::move(attrs));
   * // batt.vmax should now be 100.0, and attrs.vmax should be empty
   * @endcode
   */
  template<std::movable T>
  auto& set_attrs(T&& attrs)
  {
    annual_loss = std::exchange(attrs.annual_loss, {});
    vmin = std::exchange(attrs.vmin, {});
    vmax = std::exchange(attrs.vmax, {});
    vcost = std::exchange(attrs.vcost, {});
    vini = std::exchange(attrs.vini, {});
    vfin = std::exchange(attrs.vfin, {});

    capacity = std::exchange(attrs.capacity, {});
    expcap = std::exchange(attrs.expcap, {});
    expmod = std::exchange(attrs.expmod, {});
    capmax = std::exchange(attrs.capmax, {});
    annual_capcost = std::exchange(attrs.annual_capcost, {});
    annual_derating = std::exchange(attrs.annual_derating, {});

    return *this;
  }

  /**
   * @brief Validate the battery parameters for consistency
   * @return Expected with success (true) or error message
   */
  [[nodiscard]] std::expected<bool, std::string> validate() const
  {
    // Check for required parameters
    if (vmax.has_value() && vmin.has_value()) {
      // If both are Real values (not schedules), check min < max
      if (const auto* vmin_real = std::get_if<Real>(&vmin.value())) {
        if (const auto* vmax_real = std::get_if<Real>(&vmax.value())) {
          if (*vmin_real > *vmax_real) {
            return std::unexpected(
                "Battery minimum SOC greater than maximum SOC");
          }
        }
      }
    }

    if (vini.has_value() && vmax.has_value()) {
      Real vini_val = vini.value();
      if (const auto* vmax_real = std::get_if<Real>(&vmax.value())) {
        if (vini_val > *vmax_real) {
          return std::unexpected(
              "Battery initial SOC greater than maximum SOC");
        }
      }
    }

    return true;
  }
};

/**
 * Concept that defines what makes a valid battery type
 */
template<typename T>
concept BatteryLike = requires(T battery) {
  { battery.uid } -> std::convertible_to<Uid>;
  { battery.name } -> std::convertible_to<Name>;
  { battery.capacity } -> std::convertible_to<OptTRealFieldSched>;
  { battery.vmin } -> std::convertible_to<OptTRealFieldSched>;
  { battery.vmax } -> std::convertible_to<OptTRealFieldSched>;
};

static_assert(BatteryLike<Battery>, "Battery must satisfy BatteryLike concept");

using BatteryVar = std::variant<Uid, Name, BatteryAttrs>;
using OptBatteryVar = std::optional<BatteryVar>;

}  // namespace gtopt
