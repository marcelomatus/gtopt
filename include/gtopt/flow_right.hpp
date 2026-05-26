/**
 * @file      flow_right.hpp
 * @brief     Flow-based water right (derechos de caudal)
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the FlowRight structure representing a flow-based water right.
 * It tracks the required extraction rate (m³/s) that right holders are
 * entitled to.  When a junction is set, the flow is always consumptive:
 * it subtracts from the junction's physical water balance.
 *
 * The direction is always outflow (consumptive extraction from the
 * river system), fixed and not configurable.
 *
 * A `purpose` field indicates the right's use case (irrigation,
 * generation, environmental, etc.) — all share the same LP structure.
 *
 * ### Unified bounds + cost model
 * The bound triple (`fmin`, `target`, `fmax`) selects the LP shape:
 *
 *   - `fmin` is a hard lower bound on the served flow.  Default 0.
 *   - `fmax` is a hard upper bound on the served flow.  Defaults to
 *     `target` when `target` is set, else to `fmin` (i.e. a forced flow).
 *   - `target` is the soft kink point.  Below the target, `fcost`
 *     penalises shortfall; above the target, `uvalue` rewards excess.
 *     When `target` is unset, no kink exists and the column is a plain
 *     hard band `[fmin, fmax]`.
 *
 * The legacy `discharge` field is preserved as an alias of `target` for
 * back-compat with existing fixture JSONs.  Setting both `discharge`
 * and `target` is an error.
 *
 * ### Unit conventions
 * - Flow fields (`fmin`, `target`, `fmax`): **m³/s**
 * - Cost fields (`fcost`, `uvalue`): **$/m³/s·h** (penalty / reward
 *   per unit of unmet / excess flow per hour)
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "laja_nuevo_riego",
 *   "purpose": "irrigation",
 *   "junction": "laja_downstream",
 *   "target": [0, 0, 0, 0, 19.5, 42.25, 55.25, 65, 65, 52, 32.5, 13],
 *   "fcost": 5000
 * }
 * ```
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>
#include <gtopt/right_bound_rule.hpp>

namespace gtopt
{

/**
 * @brief Flow-based water right (derechos de caudal)
 *
 * Models the flow entitlement of a right holder at a river point.
 * Always represents a consumptive outflow (direction = -1, fixed).
 * When a junction is referenced, the flow variable is subtracted
 * from the junction's physical balance row.
 *
 * The (fmin, target, fmax, fcost, uvalue) tuple selects one of the
 * 7 unified-mode LP shapes documented in `flow_right_lp.cpp`.
 *
 * The `purpose` field indicates the use case: "irrigation",
 * "generation", "environmental", etc.
 *
 * @see Junction for the reference extraction point
 * @see FlowRightLP for the LP formulation
 */
struct FlowRight
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `FlowRightLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `FlowRight::class_name` directly (or
  /// `FlowRightLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"FlowRight"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  /// Purpose of the water right: "irrigation", "generation",
  /// "environmental", etc.  Metadata only — does not affect LP.
  OptName purpose {};

  /// Reference junction where the right is exercised.
  /// When set, the FlowRight's flow is subtracted from the
  /// junction's balance row (consumptive extraction).
  OptSingleId junction {};

  /// Direction sign for LP coupling:
  ///  +1 = supply (inflow to balance point)
  ///  -1 = withdrawal (outflow / consumptive extraction)
  OptInt direction {};

  /// Hard lower bound on the served flow [m³/s].
  /// Default 0.  Below this no shortfall is admissible — the bound is
  /// always enforced regardless of `fcost`.
  /// Per-stage-block scheduling (mirrors `Demand::lmax`): the value can
  /// vary hourly within a stage (e.g. PLP's hourly pmin obligations).
  OptTBRealFieldSched fmin {};

  /// Hard upper bound on the served flow [m³/s].
  /// When unset, defaults to `target` when `target` is set, else to
  /// `fmin` (yielding a forced exact flow).
  /// Per-stage-block scheduling.
  OptTBRealFieldSched fmax {};

  /// Soft kink point [m³/s].
  /// Below `target`: `fcost` penalty applies (irrigation-style soft
  ///   demand).  Above `target`: `uvalue` bonus / penalty applies
  ///   (variable-allocation style).
  /// When unset, the column reduces to a plain hard band [fmin, fmax]
  /// with no slack-side costs.
  /// Aliases legacy `discharge` for back-compat.
  /// Per-stage-block scheduling so hourly irrigation/forced-flow
  /// profiles round-trip through parquet without duplicate-uid warnings.
  OptTBRealFieldSched target {};

  /// Time-resolution mode for the flow LP variables and constraints.
  /// One of:
  ///  - `"per_block"`     (default): one LP `flow_b` per block; all
  ///                      bounds and the kink (target / fcost / uvalue)
  ///                      are applied per-block.
  ///  - `"stage_average"`: one LP `flow_b` per block (hard
  ///                      `[fmin_b, fmax_b]` bounds only), plus a
  ///                      stage-level `qeh = Σ_b dur_ratio_b × flow_b`
  ///                      column.  The kink and all costs are applied
  ///                      to `qeh` at stage scope.  Mirrors PLP's `H`-
  ///                      suffix stage-average variables (IQDRH, IQDEH).
  ///  - `"stage_uniform"`: a single stage-level `qeh` column is created
  ///                      and used in every block's junction balance —
  ///                      no per-block `flow_b`.  All bounds and the
  ///                      kink are applied to `qeh`.  Use when the
  ///                      right's flow is required to be constant
  ///                      across the stage (e.g. steady environmental
  ///                      releases).
  OptName flow_mode {};

  /// **Deprecated** legacy boolean that maps to `flow_mode`:
  ///  - `true`  → `flow_mode = "stage_average"`
  ///  - `false` → `flow_mode = "per_block"`
  /// Ignored when `flow_mode` is set explicitly.
  OptBool use_average {};

  /// Penalty cost for unmet flow below `target` [$/m³/s·h].
  /// Analogous to Demand::fcost for electrical load curtailment.
  /// Higher values give this right higher priority in the LP.
  /// Per-(stage, block) scheduling (mirrors Demand::fcost since PR-A).
  /// Accepts a scalar (broadcasts), a 2-D nested array, or a
  /// file-backed schedule.
  OptTBRealFieldSched fcost {};

  /// Value of exercising the right above `target` [$/m³/s·h].
  /// Subtracted from the objective (benefit): positive = incentivizes
  /// flow above target, negative = penalizes flow above target.
  /// Renamed from legacy `use_value` for naming parity with `fcost`.
  /// Per-(stage, block) scheduling.
  OptTBRealFieldSched uvalue {};

  /// Priority level for allocation ordering [dimensionless].
  OptReal priority {};

  /// Volume-dependent bound rule for dynamic fmax adjustment.
  /// When set, update_lp evaluates the piecewise-linear function
  /// at the referenced reservoir's current volume and updates the
  /// flow column upper bound to min(fmax, rule_value).
  /// This implements PLP cushion zone logic (Laja/Maule).
  std::optional<RightBoundRule> bound_rule {};

  /// Optional pass-through downstream junction.  When set, the
  /// FlowRight is no longer a pure 1-junction consumer: any water
  /// at ``junction`` that the (potentially capped) ``flow`` column
  /// cannot or does not consume can flow through an auxiliary
  /// ``bypass`` column to ``bypass_junction`` instead.  This models
  /// PLEXOS-style irrigation rights / hydro discharge envelopes
  /// where the river continues downstream after the irrigation
  /// point, regardless of how much (or little) is consumed by the
  /// right itself.
  ///
  /// LP shape per (scene, stage, block):
  ///   * the existing ``flow_col`` column still represents the
  ///     consumption side (bounded by ``[fmin, fmax]`` and the
  ///     target/fcost/uvalue kink) — its sign in the source
  ///     junction's balance is negative (water leaves the basin),
  ///   * a new ``bypass_col`` column is added (free, non-negative,
  ///     priced at ``bypass_cost·cf`` so it's only used when needed)
  ///     contributing negatively to ``junction``'s balance and
  ///     positively to ``bypass_junction``'s balance — preserving
  ///     mass conservation.
  ///
  /// Without this field the FlowRight remains a pure consumer
  /// (backward-compatible).
  OptSingleId bypass_junction {};

  /// Per-unit cost on the bypass column ($/m³/s·h).  Defaults to 0
  /// (free pass-through, used freely by the LP whenever water
  /// otherwise has nowhere to go).  Set a small positive value to
  /// prefer consumption (``flow_col``) over pass-through, or a
  /// large value to make the bypass a last-resort pressure release.
  /// Only meaningful when ``bypass_junction`` is set.
  OptReal bypass_cost {};
};

}  // namespace gtopt
