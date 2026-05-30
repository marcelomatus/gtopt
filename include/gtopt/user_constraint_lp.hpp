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
#include <gtopt/schedule.hpp>
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
  static constexpr std::string_view ConstraintName {"constraint"};
  /// Slack column emitted for one-sided soft constraints (`<=` / `>=`).
  static constexpr std::string_view SlackName {"slack"};
  /// Positive-deviation slack for soft equality constraints
  /// (`expr = rhs`).  Sign convention: row gets `+1.0 * slack_pos`.
  static constexpr std::string_view SlackPosName {"slack_pos"};
  /// Negative-deviation slack for soft equality constraints
  /// (`expr = rhs`).  Sign convention: row gets `-1.0 * slack_neg`.
  static constexpr std::string_view SlackNegName {"slack_neg"};

  explicit UserConstraintLP(const UserConstraint& uc, InputContext& ic);

  [[nodiscard]] constexpr auto&& user_constraint(this auto&& self) noexcept
  {
    return self.object();
  }

  /// PAMPL parameter accessor for `user_constraint("X").rhs` references
  /// in other constraints' expressions.  Returns the per-(stage, block)
  /// override when the schedule is set and resolves at that key;
  /// otherwise ``std::nullopt`` so the call site can fall back to the
  /// expression's scalar.
  [[nodiscard]] auto param_rhs(StageUid s, BlockUid b) const
      -> std::optional<Real>
  {
    return m_rhs_.optval(s, b);
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
  /// How the `penalty` scalar is converted to the slack-column cost.
  /// Parsed once at construction; per-block conversion is applied inside
  /// the `add_to_lp` block loop when the constraint is soft.
  PenaltyClass m_penalty_class_ {PenaltyClass::Raw};
  /// Per-(scenario, stage) row indices produced by add_to_lp
  STBIndexHolder<RowIndex> m_rows_ {};
  /// Per-(scenario, stage) slack columns for soft constraints.
  /// Used by `LESS_EQUAL` / `GREATER_EQUAL` (single slack) and as the
  /// `+` slack for soft `EQUAL` constraints.  Empty when `penalty` is
  /// not set on the underlying `UserConstraint`.
  STBIndexHolder<ColIndex> m_slack_cols_ {};
  /// Per-(scenario, stage) negative-deviation slack columns used only
  /// for soft `EQUAL` constraints (the `−` half of the absolute-value
  /// relaxation).  Empty for one-sided soft constraints.
  STBIndexHolder<ColIndex> m_slack_neg_cols_ {};
  /// Per-(stage, block) override for the constraint's RHS.  When the
  /// underlying ``UserConstraint`` ships a ``rhs`` field, this schedule
  /// stores the resolved per-(stage, block) values; ``add_to_lp``
  /// consults it via ``m_rhs_.optval(stage, block)`` and only falls
  /// back to the expression's parsed scalar when the schedule returns
  /// ``std::nullopt`` for that key.  Empty (``has_value() == false``)
  /// when the source ``UserConstraint`` left ``rhs`` unset.
  OptTBRealSched m_rhs_ {};

  /// Stable storage for the user-supplied slack column label (the
  /// ``slack_name`` field on the underlying ``UserConstraint``, populated
  /// by PAMPL ``var slack_<NAME>;`` declarations or by JSON callers
  /// directly).  Empty when the source ``UserConstraint`` left
  /// ``slack_name`` unset, in which case the LP falls back to the static
  /// ``SlackName`` / ``SlackPosName`` / ``SlackNegName`` constants.
  ///
  /// Owned by the LP instance so the ``std::string_view`` published into
  /// ``SparseCol::variable_name`` is valid for the entire LP lifecycle
  /// (the LinearInterface's label_string_pool only needs the view to be
  /// valid at ``add_col`` time, but downstream label compression code
  /// reads it again).
  std::string m_slack_label_ {};
  std::string m_slack_pos_label_ {};
  std::string m_slack_neg_label_ {};
};

// Pin the data-struct constant value so an accidental rename of the
// `UserConstraint::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"UserConstraint"`).
static_assert(UserConstraintLP::Element::class_name
                  == LPClassName {"UserConstraint"},
              "UserConstraint::class_name must remain \"UserConstraint\"");

}  // namespace gtopt
