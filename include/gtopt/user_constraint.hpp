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
 * ```text
 * generator("TORO").generation + generator("uid:23").generation <= 300,
 *     for(stage in {4,5,6}, block in 1..30)
 * ```
 *
 * See constraint_parser.hpp for the full grammar and ConstraintExpr AST.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/user_constraint_enums.hpp>

namespace gtopt
{

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
 *
 * ### Soft constraints with visible slacks
 *
 * When `penalty` is set to a positive value, the constraint becomes
 * **soft**: the LP assembly automatically creates slack column(s) and
 * folds them into the row so the constraint can be violated at the
 * supplied per-unit penalty cost.  All auto-created slacks are
 * registered in the AMPL variable registry (so other constraints and
 * reports can reference them) and emitted to output as
 * `UserConstraint/slack(_pos|_neg).{csv,parquet}`, making any unserved
 * water/energy demand traceable per constraint without primal-dual
 * gap reconstruction.
 *
 * - `LESS_EQUAL`    (`expr <= rhs`):  one slack column `slack`,
 *   row gets `-1.0 * slack`, cost `+penalty * slack`.
 * - `GREATER_EQUAL` (`expr >= rhs`):  one slack column `slack`,
 *   row gets `+1.0 * slack`, cost `+penalty * slack`.
 * - `EQUAL`         (`expr =  rhs`):  two slack columns `slack_pos`,
 *   `slack_neg`, row gets `+1·slack_pos − 1·slack_neg`, both with
 *   cost `+penalty`.
 * - `RANGE`: not yet supported with `penalty`; an explicit pair of
 *   one-sided constraints should be used instead.
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
  OptReal penalty {};  ///< Per-unit slack cost.  When set and > 0, the
                       ///< constraint is relaxed via auto-created slack
                       ///< columns.  See "Soft constraints" above.
};

}  // namespace gtopt
