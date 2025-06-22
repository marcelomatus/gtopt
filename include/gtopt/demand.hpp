/**
 * @file      demand.hpp
 * @brief     Header file defining demand-related structures for power system
 * planning
 * @date      Thu Mar 27 09:12:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides structures and types to represent and manipulate
 * electrical demand entities within the planning framework.
 */

#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{

/**
 * @struct DemandAttrs
 * @brief Attributes of a demand entity
 *
 * Contains the essential parameters that define a demand's characteristics
 * including location, limits, costs, and capacity-related attributes.
 */
struct DemandAttrs
{
  SingleId bus {};  ///< Bus ID where the demand is connected
  OptTBRealFieldSched lmax {};  ///< Maximum load schedule
  OptTRealFieldSched lossfactor {};  ///< Loss factor schedule
  OptTRealFieldSched fcost {};  ///< Fixed cost schedule
  OptTRealFieldSched emin {};  ///< Minimum energy requirement schedule
  OptTRealFieldSched ecost {};  ///< Energy cost schedule

  OptTRealFieldSched capacity {};  ///< Current capacity
  OptTRealFieldSched expcap {};  ///< Expansion capacity
  OptTRealFieldSched expmod {};  ///< Expansion mode
  OptTRealFieldSched capmax {};  ///< Maximum capacity
  OptTRealFieldSched annual_capcost {};  ///< Annual capacity cost
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor
};

/**
 * @struct Demand
 * @brief Represents a demand entity in the power system
 *
 * Extends DemandAttrs with identification and activation status.
 * Provides functionality to set attributes from a DemandAttrs instance.
 */
struct Demand
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Descriptive name
  OptActive active {};  ///< Activation status

  SingleId bus {};  ///< Bus ID where the demand is connected
  OptTBRealFieldSched lmax {};  ///< Maximum load schedule
  OptTRealFieldSched lossfactor {};  ///< Loss factor schedule
  OptTRealFieldSched fcost {};  ///< Fixed cost schedule
  OptTRealFieldSched emin {};  ///< Minimum energy requirement schedule
  OptTRealFieldSched ecost {};  ///< Energy cost schedule

  OptTRealFieldSched capacity {};  ///< Current capacity
  OptTRealFieldSched expcap {};  ///< Expansion capacity
  OptTRealFieldSched expmod {};  ///< Expansion mode
  OptTRealFieldSched capmax {};  ///< Maximum capacity
  OptTRealFieldSched annual_capcost {};  ///< Annual capacity cost
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor

  /**
   * @brief Sets the demand attributes from a DemandAttrs object
   *
   * @tparam T Type of attributes (deduced automatically)
   * @param attrs The attribute object to transfer values from
   * @return Reference to the updated demand object
   */
  auto& set_attrs(auto&& attrs)
  {
    bus = std::exchange(attrs.bus, {});
    lmax = std::exchange(attrs.lmax, {});
    lossfactor = std::exchange(attrs.lossfactor, {});
    fcost = std::exchange(attrs.fcost, {});
    emin = std::exchange(attrs.emin, {});
    ecost = std::exchange(attrs.ecost, {});

    capacity = std::exchange(attrs.capacity, {});
    expcap = std::exchange(attrs.expcap, {});
    expmod = std::exchange(attrs.expmod, {});
    capmax = std::exchange(attrs.capmax, {});
    annual_capcost = std::exchange(attrs.annual_capcost, {});
    annual_derating = std::exchange(attrs.annual_derating, {});

    return *this;
  }
};

/**
 * @typedef DemandVar
 * @brief Variant type that can hold a demand identifier or attributes
 */
using DemandVar = std::variant<Uid, Name, DemandAttrs>;

/**
 * @typedef OptDemandVar
 * @brief Optional variant for demand-related data
 */
using OptDemandVar = std::optional<DemandVar>;

}  // namespace gtopt
