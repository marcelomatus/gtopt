/**
 * @file      lng_terminal.hpp
 * @brief     Defines the LngTerminal structure representing an LNG storage
 *            terminal
 * @date      Sun Apr 13 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This file defines the `gtopt::LngTerminal` structure, which models a
 * Liquefied Natural Gas (LNG) storage terminal within a power system.
 * An LNG terminal stores fuel that is regasified and consumed by linked
 * thermal generators, subject to tank volume limits, boil-off losses,
 * delivery schedules, and regasification capacity constraints.
 *
 * ### Unit conventions
 *
 * | Field                    | Unit                | Description |
 * |--------------------------|---------------------|-----------------------------------|
 * | emin, emax, eini, efin   | m³                  | LNG volume in tank | |
 * ecost                    | $/m³                | Holding cost per m³ stored |
 * | annual_loss              | p.u./year           | Boil-off gas fraction per
 * year    | | sendout_max, sendout_min | m³/h                | Regasification
 * rate limits        | | delivery                 | m³/stage            | Total
 * LNG delivered per stage     | | spillway_cost            | $/m³ | Penalty for
 * venting LNG           | | spillway_capacity        | m³/h                |
 * Max venting rate                  | | scost                    | $/m³ | SDDP
 * state penalty (× mpf → $/MWh) | | mean_production_factor   | MWh/m³ | Energy
 * content of LNG             | | soft_emin, soft_emin_cost| m³, $/m³ | Soft
 * lower bound and penalty      | | flow_conversion_rate     | m³/(m³/h · h) |
 * Default 1.0 (identity)            | | heat_rate (LngGeneratorLink) |
 * m³_LNG/MWh      | Fuel per MWh of electrical output |
 *
 * The default `flow_conversion_rate = 1.0` means flow [m³/h] × hours
 * → m³ (no unit conversion needed).  Set to a different value when using
 * energy-based units (e.g. MMBtu) instead of volumetric (m³).
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "GNL_Quintero",
 *   "emin": 5000,
 *   "emax": 150000,
 *   "eini": 80000,
 *   "sendout_max": 2000,
 *   "delivery": [50000, 0, 50000, 0],
 *   "spillway_cost": 100,
 *   "annual_loss": 0.001,
 *   "use_state_variable": true,
 *   "generators": [
 *     {"generator": 10, "heat_rate": 0.18},
 *     {"generator": 11, "heat_rate": 0.20}
 *   ]
 * }
 * ```
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Link between an LNG terminal and a thermal generator
 *
 * Defines the fuel consumption coupling: the generator consumes LNG at a
 * rate of `heat_rate` m³ per MWh of electrical output.
 */
struct LngGeneratorLink
{
  SingleId generator {unknown_uid};  ///< UID of the linked generator
  Real heat_rate {1.0};  ///< Fuel consumption rate [m³_LNG/MWh]
};

/**
 * @brief LNG storage terminal in a power system
 *
 * The terminal accumulates and releases LNG between time blocks. The
 * volume balance per block is:
 *
 * ```text
 * V[t+1] = V[t] × (1 − annual_loss/8760 × duration)
 *        + delivery_rate × duration
 *        − sendout × duration
 *        − vent × duration
 * ```
 *
 * where sendout feeds linked generators via heat-rate coupling:
 * `sendout ≥ Σ_g (heat_rate_g × P_g)` over each block.
 *
 * @see LngTerminalLP for the LP formulation
 */
struct LngTerminal
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `LngTerminalLP` exposes no separate `ClassName` member; callers
  /// reach the constant via `LngTerminal::class_name` directly (or
  /// `LngTerminalLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"LngTerminal"};

  /// @name Default physical constants
  /// @{
  static constexpr Real default_sendout_max = 10'000.0;  ///< [m³/h]
  static constexpr Real default_spillway_capacity = 1'000.0;  ///< [m³/h]
  static constexpr Real default_flow_conversion_rate = 1.0;  ///< [m³/(m³/h·h)]
  static constexpr Real default_mean_production_factor =
      5.0;  ///< [MWh/m³] — fallback for scost computation
  /// @}

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  // ── Storage parameters ─────────────────────────────────────────────────
  OptTRealFieldSched emin {};  ///< Minimum tank level [m³]
  OptTRealFieldSched emax {};  ///< Maximum tank level [m³]
  OptTRealFieldSched ecost {};  ///< Storage holding cost [$/m³]
  OptReal eini {};  ///< Initial tank level [m³]
  OptReal efin {};  ///< End-of-horizon minimum level [m³]
  OptReal efin_cost {};  ///< Penalty cost per unit of `efin` shortfall
                         ///< [$/m³].  When set (and > 0) the hard
                         ///< ``vol_end >= efin`` row becomes soft:
                         ///< ``vol_end + slack >= efin`` with the slack
                         ///< priced at `efin_cost`.  Mirrors the
                         ///< `Reservoir.efin_cost` mechanism (see
                         ///< storage_lp.hpp).

  // ── Losses ─────────────────────────────────────────────────────────────
  OptTRealFieldSched annual_loss {};  ///< Boil-off gas rate [p.u./year]

  // ── Send-out (regasification) ──────────────────────────────────────────
  OptReal sendout_max {};  ///< Max regasification rate [m³/h]
  OptReal sendout_min {};  ///< Min regasification rate [m³/h]

  // ── Delivery schedule ──────────────────────────────────────────────────
  OptTRealFieldSched delivery {};  ///< Scheduled LNG arrival [m³/stage]

  // ── Venting / spill ────────────────────────────────────────────────────
  OptReal spillway_cost {};  ///< Venting penalty cost [$/m³]
  OptReal spillway_capacity {};  ///< Max venting rate [m³/h]

  // ── SDDP state coupling ────────────────────────────────────────────────
  OptBool use_state_variable {};  ///< Propagate tank level across stages
  OptReal mean_production_factor {};  ///< [MWh/m³] for scost computation
  OptTRealFieldSched scost {};  ///< State penalty [$/m³]

  // ── Soft minimum ───────────────────────────────────────────────────────
  OptTRealFieldSched soft_emin {};  ///< Soft minimum tank level [m³]
  OptTRealFieldSched soft_emin_cost {};  ///< Penalty for soft_emin [$/m³]

  // ── Flow conversion ────────────────────────────────────────────────────
  OptReal flow_conversion_rate {};  ///< Converts m³/h × h → m³ (default 1.0)

  // ── Generator coupling ─────────────────────────────────────────────────
  Array<LngGeneratorLink> generators {};  ///< Linked generators with heat rates
};

}  // namespace gtopt
