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
 * ### Dual-value output
 *
 * `add_user_constraints_to_lp()` returns a vector of `UserConstraintState`
 * objects, one per active constraint that successfully added at least one row.
 * Each state holds the constraint UID/name, the resolved `ConstraintScaleType`,
 * and the per-(scenario, stage, block) row indices.
 *
 * After the LP is solved, pass the returned states to
 * `add_user_constraints_to_output()` to write dual (shadow-price) values to
 * the output context under the `"UserConstraint"` class name with field name
 * `"constraint"`, yielding
 * `output/UserConstraint/constraint_dual.{csv,parquet}`.
 *
 * Dual scaling per `ConstraintScaleType`:
 *  - `Power`  (default) → `scale_obj / (prob × discount × Δt)` — same as demand
 * rows
 *  - `Energy`           → `scale_obj / (prob × discount × Δt)` — same scaling,
 * unit $/MWh
 *  - `Raw`              → `scale_obj / discount` — discount factor only
 */

#pragma once

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
  ConstraintScaleType scale_type {ConstraintScaleType::Power};  ///< Resolved
                                                                ///< scale type
  STBIndexHolder<RowIndex>
      rows {};  ///< Row indices per (scenario, stage, block)
};

/**
 * @brief Add user-defined constraints to a `LinearProblem` before flattening.
 *
 * For each active `UserConstraint` in @p constraints:
 *  1. Parses `uc.expression` into a `ConstraintExpr` AST.
 *  2. Resolves `uc.constraint_type` to a `ConstraintScaleType` enum
 *     (defaulting to `Power` when absent).
 *  3. Iterates over every (scenario, stage, block) combination that falls
 *     inside the expression's domain restriction.
 *  4. Resolves each term's LP column by name or UID via @p sc.
 *  5. Adds one `SparseRow` per (scenario, stage, block) to @p lp.
 *  6. Saves the resulting `RowIndex` in the returned `UserConstraintState`.
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
    const SystemContext& sc,
    const PhaseLP& phase,
    const SceneLP& scene,
    LinearProblem& lp);

/**
 * @brief Write dual (shadow-price) values for user constraints to output.
 *
 * For each `UserConstraintState` in @p uc_states, calls `out.add_row_dual()`
 * (or the discount-only variant for `Raw`) to write per-(scenario,stage,block)
 * dual values.  The output goes to
 * `output/UserConstraint/constraint_dual.{csv,parquet}`.
 *
 * Scaling selected by `UserConstraintState::scale_type`:
 *  - `Power`  → `out.add_row_dual(...)` — uses `block_cost_factors`
 *  - `Energy` → `out.add_row_dual(...)` — uses `block_cost_factors`
 *  - `Raw`    → `out.add_row_dual_raw(...)` — uses
 * `discount_block_cost_factors`
 *
 * @param uc_states     States returned by `add_user_constraints_to_lp`.
 * @param out           Output context to write to.
 */
void add_user_constraints_to_output(
    const std::vector<UserConstraintState>& uc_states, OutputContext& out);

}  // namespace gtopt
