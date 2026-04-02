/**
 * @file      right_junction.hpp
 * @brief     Water rights balance node (punto de control de derechos)
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the RightJunction structure representing a water rights
 * balance point — a node where multiple FlowRight entities connect
 * and their flows must satisfy a balance constraint.
 *
 * This is the rights-domain counterpart of Junction: where Junction
 * balances physical water mass, RightJunction balances water rights
 * claims against available supply.  It is NOT part of the physical
 * hydrological topology.
 *
 * In the Chilean Maule agreement, the "Armerillo" point is the
 * canonical example: upstream contributions (supply FlowRights with
 * direction=+1) are balanced against downstream rights claims
 * (withdrawal FlowRights with direction=-1).
 *
 * ### LP formulation
 * Creates a balance row per block:
 * ```
 * sum_{fr : direction=+1} flow(fr,b) - sum_{fr : direction=-1} flow(fr,b)
 *     + drain(b) >= 0
 * ```
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "armerillo",
 *   "junction": "maule_armerillo",
 *   "drain": true
 * }
 * ```
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Water rights balance node (punto de control de derechos)
 *
 * A node in the rights accounting graph where FlowRight entities
 * connect.  Each connected FlowRight contributes to the balance
 * row with its direction sign (+1 for supply, -1 for withdrawal).
 *
 * When `drain` is true (default), excess supply is allowed
 * (the balance constraint is >=).  When false, supply must exactly
 * match demand (equality constraint — unusual).
 *
 * The optional `junction` field references the physical hydro Junction
 * at the same location, for documentation and coupling purposes.
 * The RightJunction does NOT participate in the physical junction balance.
 *
 * @see Junction for the physical water balance node
 * @see FlowRight for the rights that connect to this node
 * @see RightJunctionLP for the LP formulation
 */
struct RightJunction
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  /// Optional reference to the physical Junction at the same location.
  /// For documentation and coupling purposes only — the RightJunction
  /// does NOT participate in the physical junction balance.
  OptSingleId junction {};

  /// Whether excess supply is allowed (true, default) or supply must
  /// exactly match demand (false).  Analogous to Junction::drain.
  OptBool drain {};
};

}  // namespace gtopt
