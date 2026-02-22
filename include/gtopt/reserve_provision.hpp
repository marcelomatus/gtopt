/**
 * @file      reserve_provision.hpp
 * @brief     Defines ReserveProvision – generator contribution to a reserve
 * zone
 * @date      Thu Apr  3 10:30:07 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the ReserveProvision structure which specifies how a
 * generator contributes up/down spinning reserve to one or more reserve zones.
 * Each provision entry links a generator to zone(s) and defines capacity
 * limits and cost coefficients.
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct ReserveProvision
 * @brief Generator contribution to spinning-reserve zone(s)
 *
 * Links a @ref Generator to one or more @ref ReserveZone entries and defines
 * the maximum reserve it may offer, optional capacity-factor limits, and the
 * reserve bid cost.
 *
 * `reserve_zones` is a comma-separated list of ReserveZone UIDs or names.
 *
 * @see ReserveZone for zone-level requirements
 * @see ReserveProvisionLP for the LP formulation
 */
struct ReserveProvision
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status

  SingleId generator {unknown_uid};  ///< ID of the providing generator
  String reserve_zones {};  ///< Comma-separated list of ReserveZone IDs/names

  OptTBRealFieldSched urmax {};  ///< Maximum up-reserve offer [MW]
  OptTBRealFieldSched drmax {};  ///< Maximum down-reserve offer [MW]

  OptTRealFieldSched
      ur_capacity_factor {};  ///< Up-reserve capacity factor [p.u.]
  OptTRealFieldSched
      dr_capacity_factor {};  ///< Down-reserve capacity factor [p.u.]

  OptTRealFieldSched
      ur_provision_factor {};  ///< Up-reserve provision factor [p.u.]
  OptTRealFieldSched
      dr_provision_factor {};  ///< Down-reserve provision factor [p.u.]

  OptTRealFieldSched urcost {};  ///< Up-reserve bid cost [$/MW]
  OptTRealFieldSched drcost {};  ///< Down-reserve bid cost [$/MW]
};

}  // namespace gtopt
