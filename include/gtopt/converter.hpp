/**
 * @file      converter.hpp
 * @brief     Defines the Converter structure for battery-generator coupling
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Converter structure which couples a Battery to a
 * Generator (discharge path) and a Demand (charge path), enabling energy
 * storage dispatch modeling in the LP formulation.
 */

#pragma once

#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct Converter
 * @brief Couples a battery to a generator (discharge) and a demand (charge)
 *
 * A converter links:
 * - A @ref Battery as the energy source
 * - A @ref Generator for the discharge (generation) path
 * - A @ref Demand for the charge (load) path
 *
 * The `conversion_rate` scales the electrical output relative to the stored
 * energy withdrawn from the battery.
 *
 * @see ConverterLP for the LP formulation
 */
struct Converter
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status

  SingleId battery {unknown_uid};  ///< ID of the linked battery
  SingleId generator {unknown_uid};  ///< ID of the discharge generator
  SingleId demand {unknown_uid};  ///< ID of the charge demand

  OptTRealFieldSched
      conversion_rate {};  ///< Electrical output per unit stored energy

  OptTRealFieldSched capacity {};  ///< Installed capacity [MW]
  OptTRealFieldSched expcap {};  ///< Expansion capacity [MW]
  OptTRealFieldSched expmod {};  ///< Expansion module size [MW]
  OptTRealFieldSched capmax {};  ///< Maximum capacity limit [MW]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual derating factor [p.u./year]
};

}  // namespace gtopt
