/**
 * @file      user_constraint_lp.hpp
 * @brief     LP element for user-defined constraints
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Declares `UserConstraintLP`, an LP element class (similar to `DemandLP`)
 * that wraps a single `UserConstraint` and participates in the standard
 * LP element collection mechanism.
 *
 * ### Collection registration
 *
 * `UserConstraintLP` is registered as the LAST type in `lp_element_types_t`
 * (see `lp_element_types.hpp`) so that user constraints are added to the LP
 * after all other elements — user constraint expressions reference LP columns
 * created by the earlier elements.
 *
 * ### Dual-value output
 *
 * `add_to_output()` writes dual (shadow-price) values for the constraint rows
 * to `output/UserConstraint/constraint_dual.{csv,parquet}`.
 *
 * Dual scaling follows the `ConstraintScaleType` stored on the constraint:
 *  - `Power`  (default) → `scale_obj / (prob × discount × Δt)`
 *  - `Energy`           → same scaling as Power, unit $/MWh
 *  - `Raw`              → `scale_obj / discount` — discount factor only
 */

#pragma once

#include <optional>

#include <gtopt/constraint_expr.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/user_constraint.hpp>

namespace gtopt
{

/**
 * @brief LP element wrapping a single user-defined constraint.
 *
 * Follows the same pattern as `DemandLP`, `GeneratorLP`, etc.:
 *  - Constructor parses and caches the constraint expression.
 *  - `add_to_lp()` is called once per (scenario, stage) by the system LP
 *    loop; it adds one SparseRow per block to the LinearProblem.
 *  - `add_to_output()` writes the LP dual values after solving.
 *
 * An empty or unparseable expression is silently skipped (no rows added).
 */
class UserConstraintLP : public ObjectLP<UserConstraint>
{
public:
  static constexpr LPClassName ClassName {"UserConstraint"};
  static constexpr std::string_view ConstraintName {"constraint"};

  explicit UserConstraintLP(const UserConstraint& uc, InputContext& ic);

  [[nodiscard]] constexpr auto&& user_constraint(this auto&& self) noexcept
  {
    return self.object();
  }

  /**
   * @brief Add LP rows for this constraint for one (scenario, stage) pair.
   *
   * Iterates over all blocks in @p stage; for each block that falls within
   * the constraint's domain, builds one `SparseRow` with the constraint
   * coefficients and bounds, and adds it to @p lp.  Stores the resulting
   * `RowIndex` values for later use in `add_to_output()`.
   *
   * @return true (always; skipped blocks are not errors)
   */
  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  /**
   * @brief Write dual (shadow-price) values for this constraint to output.
   *
   * Writes per-(scenario, stage, block) duals to
   * `output/UserConstraint/constraint_dual.{csv,parquet}`.
   * Scaling is determined by the `constraint_type` field on the underlying
   * `UserConstraint` object.
   *
   * @return true (always)
   */
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  /// Cached parsed expression (nullopt if expression was empty or invalid)
  std::optional<ConstraintExpr> m_expr_ {};
  /// How the LP dual should be scaled for output
  ConstraintScaleType m_scale_type_ {ConstraintScaleType::Power};
  /// Per-(scenario, stage) row indices produced by add_to_lp
  STBIndexHolder<RowIndex> m_rows_ {};
};

}  // namespace gtopt
