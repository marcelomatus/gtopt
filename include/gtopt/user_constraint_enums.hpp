/**
 * @file      user_constraint_enums.hpp
 * @brief     Enumerations for user-defined constraint configuration
 * @date      2026-03-30
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <cstdint>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// ─── ConstraintScaleType ────────────────────────────────────────────────────

/**
 * @brief How the LP dual of a user constraint row is scaled for output.
 *
 * The LP objective accumulates cost as `prob × discount × duration / scale_obj`
 * per unit.  The LP dual of a row therefore carries a factor that must be
 * inverted to recover the physical shadow price.  The `ConstraintScaleType`
 * determines which factors are removed:
 *
 * | Enum   | Accepted strings             | Inverse scale |
 * |--------|------------------------------|-------------------------------------|
 * | Power  | `"power"` (default)   | see below |
 * | Energy | `"energy"`            | see below |
 * | Raw    | `"raw"`, `"unitless"` | see below |
 *
 * Inverse scale formulas:
 * - Power/Energy: `scale_obj / (prob × discount × Δt)`
 * - Raw: `scale_obj / discount`
 *
 * - **Power** — constraint on an instantaneous-power (MW) variable such as
 *   generator output, load, or line flow.  Dual unit: $/MW.
 * - **Energy** — constraint on an energy (MWh) variable such as battery SoC.
 *   Dual unit: $/MWh.  Uses the same block_cost_factors scaling as Power.
 * - **Raw / Unitless** — constraint has no physical unit (e.g. a dimensionless
 *   coefficient matrix).  The dual is scaled back only by the stage discount
 *   factor; probability and block duration are NOT removed.  Dual unit:
 *   `scale_obj / discount`.
 */
enum class ConstraintScaleType : uint8_t
{
  Power = 0,  ///< Default — power (MW) constraint
  Energy,  ///< Energy (MWh) constraint
  Raw,  ///< Raw / unitless — only discount scaling
};

inline constexpr auto constraint_scale_type_entries =
    std::to_array<EnumEntry<ConstraintScaleType>>({
        {.name = "power", .value = ConstraintScaleType::Power},
        {.name = "energy", .value = ConstraintScaleType::Energy},
        {.name = "raw", .value = ConstraintScaleType::Raw},
        {.name = "unitless", .value = ConstraintScaleType::Raw},
    });

constexpr auto enum_entries(ConstraintScaleType /*tag*/) noexcept
{
  return std::span {constraint_scale_type_entries};
}

// ─── PenaltyClass ───────────────────────────────────────────────────────────

/**
 * @brief How the `penalty` value on a soft `UserConstraint` is interpreted
 *        before being assigned to the slack column's LP objective coefficient.
 *
 * The `UserConstraint::penalty` field stores a single scalar supplied by the
 * modeller, but different physical quantities use different natural units.
 * When the expression is a flow balance in m³/s, for example, the modeller
 * would like to author the penalty in $/m³ (the same unit as the global
 * `hydro_fail_cost`) and let the LP assembly convert to $/(m³/s) per block
 * using block duration.  `PenaltyClass` selects which conversion the slack
 * column's cost goes through:
 *
 * | Enum       | Accepted strings | Slack cost                        |
 * |------------|------------------|-----------------------------------|
 * | Raw        | `"raw"` (default)| `penalty` (no conversion)         |
 * | HydroFlow  | `"hydro_flow"`   | `penalty × duration[h] × 3600`    |
 *
 * - **Raw** — penalty is used verbatim.  Appropriate for dimensionless or
 *   already-converted coefficients, and for energy ($/MWh) constraints where
 *   the slack column is itself in MWh.  Default when `penalty_class` is
 *   absent (backwards-compatible with pre-2026-04 inputs).
 * - **HydroFlow** — penalty is authored in $/m³ (volume) and converted to
 *   $/(m³/s) per block using `duration × 3600`.  This mirrors the FlowRight
 *   fail-cost path (`source/flow_right_lp.cpp:74`) so that a soft-flow
 *   UserConstraint using `hydro_fail_cost`-style pricing composes uniformly
 *   with element-level pricing.
 */
enum class PenaltyClass : uint8_t
{
  Raw = 0,  ///< Default — penalty used verbatim as slack cost
  HydroFlow,  ///< $/m³ → $/(m³/s) via `× duration[h] × 3600`
};

inline constexpr auto penalty_class_entries =
    std::to_array<EnumEntry<PenaltyClass>>({
        {.name = "raw", .value = PenaltyClass::Raw},
        {.name = "hydro_flow", .value = PenaltyClass::HydroFlow},
    });

constexpr auto enum_entries(PenaltyClass /*tag*/) noexcept
{
  return std::span {penalty_class_entries};
}

}  // namespace gtopt
