/**
 * @file      generator_profile.hpp
 * @brief     Generator profile configuration and attributes
 * @date      Tue Apr  1 21:20:35 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the GeneratorProfile structure which contains configuration data
 * for generator operational profiles including:
 * - Identification and activation status
 * - Associated generator reference
 * - Generation profile schedule
 * - Short-term cost schedule
 */

#pragma once

#include <gtopt/generator.hpp>

namespace gtopt
{

/**
 * @brief Configuration data for generator operational profiles
 * 
 * Contains all parameters needed to model a generator's operational
 * characteristics in the optimization problem.
 */
struct GeneratorProfile
{
  Uid uid {unknown_uid};      ///< Unique identifier for this profile
  Name name {};               ///< Human-readable name
  OptActive active {};        ///< Activation status schedule

  SingleId generator {unknown_uid};  ///< Associated generator ID
  STBRealFieldSched profile {};      ///< Generation profile schedule (per scenario/time/block)
  OptTRealFieldSched scost {};       ///< Short-term cost schedule (optional)
};

}  // namespace gtopt
