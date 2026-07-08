/**
 * @file      volume_right.hpp
 * @brief     Volume-based water right (derechos de volumen)
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the VolumeRight structure representing a volume-based
 * water right.  This is an accounting entity — it tracks
 * accumulated right volumes (hm³) like Tilmant's "dummy reservoir"
 * for SDDP state coupling, but it is NOT part of the hydrological
 * topology: it does not connect to junctions or participate in the
 * physical water mass balance.
 *
 * The right references a physical reservoir from which the right is
 * sourced, but the coupling to the physical hydro system is handled
 * through separate constraints, not through the junction balance.
 *
 * A `purpose` field indicates the right's use case (irrigation,
 * generation, environmental, etc.) — all share the same LP structure.
 *
 * ### Unit conventions
 * - Volume fields (`emin`, `emax`, `eini`, `efin`, `demand`): **hm³**
 * - Flow fields (`max_rate`): **m³/s**
 * - Cost fields (`fail_cost`): **$/hm³**
 * - `flow_conversion_rate = 0.0036` converts m³/s × h → hm³
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "laja_irrigation_vol",
 *   "purpose": "irrigation",
 *   "reservoir": "laguna_laja",
 *   "demand": [0, 0, 0, 0, 100, 200, 300, 300, 200, 100, 50, 0],
 *   "fail_cost": 5000,
 *   "emax": 500
 * }
 * ```
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>
#include <gtopt/right_bound_rule.hpp>
#include <gtopt/stage_enums.hpp>

namespace gtopt
{

/**
 * @brief Volume-based water right (derechos de volumen)
 *
 * Models the accumulated volume entitlement of a right holder from
 * a reservoir.  Behaves like a storage element for LP/SDDP purposes:
 * accumulates delivered volumes over time with state variable coupling
 * across phases.
 *
 * This is purely a rights accounting entity — it is NOT part of the
 * hydrological topology.  The physical water delivery is modeled
 * through the existing hydro cascade; this entity tracks whether
 * enough water has been allocated to satisfy the right.
 *
 * The `purpose` field indicates the use case: "irrigation" for
 * consumptive agricultural rights, "generation" for non-consumptive
 * hydroelectric rights.
 *
 * @see Reservoir for the physical water source
 * @see VolumeRightLP for the LP formulation
 */
struct VolumeRight
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `VolumeRightLP` exposes no separate `ClassName` member; callers
  /// reach the constant via `VolumeRight::class_name` directly (or
  /// `VolumeRightLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"VolumeRight"};

  /// @name Default constants
  /// @{
  static constexpr Real default_flow_conversion_rate =
      0.0036;  ///< [hm³/(m³/s·h)]
  /// @}

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  /// Purpose of the water right: "irrigation", "generation",
  /// "environmental", etc.  Metadata only — does not affect LP.
  OptName purpose {};

  /// Reference to the physical source reservoir.  When set, the
  /// VolumeRight's input flow is subtracted from the Reservoir's
  /// energy balance (consumptive extraction — water exits the reservoir).
  OptSingleId reservoir {};

  /// Optional reference to another VolumeRight for volume balance.
  /// When set, this VolumeRight's input flow is added to the target
  /// VolumeRight's energy balance row with the given direction sign.
  /// This enables hierarchical rights structures (e.g., a parent
  /// VolumeRight distributing volume among child rights).
  OptSingleId right_reservoir {};

  /// Direction sign for the right_reservoir balance row:
  ///  +1 = supply (volume inflow to the balance)
  ///  -1 = withdrawal (volume extraction)
  /// Only meaningful when right_reservoir is set.
  OptInt direction {};

  // ── Storage-like fields (rights accounting) ────────────────────────

  OptTBRealFieldSched emin {};  ///< Minimum accumulated right volume [hm³]
                                ///< — per-(stage, block); accepts a scalar
                                ///< (broadcasts), a 2-D nested array, or
                                ///< a file-backed schedule.
  OptTBRealFieldSched emax {};  ///< Maximum accumulated right volume [hm³]
                                ///< — same shapes as ``emin``.
  OptTBRealFieldSched ecost {};  ///< Shadow cost of accumulated rights
                                 ///< [$/hm³] — per-(stage, block); accepts
                                 ///< a scalar (broadcasts), a 2-D nested
                                 ///< array, or a file-backed schedule.
  OptReal eini {};  ///< Initial accumulated volume at start of horizon [hm³]
  OptReal efin {};  ///< Minimum required accumulated volume at end [hm³]
  OptReal efin_cost {};  ///< Penalty cost per unit of `efin` shortfall
                         ///< [$/hm³].  Mirrors `Reservoir.efin_cost`
                         ///< — see storage_lp.hpp for semantics.

  OptTBRealFieldSched soft_emin {};  ///< Soft minimum volume [hm³] —
                                     ///< per-(stage, block); accepts a
                                     ///< scalar (broadcasts), a 2-D
                                     ///< nested array, or a file-backed
                                     ///< schedule.
  OptTBRealFieldSched
      soft_emin_cost {};  ///< Penalty cost for soft_emin violation
                          ///< [$/hm³] — per-(stage, block).

  // ── Right demand fields ────────────────────────────────────────────

  /// Required volume delivery per stage [hm³].
  /// This is the demand that must be met — unmet demand
  /// incurs the fail_cost penalty.
  OptTRealFieldSched demand {};

  /// Maximum extraction rate from the right [m³/s].
  /// Physical capacity limit on the rate at which the right can be
  /// exercised.
  OptTBRealFieldSched fmax {};

  /// Penalty cost for unmet volume demand [$/hm³].
  /// Analogous to demand_fail_cost for electrical load curtailment.
  /// Higher values give this right higher priority in the LP.
  OptReal fail_cost {};

  /// Priority level for allocation ordering [dimensionless].
  /// Used to differentiate between rights when multiple volume
  /// rights compete for the same water.
  OptReal priority {};

  // ── Saving (economy inflow) fields ────────────────────────────────

  /// Maximum saving deposit rate per block [m³/s].
  /// Only meaningful for economy VolumeRights (purpose="economy"):
  /// represents the rate at which unused rights are converted into
  /// savings.  When set, a `saving` LP variable is created per block.
  /// PLP: IVESN/IVERN/IVAPN.
  OptTBRealFieldSched saving_rate {};

  // ── Storage configuration ──────────────────────────────────────────

  OptReal flow_conversion_rate {
      default_flow_conversion_rate,
  };  ///< Converts m³/s × hours into hm³ [hm³/(m³/s·h)]

  /// Whether to propagate accumulated volume state across phases via
  /// StateVariables (SDDP-style coupling — Tilmant's "dummy reservoir").
  OptBool use_state_variable {};

  OptTRealFieldSched annual_loss {};  ///< Annual fractional loss [p.u./year]

  /// Calendar month at which rights are re-provisioned.
  /// When the stage's month matches reset_month, eini is set to:
  ///   - reset_value if set (explicit provision, e.g. 0 for the PLP
  ///     IVGAF anticipado up-counter reset at INICIOANTIC)
  ///   - evaluate_bound_rule(reservoir_volume) if bound_rule is set
  ///     (dynamic provisioning based on current reservoir level,
  ///     PLP: DerRiego = Base + Σ(Factor_i × Zone_Volume_i))
  ///   - emax if no bound_rule (simple full reprovision)
  /// This implements seasonal accounting (e.g., Laja irrigation
  /// rights re-provisioned each December for the Dec-Apr season).
  std::optional<MonthType> reset_month {};

  /// Reset at EVERY stage that starts a new calendar month (PLP:
  /// ``TipoEtaDE != INTRAETA``, genpdmaule.f:937 — the monthly
  /// electric counter IVMGEMF re-provisions each month).  Requires
  /// stage months; the first stage of the horizon keeps its config
  /// ``eini`` (PLP resets only for ``IEta > 1``).  May be combined
  /// with ``reset_month`` (either condition fires the reset).  Note:
  /// ``update_lp`` bound_rule refresh only tracks ``reset_month``
  /// stages — do not combine ``reset_monthly`` with a ``bound_rule``
  /// provision.
  OptBool reset_monthly {};

  /// Explicit provision value at reset_month [hm³].  Overrides both
  /// the bound_rule provisioning and the emax fallback.  Use 0 for
  /// PLP-style up-counters that restart each season (IVGAF resets to
  /// zero at INICIOANTIC, genpdlajam.f:656-660).
  OptReal reset_value {};

  /// Debit reference for the reset provisioning.  When set, the reset
  /// does NOT pin `eini` to the provision; instead it emits the PLP
  /// row  `eini + debit.vol_in = provision`  (genpdlajam.f:234-239):
  /// the referenced VolumeRight's incoming volume (an up-counter of
  /// early spending, e.g. the anticipado counter IVGAF) is subtracted
  /// from this bucket's seasonal provision.  With `eini ≥ 0`, spending
  /// more in advance than the provision covers is infeasible — the
  /// same guard PLP gets from `IVDRF ≥ 0`.
  ///
  /// ORDERING: the referenced VolumeRight must appear EARLIER in
  /// `volume_right_array` (same constraint as `right_reservoir`).
  OptSingleId reset_debit_right {};

  /// Volume-dependent bound rule for dynamic extraction adjustment.
  /// Serves two purposes:
  /// 1. Per-block: caps extraction rate to min(fmax, rule_value)
  /// 2. At reset_month: provisions eini = rule_value (annual quota)
  /// Both evaluated from the referenced reservoir's current volume
  /// via a piecewise-linear function.  Implements PLP cushion zone
  /// logic (Laja/Maule).
  std::optional<RightBoundRule> bound_rule {};
};

}  // namespace gtopt
