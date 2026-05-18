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

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>
#include <gtopt/single_id.hpp>

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
 * `reserve_zones` is a typed array of ReserveZone references (each element
 * is either a Uid number or a Name string).
 *
 * @see ReserveZone for zone-level requirements
 * @see ReserveProvisionLP for the LP formulation
 */
struct ReserveProvision
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `ReserveProvisionLP` exposes no separate `ClassName` member;
  /// callers reach the constant via `ReserveProvision::class_name`
  /// directly (or `ReserveProvisionLP::Element::class_name` in generic
  /// contexts).
  static constexpr LPClassName class_name {"ReserveProvision"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status

  SingleId generator {unknown_uid};  ///< ID of the providing generator
  Array<SingleId> reserve_zones {};  ///< Typed array of ReserveZone IDs / names

  OptTBRealFieldSched urmax {};  ///< Maximum up-reserve offer [MW]
  OptTBRealFieldSched drmax {};  ///< Maximum down-reserve offer [MW]

  OptTBRealFieldSched
      ur_capacity_factor {};  ///< Up-reserve capacity factor [p.u.] —
                              ///< per-(stage, block); accepts a scalar
                              ///< (broadcasts), a 2-D nested array, or a
                              ///< file-backed schedule.
  OptTBRealFieldSched
      dr_capacity_factor {};  ///< Down-reserve capacity factor [p.u.] —
                              ///< same shapes as ``ur_capacity_factor``.

  OptTBRealFieldSched
      ur_provision_factor {};  ///< Up-reserve provision factor [p.u.] —
                               ///< per-(stage, block); same shapes as
                               ///< ``ur_capacity_factor``.
  OptTBRealFieldSched
      dr_provision_factor {};  ///< Down-reserve provision factor [p.u.] —
                               ///< per-(stage, block).

  OptTBRealFieldSched urcost {};  ///< Up-reserve bid cost [$/MW] —
                                  ///< per-(stage, block).
  OptTBRealFieldSched drcost {};  ///< Down-reserve bid cost [$/MW] —
                                  ///< per-(stage, block).
};

}  // namespace gtopt
