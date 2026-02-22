/**
 * @file      reserve_zone.hpp
 * @brief     Defines the ReserveZone structure for spinning-reserve modeling
 * @date      Thu Apr  3 10:32:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the ReserveZone structure which represents a regional
 * spinning-reserve requirement in the LP formulation. Each zone specifies
 * up/down reserve requirements and shortage penalty costs.
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct ReserveZone
 * @brief Regional spinning-reserve requirement zone
 *
 * A reserve zone aggregates up/down reserve requirements for a set of buses.
 * Generators contribute to zones via @ref ReserveProvision entries.
 * If requirements are not met, the shortage is penalized at `urcost`/`drcost`.
 *
 * @see ReserveProvision for generator-to-zone linkage
 * @see ReserveZoneLP for the LP formulation
 */
struct ReserveZone
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status

  OptTBRealFieldSched urreq {};  ///< Up-reserve requirement schedule [MW]
  OptTBRealFieldSched drreq {};  ///< Down-reserve requirement schedule [MW]
  OptTRealFieldSched urcost {};  ///< Up-reserve shortage penalty [$/MW]
  OptTRealFieldSched drcost {};  ///< Down-reserve shortage penalty [$/MW]
};

}  // namespace gtopt
