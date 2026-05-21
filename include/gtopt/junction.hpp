/**
 * @file      junction.hpp
 * @brief     Hydraulic junction node for hydro cascade systems
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Junction structure representing a hydraulic node
 * where one or more waterways and reservoirs meet. The water balance at each
 * junction is: inflows − outflows − reservoir_change = 0.
 *
 * ### JSON Example
 * ```json
 * {"uid": 1, "name": "j1"}
 * ```
 *
 * @see Waterway for hydraulic branches between junctions
 * @see Reservoir for storage connected to a junction
 * @see Flow for exogenous inflows/outflows at a junction
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>

namespace gtopt
{

/**
 * @struct Junction
 * @brief Hydraulic node in a hydro cascade network
 *
 * A junction is the analogue of an electrical bus in the hydraulic network.
 * Water can arrive from upstream waterways (flows in), leave to downstream
 * waterways (flows out), be stored in a reservoir, or leave the system
 * through a `drain` junction (e.g., the sea or an evaporation sink).
 *
 * @see Waterway for hydraulic branch connections
 * @see JunctionLP for the LP water-balance constraint
 */
struct Junction
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `JunctionLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Junction::class_name` directly (or
  /// `JunctionLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Junction"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable junction name
  OptActive active {};  ///< Activation status (default: active)

  OptBool drain {};  ///< If true, excess water at this junction can leave the
                     ///< system through a free outflow column (the
                     ///< per-block "drain") priced at @ref drain_cost
                     ///< (default 0) and bounded above by @ref
                     ///< drain_capacity (default +∞).  A bounded /
                     ///< costed drain lets plp2gtopt / plexos2gtopt
                     ///< encode "spill to the ocean" semantics
                     ///< directly on the central's own junction
                     ///< instead of synthesising a per-central
                     ///< ``<central>_ocean`` Junction + a connecting
                     ///< ``_ver`` Waterway with ``fmax = VertMax`` /
                     ///< ``fcost = CVert``.  Both encodings produce
                     ///< the same LP — the junction-level form just
                     ///< saves one Junction + one Waterway per
                     ///< terminal central.
  OptReal drain_capacity {};  ///< Upper bound [m³/s] on the drain column
                              ///< (default +∞).  Matches PLP's
                              ///< ``VertMax`` and PLEXOS's
                              ///< ``Waterway.Max Flow`` /
                              ///< ``Storage.Max Spill``.  Ignored
                              ///< unless @ref drain is true.
  OptReal drain_cost {};  ///< Per-(m³/s)·h cost on the drain column
                          ///< (default 0).  Matches PLP's ``CVert`` /
                          ///< ``Costo de Rebalse`` and PLEXOS's
                          ///< ``Waterway.Max Flow Penalty`` /
                          ///< ``Storage.Spill Penalty``.  Ignored
                          ///< unless @ref drain is true.
};

}  // namespace gtopt
