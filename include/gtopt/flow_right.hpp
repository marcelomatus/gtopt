/**
 * @file      flow_right.hpp
 * @brief     Flow-based water right (derechos de caudal)
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the FlowRight structure representing a flow-based water right.
 * This is an accounting entity — it tracks the required extraction rate
 * (m³/s) that right holders are entitled to, but it is NOT part of the
 * hydrological topology: it does not connect to junctions or participate
 * in the physical water mass balance.
 *
 * The direction is always outflow (consumptive extraction from the
 * river system), fixed and not configurable.
 *
 * A `purpose` field indicates the right's use case (irrigation,
 * generation, environmental, etc.) — all share the same LP structure.
 *
 * ### Unit conventions
 * - Flow fields (`discharge`, `fmax`): **m³/s**
 * - Cost fields (`fail_cost`): **$/m³/s·h** (penalty per unit of
 *   unmet flow demand per hour)
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "laja_nuevo_riego",
 *   "purpose": "irrigation",
 *   "junction": "laja_downstream",
 *   "discharge": [0, 0, 0, 0, 19.5, 42.25, 55.25, 65, 65, 52, 32.5, 13],
 *   "fail_cost": 5000
 * }
 * ```
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>
#include <gtopt/right_bound_rule.hpp>

namespace gtopt
{

/**
 * @brief Flow-based water right (derechos de caudal)
 *
 * Models the flow entitlement of a right holder at a river point.
 * Always represents a consumptive outflow (direction = -1, fixed).
 *
 * This is purely a rights accounting entity — it is NOT part of the
 * hydrological topology.  A deficit variable with penalty cost allows
 * soft-constraint violation when water is scarce, analogous to
 * demand_fail_cost for unserved electrical load.
 *
 * The `purpose` field indicates the use case: "irrigation" for
 * consumptive agricultural rights, "generation" for non-consumptive
 * hydroelectric rights, "environmental" for ecological minimum flows.
 *
 * @see Junction for the reference extraction point
 * @see FlowRightLP for the LP formulation
 */
struct FlowRight
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  /// Purpose of the water right: "irrigation", "generation",
  /// "environmental", etc.  Metadata only — does not affect LP.
  OptName purpose {};

  /// Reference junction where the right is exercised.
  /// For documentation and coupling purposes only — the FlowRight
  /// does NOT participate in the physical junction balance.
  OptSingleId junction {};

  /// Direction sign for LP coupling:
  ///  +1 = supply (inflow to balance point)
  ///  -1 = withdrawal (outflow / consumptive extraction)
  OptInt direction {};

  /// Required extraction flow schedule [m³/s].
  /// The demand that must be met — unmet demand incurs
  /// the fail_cost penalty.  Indexed by [scenario][stage][block].
  STBRealFieldSched discharge {};

  /// Maximum flow for variable-mode rights [m³/s].
  /// When set (and discharge is 0 or unset), the flow column becomes
  /// variable [0, fmax] instead of fixed [discharge, discharge].
  /// This enables partition constraints where the optimizer decides
  /// how to split flow among rights categories.
  OptTBRealFieldSched fmax {};

  /// Whether this FlowRight's flow variable should be added to the
  /// physical Junction's balance row.  When true, the flow is
  /// consumptive — water exits the physical network.
  /// Default: false (rights accounting only, no physical coupling).
  OptBool consumptive {};

  /// Whether to create a stage-average hourly flow variable (`qeh`).
  /// When true, FlowRightLP creates a stage-level LP variable
  /// equal to the duration-weighted average of the block-level flows:
  ///   qeh = Σ_b [ flow(b) × dur(b) / dur_stage ]
  /// This mirrors PLP's `IQDRH`, `IQDEH`, etc. — the "H"-suffix
  /// stage-average variables used in volume accumulation constraints
  /// and rights limit bounds.
  /// Default: false (block-level only).
  OptBool use_average {};

  /// Penalty cost for unmet flow demand [$/m³/s·h].
  /// Analogous to demand_fail_cost for electrical load curtailment.
  /// Higher values give this right higher priority in the LP.
  /// Supports per-stage-block scheduling for monthly cost modulation.
  OptTBRealFieldSched fail_cost {};

  /// Value of exercising the right [$/m³/s·h].
  /// Subtracted from the objective (benefit): positive = incentivizes use,
  /// negative = penalizes use.  Added as −use_value to the flow variable's
  /// objective coefficient.
  /// Supports per-stage-block scheduling for monthly value modulation.
  OptTBRealFieldSched use_value {};

  /// Priority level for allocation ordering [dimensionless].
  OptReal priority {};

  /// Volume-dependent bound rule for dynamic fmax adjustment.
  /// When set, update_lp evaluates the piecewise-linear function
  /// at the referenced reservoir's current volume and updates the
  /// flow column upper bound to min(fmax, rule_value).
  /// This implements PLP cushion zone logic (Laja/Maule).
  std::optional<RightBoundRule> bound_rule {};
};

}  // namespace gtopt
