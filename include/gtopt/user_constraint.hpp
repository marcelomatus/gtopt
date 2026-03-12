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

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @brief Stores a user-defined linear constraint expression
 *
 * The expression field contains the full constraint in AMPL-inspired syntax.
 * It is parsed at LP construction time into a ConstraintExpr AST
 * (see constraint_parser.hpp) and then applied to the LinearProblem.
 */
struct UserConstraint
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable constraint name
  OptBool active {};  ///< Activation status (default: active)
  Name expression {};  ///< Constraint expression in AMPL-inspired syntax
  OptName description {};  ///< Optional free-text description of the constraint
};

}  // namespace gtopt
