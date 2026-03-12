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
 *
 * ### Dual-value output
 *
 * `add_user_constraints_to_lp()` returns a vector of `UserConstraintState`
 * objects, one per active constraint that successfully added at least one row.
 * Each state holds the constraint UID/name, the optional `constraint_type`,
 * and the per-(scenario, stage, block) row indices.
 *
 * After the LP is solved, pass the returned states to
 * `add_user_constraints_to_output()` to write dual (shadow-price) values to
 * the output context under the `"UserConstraint"` class name with field name
 * `"constraint"`, yielding
 * `output/UserConstraint/constraint_dual.{csv,parquet}`.
 *
 * Both `"power"` and `"energy"` constraint types use the same
 * `block_cost_factors` scaling (`scale_obj / (prob × discount × duration)`),
 * which converts the LP dual back to physical units.  The `constraint_type`
 * field is informational only:
 *  - `"power"` (or absent): dual is in $/MW — constraint is on a power variable
 *  - `"energy"`: dual is in $/MWh — constraint is on an energy variable
 */

#pragma once

#include <string>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/user_constraint.hpp>

namespace gtopt
{

/**
 * @brief Per-constraint LP state: row indices produced during `add_to_lp`.
 *
 * One `UserConstraintState` is created for each active `UserConstraint` that
 * successfully adds at least one row to the `LinearProblem`.  The `rows` map
 * provides the `RowIndex` for every (scenario, stage, block) tuple so that
 * dual values can be retrieved after the LP is solved.
 */
struct UserConstraintState
{
  Uid uid {};  ///< Constraint UID (from UserConstraint)
  Name name {};  ///< Constraint name (from UserConstraint)
  OptName constraint_type {};  ///< Scaling hint: "power" (default) or "energy"
  STBIndexHolder<RowIndex>
      rows {};  ///< Row indices per (scenario, stage, block)
};

/**
 * @brief Add user-defined constraints to a `LinearProblem` before flattening.
 *
 * For each active `UserConstraint` in @p constraints:
 *  1. Parses `uc.expression` into a `ConstraintExpr` AST.
 *  2. Iterates over every (scenario, stage, block) combination that falls
 *     inside the expression's domain restriction (`for(...)` clause).
 *  3. Resolves each term's LP column by name or UID via @p sc.
 *  4. Adds one `SparseRow` per (scenario, stage, block) to @p lp.
 *  5. Saves the resulting `RowIndex` in the returned `UserConstraintState`.
 *
 * Skips any term whose LP variable cannot be resolved (logs a warning).
 *
 * @param constraints   The user constraint definitions.
 * @param sc            System context (provides element lookup + labels).
 * @param phase         Phase whose stages are being built.
 * @param scene         Scene whose scenarios are being built.
 * @param lp            Target linear problem (modified in-place).
 * @return              One `UserConstraintState` per active constraint that
 *                      added at least one row.
 */
[[nodiscard]] std::vector<UserConstraintState> add_user_constraints_to_lp(
    const std::vector<UserConstraint>& constraints,
    SystemContext& sc,
    const PhaseLP& phase,
    const SceneLP& scene,
    LinearProblem& lp);

/**
 * @brief Write dual (shadow-price) values for user constraints to output.
 *
 * For each `UserConstraintState` in @p uc_states, calls `out.add_row_dual()`
 * to write the per-(scenario, stage, block) dual values.  The output goes to
 * `output/UserConstraint/constraint_dual.{csv,parquet}` with one column per
 * user constraint (identified by `uid:N` or `name:N` depending on options).
 *
 * @param uc_states     States returned by `add_user_constraints_to_lp`.
 * @param out           Output context to write to.
 */
void add_user_constraints_to_output(
    const std::vector<UserConstraintState>& uc_states, OutputContext& out);

}  // namespace gtopt
