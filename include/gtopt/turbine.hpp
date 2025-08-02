/**
 * @file      turbine.hpp
 * @brief     Defines the Turbine structure representing a hydroelectric turbine
 * @date      Thu Jul 31 01:50:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Turbine structure which represents a hydroelectric 
 * turbine component in the system. A turbine converts water flow into electrical
 * energy through an associated generator.
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief Represents a hydroelectric turbine in the system
 *
 * A turbine converts water flow from a waterway into electrical energy
 * through an associated generator. The conversion is governed by the
 * conversion_rate parameter.
 */
struct Turbine
{
  Uid uid {unknown_uid};          ///< Unique identifier for the turbine
  Name name {};                   ///< Human-readable name
  OptActive active {};            ///< Activation status of the turbine

  SingleId waterway {unknown_uid}; ///< ID of connected waterway
  SingleId generator {unknown_uid}; ///< ID of connected generator

  OptTRealFieldSched conversion_rate {}; ///< Water-to-power conversion rate schedule
  OptTRealFieldSched capacity {};        ///< Maximum power capacity schedule
};

}  // namespace gtopt
