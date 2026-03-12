/**
 * @file      user_constraint.hpp
 * @brief     User-defined constraint data structure for LP formulation
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * This module defines the UserConstraint struct, which stores a user-defined
 * linear constraint expression. The expression string follows an AMPL-inspired
 * syntax that references gtopt elements (generators, demands, lines, batteries)
 * and their LP variables, with optional domain restrictions over scenarios,
 * stages, and blocks.
 *
 * ### Expression syntax (AMPL-inspired)
 *
 * ```
 * generator("TORO").generation + generator("uid:23").generation <= 300,
 *     for(stage in {4,5,6}, block in 1..30)
 * ```
 *
 * See constraint_parser.hpp for the full grammar and ConstraintExpr AST.
 */

#pragma once

#include <string_view>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

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
 * Δt)` | | Energy | `"energy"`                   | `scale_obj / (prob ×
 * discount × Δt)` | | Raw    | `"raw"`, `"unitless"`        | `scale_obj /
 * discount`               |
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
enum class ConstraintScaleType : unsigned char
{
  Power = 0,  ///< Default — power (MW) constraint
  Energy,  ///< Energy (MWh) constraint
  Raw,  ///< Raw / unitless — only discount scaling
};

/**
 * @brief Parse a user-supplied string into a `ConstraintScaleType`.
 *
 * Accepted values (case-insensitive comparison NOT applied — exact match):
 *  - `"power"` → `ConstraintScaleType::Power`
 *  - `"energy"` → `ConstraintScaleType::Energy`
 *  - `"raw"`, `"unitless"` → `ConstraintScaleType::Raw`
 *  - empty / absent → `ConstraintScaleType::Power` (default)
 *
 * Unrecognised strings are treated as `Power` (with a warning emitted by the
 * caller).
 */
[[nodiscard]] constexpr ConstraintScaleType parse_constraint_scale_type(
    std::string_view s) noexcept
{
  if (s == "energy") {
    return ConstraintScaleType::Energy;
  }
  if (s == "raw" || s == "unitless") {
    return ConstraintScaleType::Raw;
  }
  // "power" or unknown → default Power
  return ConstraintScaleType::Power;
}

/**
 * @brief Stores a user-defined linear constraint expression
 *
 * The expression field contains the full constraint in AMPL-inspired syntax.
 * It is parsed at LP construction time into a ConstraintExpr AST
 * (see constraint_parser.hpp) and then applied to the LinearProblem.
 *
 * ### `constraint_type` and dual-value scaling
 *
 * The optional `constraint_type` field controls how the dual (shadow price) of
 * the constraint row is scaled when written to the output files.  When absent,
 * `"power"` is assumed.  See `ConstraintScaleType` for the full table.
 *
 * | Value                 | Dual output scaling                          |
 * |-----------------------|----------------------------------------------|
 * | `"power"` (default)   | `scale_obj / (prob × discount × Δt)`         |
 * | `"energy"`            | `scale_obj / (prob × discount × Δt)`         |
 * | `"raw"` / `"unitless"`| `scale_obj / discount`                       |
 */
struct UserConstraint
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable constraint name
  OptBool active {};  ///< Activation status (default: active)
  Name expression {};  ///< Constraint expression in AMPL-inspired syntax
  OptName description {};  ///< Optional free-text description of the constraint
  OptName constraint_type {};  ///< Scaling hint: "power" (default), "energy",
                               ///< "raw", or "unitless"
};

}  // namespace gtopt
