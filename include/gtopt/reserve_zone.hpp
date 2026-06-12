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

#include <gtopt/lp_class_name.hpp>
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
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `ReserveZoneLP` exposes no separate `ClassName` member; callers
  /// reach the constant via `ReserveZone::class_name` directly (or
  /// `ReserveZoneLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"ReserveZone"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status
  OptName type {};  ///< Optional element type/category tag
  OptName description {};  ///< Optional free-text description (e.g. conversion
                           ///< provenance)

  OptTBRealFieldSched urreq {};  ///< Up-reserve requirement schedule [MW]
  OptTBRealFieldSched drreq {};  ///< Down-reserve requirement schedule [MW]
  OptTBRealFieldSched urmin {};  ///< Up-reserve Min-Provision floor [MW].  A
                                 ///< static lower bound on total provided
                                 ///< up-reserve (PLEXOS ``Min Provision``):
                                 ///< the LP enforces ``Σ pf·prov ≥
                                 ///< max(urreq, urmin)``.  Kept separate from
                                 ///< ``urreq`` so the time-varying requirement
                                 ///< schedule mirrors PLEXOS's reported RHS
                                 ///< instead of being raised by the floor.
  OptTBRealFieldSched drmin {};  ///< Down-reserve Min-Provision floor [MW].
                                 ///< Mirrors ``urmin`` for the down direction.
  OptTBRealFieldSched urcost {};  ///< Up-reserve shortage penalty [$/MW]
                                  ///< per-(stage, block); accepts a scalar
                                  ///< (broadcasts), a 2-D nested array, or
                                  ///< a file-backed schedule.
  OptTBRealFieldSched drcost {};  ///< Down-reserve shortage penalty [$/MW]
                                  ///< per-(stage, block); same shapes as
                                  ///< ``urcost``.
};

}  // namespace gtopt
