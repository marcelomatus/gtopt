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

class SceneLP;
class PhaseLP;
class BlockLP;

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

  /// Planning-level build for `phase`/`global`-scoped constraints — one LP
  /// row per (scene, phase) cell.  Dispatched by the planning pass
  /// (`add_to_planning_lp`) AFTER the per-(scenario, stage) operational
  /// sweep, so coarse rows can reference any column the stage sweep created.
  /// `phase`-scope routes here; `global`-scope routes through
  /// `add_to_global_lp` below.  Block/stage-scoped constraints do nothing
  /// here (they are built in `add_to_lp`).  See `ConstraintScope`.
  [[nodiscard]] bool add_to_phase_lp(const SystemContext& sc,
                                     const SceneLP& scene,
                                     const PhaseLP& phase,
                                     LinearProblem& lp);

  /// Planning-level build for `global`-scoped constraints — one un-indexed
  /// LP row per (scene, phase) cell (the FCF α terminal-cut / annual-cap
  /// shape).  Runs in the global sweep, which the planning pass executes
  /// after the phase sweep so a global row can reference state columns the
  /// phase sweep registered.  Block/stage/phase-scoped constraints do
  /// nothing here.
  [[nodiscard]] bool add_to_global_lp(const SystemContext& sc,
                                      const SceneLP& scene,
                                      const PhaseLP& phase,
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

  // ── Additive read-only accessors (used by UserModelLP) ──────────────────
  // UserModelLP drives an internal UserConstraintLP per bundled constraint
  // and re-emits its rows/slacks under `output/UserModel/<tag>/...` instead
  // of `UserConstraint/`.  These getters expose the LP-build state read by
  // `add_to_output` WITHOUT changing any standalone behaviour.

  /// Per-(scenario, stage) row indices produced by the build path.
  [[nodiscard]] constexpr const STBIndexHolder<RowIndex>& rows_holder()
      const noexcept
  {
    return m_rows_;
  }

  /// Per-(scenario, stage) soft-constraint slack columns (`slack`/`slack_pos`).
  [[nodiscard]] constexpr const STBIndexHolder<ColIndex>& slack_cols_holder()
      const noexcept
  {
    return m_slack_cols_;
  }

  /// Per-(scenario, stage) negative-deviation slack columns (`slack_neg`).
  [[nodiscard]] constexpr const STBIndexHolder<ColIndex>&
  slack_neg_cols_holder() const noexcept
  {
    return m_slack_neg_cols_;
  }

  /// Dual-output scaling parsed from `constraint_type`.
  [[nodiscard]] constexpr ConstraintScaleType scale_type() const noexcept
  {
    return m_scale_type_;
  }

private:
  /// Shared builder for the COARSE-scope paths (`stage`/`phase`/`global`):
  /// resolves the expression at the supplied representative
  /// (scenario, stage, block), folds in the RHS override + soft slack,
  /// stamps the row with @p row_ctx, adds it, and records it (+ any slack
  /// cols) under the (scenario, stage) key keyed at @p block.  Returns true
  /// always (a row with no resolved columns is silently skipped, matching
  /// the per-block path).  `block`-scope does NOT use this — it has its own
  /// per-block loop in `add_to_lp`.
  [[nodiscard]] bool build_coarse_row(const SystemContext& sc,
                                      const ScenarioLP& scenario,
                                      const StageLP& stage,
                                      const BlockLP& block,
                                      const LpContext& row_ctx,
                                      LinearProblem& lp);

  /// Shared backend for `add_to_phase_lp` / `add_to_global_lp`: build ONE
  /// `PhaseContext`-stamped LP row per (scene, phase) cell, anchored at the
  /// cell's LAST in-domain stage / LAST in-domain block under the FIRST
  /// in-domain scenario (the FCF terminal-cut shape).  Returns true even
  /// when nothing is in-domain (no rows).
  [[nodiscard]] bool build_phase_cell_row(const SystemContext& sc,
                                          const SceneLP& scene,
                                          const PhaseLP& phase,
                                          LinearProblem& lp);

  /// Cached parsed expression (nullopt if expression was empty or invalid)
  std::optional<ConstraintExpr> m_expr_ {};
  /// How the LP dual should be scaled for output
  ConstraintScaleType m_scale_type_ {ConstraintScaleType::Power};
  /// How the `penalty` scalar is converted to the slack-column cost.
  /// Parsed once at construction; per-block conversion is applied inside
  /// the `add_to_lp` block loop when the constraint is soft.
  PenaltyClass m_penalty_class_ {PenaltyClass::Raw};
  /// Time-granularity at which the constraint's LP rows are instantiated
  /// (`block` default, `stage`, `phase`, `global`).  Parsed once at
  /// construction from the underlying `UserConstraint::scope` field.
  /// `Block`/`Stage` route through `add_to_lp`; `Phase`/`Global` route
  /// through the planning passes (`add_to_phase_lp` / `add_to_global_lp`).
  ConstraintScope m_scope_ {ConstraintScope::Block};
  /// Per-(scenario, stage) row indices produced by add_to_lp.  For
  /// `block`-scope each (scenario, stage) maps a per-block holder; for
  /// `stage`-scope each (scenario, stage) maps a single-entry holder keyed
  /// at the stage's first block.  For `phase`/`global`-scope the planning
  /// passes store the single per-cell row here keyed at the cell's first
  /// (scenario, stage, block) so `add_to_output` reads it uniformly.
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
