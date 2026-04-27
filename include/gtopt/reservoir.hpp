/**
 * @file      reservoir.hpp
 * @brief     Defines the Reservoir structure representing a water reservoir
 * @date      Wed Jul 30 23:11:55 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This file defines the `gtopt::Reservoir` structure, which models a water
 * reservoir within a hydro-thermal power system. A reservoir stores water
 * that drives turbines, subject to volume limits, spill constraints, and
 * evaporation losses.
 *
 * ### Unit conventions
 * - Volume fields (`emin`, `emax`, `eini`, `efin`, `capacity`): **hm³**
 *   (cubic hectometre = 10⁶ m³).  Legacy comments may say "dam³"
 *   but the actual unit is hm³, as demonstrated by
 *   `flow_conversion_rate = 0.0036`: `3600 m³ / 10⁶ = 0.0036`.
 * - Flow fields (`spillway_capacity`, `fmin`, `fmax`): **m³/s**
 * - The default `flow_conversion_rate = 0.0036` converts m³/s × h → hm³:
 *   `volume = 0.0036 × flow_m3s × duration_h`
 *
 * ### Scaling
 * LP variable scaling for reservoir energy/volume variables is controlled via
 * `PlanningOptions::variable_scales`.  No per-element `energy_scale` field is
 * exposed — use the `variable_scales` option array instead:
 * ```json
 * "variable_scales": [
 *   {"class_name": "Reservoir", "variable": "energy", "uid": 1, "scale": 1000}
 * ]
 * ```
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "res1",
 *   "junction": "j1",
 *   "emin": 100,
 *   "emax": 5000,
 *   "eini": 2500
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 1-D inline array indexed by `[stage]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Reservoir/`
 */

#pragma once

#include <array>

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>
#include <gtopt/reservoir_discharge_limit.hpp>
#include <gtopt/reservoir_production_factor.hpp>
#include <gtopt/reservoir_seepage.hpp>

namespace gtopt
{

/**
 * @brief Water reservoir in a hydro cascade system
 *
 * The reservoir accumulates and releases water between time blocks. The
 * volume balance per block is:
 *
 * ```text
 * V[t+1] = V[t] × (1 − annual_loss/8760 × duration)
 *        + flow_conversion_rate × (inflows − outflows) × duration
 * ```
 *
 * where inflows/outflows include waterway flows, turbine discharges, natural
 * inflows (Flow), and seepage (ReservoirSeepage).
 *
 * @see Junction for the hydraulic node the reservoir is attached to
 * @see ReservoirLP for the LP formulation
 */
struct Reservoir
{
  /// @name Default physical constants
  /// @{
  static constexpr Real default_spillway_capacity = 6'000.0;  ///< [m³/s]
  static constexpr Real default_fmin = -10'000.0;  ///< [m³/s]
  static constexpr Real default_fmax = +10'000.0;  ///< [m³/s]
  static constexpr Real default_flow_conversion_rate =
      0.0036;  ///< [hm³/(m³/s·h)]
  static constexpr Real default_mean_production_factor =
      5.0;  ///< [MWh/hm³] — fallback when no turbine data available
  /// @}

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId junction {unknown_uid};  ///< ID of the associated hydraulic junction

  /// Optional downstream junction for spill routing.  When set, the
  /// reservoir's drain (spillway) variable is also added with coefficient
  /// `+1.0` (m³/s as inflow) to that junction's balance row, so spilled
  /// water flows downstream instead of vanishing.  Mirrors PLP's `qv` chain
  /// where the vertimiento variable appears in BOTH the source reservoir's
  /// balance AND the downstream junction's balance (PLP `SerVer` field in
  /// `plpcnfce.dat`).  When unset (default), drain depletes storage only —
  /// matches the behaviour for reservoirs whose `SerVer = 0` (spill to sea).
  OptSingleId spill_junction {};

  OptReal spillway_capacity {
      default_spillway_capacity};  ///< Maximum uncontrolled spill capacity
                                   ///< [m³/s]
  OptReal spillway_cost {};  ///< Penalty cost per unit of spilled water [$/hm³]

  OptTRealFieldSched capacity {};  ///< Total usable storage capacity [hm³]
  OptTRealFieldSched annual_loss {};  ///< Annual fractional evaporation/seepage
                                      ///< loss [p.u./year]
  OptTRealFieldSched emin {};  ///< Minimum allowed stored volume [hm³]
  OptTRealFieldSched emax {};  ///< Maximum allowed stored volume [hm³]
  OptTRealFieldSched
      ecost {};  ///< Shadow cost of stored water (water value) [$/hm³]
  OptReal eini {};  ///< Initial stored volume at start of horizon [hm³].
                    ///< Sets an equality constraint vol_start = eini in the
                    ///< first stage of the first phase only.
  OptReal efin {};  ///< Minimum required stored volume at end of horizon
                    ///< [hm³].  Sets a >= constraint vol_end >= efin in the
                    ///< last stage of the last phase (not an equality).
  OptReal efin_cost {};  ///< Penalty cost per unit of `efin` shortfall
                         ///< [$/hm³].  When set (and > 0), the hard
                         ///< ``vol_end >= efin`` row becomes soft:
                         ///< ``vol_end + slack >= efin`` with `slack`
                         ///< priced at `efin_cost` in the objective.
                         ///< This mirrors PLP's per-stage rebalse-cost
                         ///< slack on the end-of-horizon volume target,
                         ///< letting the LP miss `efin` at a cost rather
                         ///< than going infeasible when upstream Benders
                         ///< cuts have clamped `sini` below what one
                         ///< stage of inflows can recover.  Without this
                         ///< field, ``efin`` is enforced as a hard >=
                         ///< constraint (the historical behaviour).

  OptReal mean_production_factor {};  ///< Expected turbine production factor
                                      ///< [MWh/hm³].  Converts the global
                                      ///< `state_fail_cost` ($/MWh) into
                                      ///< reservoir-specific units ($/hm³).
                                      ///< Defaults to 5.0 MWh/hm³.
  OptTRealFieldSched
      scost {};  ///< State cost: elastic penalty for SDDP state variable
                 ///< violations [$/hm³].  If not set, computed as
                 ///< `state_fail_cost × mean_production_factor`.

  OptTRealFieldSched
      soft_emin {};  ///< Soft minimum volume per stage [hm³].
                     ///< Creates a penalized constraint: efin + slack >=
                     ///< soft_emin, where the slack variable has a penalty cost
                     ///< (soft_emin_cost) in the objective.  Unlike emin (a
                     ///< hard variable bound), this allows the volume to drop
                     ///< below the threshold at a cost.  Corresponds to PLP's
                     ///< plpminembh.dat "holgura" (slack) constraint.
  OptTRealFieldSched
      soft_emin_cost {};  ///< Penalty cost per unit of soft_emin
                          ///< violation [$/hm³].  Applied to the slack
                          ///< variable that relaxes the soft_emin constraint.
                          ///< Must be > 0 for the constraint to be active.

  OptReal fmin {
      default_fmin};  ///< Minimum net flow into the reservoir junction [m³/s]
  OptReal fmax {
      default_fmax};  ///< Maximum net flow into the reservoir junction [m³/s]

  OptReal flow_conversion_rate {
      default_flow_conversion_rate};  ///< Converts m³/s × hours into hm³
                                      ///< [hm³/(m³/s·h)]

  /// Whether to propagate volume state across stage/phase boundaries via
  /// StateVariables (SDDP-style coupling). When true (the default for
  /// reservoirs), the final volume of one phase is carried over as the initial
  /// volume of the next. When false, an efin==eini constraint is added to close
  /// each phase independently.
  OptBool use_state_variable {};

  /// Enable PLP-style daily cycle operation (see Battery::daily_cycle).
  /// Default for reservoirs is false (disabled); can be enabled explicitly
  /// for small reservoirs that operate on a daily cycle.
  OptBool daily_cycle {};

  // ── Inline reservoir constraints (optional) ───────────────────────────
  // These can be defined here instead of in separate system-level arrays.
  // During expand_reservoir_constraints(), embedded entries are extracted
  // into the flat System arrays with auto-generated uids and the
  // reservoir field set from the parent.

  /// Inline seepage definitions for this reservoir.
  /// Each entry needs only `waterway` (and optionally slope/constant/segments);
  /// `reservoir` is set automatically from this reservoir's uid.
  Array<ReservoirSeepage> seepage {};

  /// Inline discharge limit definitions for this reservoir.
  /// Each entry needs only `waterway` and `segments`;
  /// `reservoir` is set automatically.
  Array<ReservoirDischargeLimit> discharge_limit {};

  /// Inline production factor definitions for this reservoir.
  /// Each entry needs only `turbine` and `segments`;
  /// `reservoir` is set automatically.
  Array<ReservoirProductionFactor> production_factor {};
};

}  // namespace gtopt
