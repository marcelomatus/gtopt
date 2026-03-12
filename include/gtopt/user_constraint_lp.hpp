/**
 * @file      user_constraint_lp.hpp
 * @brief     LP application of user-defined constraints
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Declares `add_user_constraints_to_lp()`, which translates a vector of
 * `UserConstraint` objects into rows in a `LinearProblem` **before** it is
 * flattened into the internal solver structure.
 *
 * The function is called from `SystemLP`'s internal `create_linear_interface`
 * helper after all element LP rows and columns have been added.
 *
 * ### Element ID resolution
 *
 * Element IDs in constraint expressions are resolved using
 * `SystemContext::get_element<T>()`:
 *
 * | Expression form   | Resolved as            |
 * |-------------------|------------------------|
 * | `"G1"`            | Name lookup            |
 * | `"uid:3"`         | UID 3 lookup           |
 * | `3` (bare int)    | UID 3 lookup           |
 */

#pragma once

#include <vector>

#include <gtopt/linear_problem.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/user_constraint.hpp>

namespace gtopt
{

/**
 * @brief Add user-defined constraints to a `LinearProblem` before flattening.
 *
 * For each active `UserConstraint` in @p constraints:
 *  1. Parses `uc.expression` into a `ConstraintExpr` AST.
 *  2. Iterates over every (scenario, stage, block) combination that falls
 *     inside the expression's domain restriction (`for(...)` clause).
 *  3. Resolves each term's LP column by name or UID via @p sc.
 *  4. Adds one `SparseRow` per (scenario, stage, block) to @p lp.
 *
 * Skips any term whose LP variable cannot be resolved (logs a warning).
 *
 * @param constraints   The user constraint definitions.
 * @param sc            System context (provides element lookup + labels).
 * @param phase         Phase whose stages are being built.
 * @param scene         Scene whose scenarios are being built.
 * @param lp            Target linear problem (modified in-place).
 */
void add_user_constraints_to_lp(const std::vector<UserConstraint>& constraints,
                                SystemContext& sc,
                                const PhaseLP& phase,
                                const SceneLP& scene,
                                LinearProblem& lp);

}  // namespace gtopt
