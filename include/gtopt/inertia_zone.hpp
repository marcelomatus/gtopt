/**
 * @file      inertia_zone.hpp
 * @brief     Defines the InertiaZone structure for system inertia requirements
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the InertiaZone structure which represents a regional
 * or system-wide minimum inertia requirement in the LP formulation.
 * Each zone specifies a required inertia level [MWs] and a shortage
 * penalty cost.
 *
 * The inertia requirement ensures that sufficient synchronous generation
 * is online to maintain frequency stability.  The constraint is:
 *
 *   sum_g (provision_factor_g * r_inertia_g) >= H_req
 *
 * where provision_factor = H_g * S_g / Pmin_g [MWs/MW].
 *
 * @see InertiaProvision for generator-to-zone linkage
 * @see InertiaZoneLP for the LP formulation
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct InertiaZone
 * @brief System inertia requirement zone
 *
 * An inertia zone specifies the minimum system inertia [MWs] that must
 * be provided by committed synchronous generators.  Generators contribute
 * to zones via @ref InertiaProvision entries.  If the requirement is
 * not met, the shortage is penalized at `cost` [$/MWs].
 *
 * @see InertiaProvision for generator-to-zone linkage
 * @see InertiaZoneLP for the LP formulation
 */
struct InertiaZone
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `InertiaZoneLP` exposes no separate `ClassName` member; callers
  /// reach the constant via `InertiaZone::class_name` directly (or
  /// `InertiaZoneLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"InertiaZone"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status

  OptTBRealFieldSched requirement {};  ///< Min inertia requirement [MWs]
  OptTRealFieldSched cost {};  ///< Inertia shortage penalty [$/MWs]
};

}  // namespace gtopt
