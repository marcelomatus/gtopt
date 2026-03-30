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
 * | Power  | `"power"` (default)          | `scale_obj / (prob × discount ×
 * Δt)`| | Energy | `"energy"`                   | `scale_obj / (prob × discount
 * × Δt)`| | Raw    | `"raw"`, `"unitless"`        | `scale_obj / discount` |
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

}  // namespace gtopt
