/**
 * @file      flow.hpp
 * @brief     Exogenous water flow (inflow/outflow) at a hydraulic junction
 * @date      Wed Jul 30 15:52:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Flow structure representing an exogenous water inflow (natural
 * river runoff) or outflow (minimum environmental discharge) at a junction.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "inflow_j1",
 *   "junction": "j1",
 *   "direction": 1,
 *   "discharge": "inflow"
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 3-D inline array indexed by `[scenario][stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Flow/`
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Exogenous water flow at a hydraulic junction
 *
 * Positive direction (direction = +1) models natural river inflow; negative
 * direction (direction = -1) models mandatory minimum discharge releases or
 * evaporation losses.
 *
 * @see Junction for the connected node
 * @see FlowLP for the LP water-balance contribution
 */
struct Flow
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `FlowLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Flow::class_name` directly (or
  /// `FlowLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Flow"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  OptInt direction {
      1};  ///< Flow direction: +1 = inflow, −1 = outflow [dimensionless]

  OptSingleId junction {};  ///< ID of the connected junction (optional for
                            ///< flow-turbine mode)
  /// Water discharge schedule [m³/s].  Optional: when unset AND
  /// ``fcost`` is set, the LP column upper bound defaults to
  /// ``+inf`` (``DblMax``) — useful for non-physical inflow slacks
  /// that don't need an explicit cap.  When unset AND ``fcost`` is
  /// also unset, the Flow contributes no column for that block (no
  /// bound information available).
  OptSTBRealFieldSched discharge {};

  /// Optional per-(stage, block) flow cost [$/(m³/s)/h].  When set,
  /// the LP-side ``Flow`` column relaxes from the default hard
  /// equality ``lowb = uppb = discharge`` to a soft band
  /// ``lowb = 0, uppb = discharge`` (or ``+inf`` if ``discharge`` is
  /// unset) and the column receives a positive objective coefficient
  /// ``fcost × cost_factor(scenario, stage, block)`` so the LP pays
  /// ``fcost · flow · duration`` per block.  Models PLEXOS-style
  /// "Non-physical Inflow Penalty" — a costed slack column that the
  /// solver only activates when the junction balance cannot otherwise
  /// close.  When unset the legacy hard-forced semantics are
  /// preserved (no cost, ``flow = discharge`` exactly).
  OptTBRealFieldSched fcost {};

  /// @return true if flow is directed into the junction (inflow)
  [[nodiscard]] constexpr bool is_input() const noexcept
  {
    return direction.value_or(1) >= 0;
  }
};

}  // namespace gtopt
