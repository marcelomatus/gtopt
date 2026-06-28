/**
 * @file      user_model_lp.cpp
 * @brief     LP build / output path for UserModel (generic AMPL capture)
 * @date      Sun Jun 22 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `UserModelLP` is an additive aggregation: it owns one internal
 * `DecisionVariableLP` per declared variable and one internal
 * `UserConstraintLP` per declared constraint, and delegates the LP build to
 * them verbatim.  Every reused side-effect (the shared expr resolver, the
 * `add_ampl_variable` routing, the `silent_flatten_pass()` rebuild gate, the
 * soft-slack folding, the scope routing, the abs/min/max aux-col lowering)
 * runs in the SAME code paths as the standalone elements — no parallel
 * registry, no forked AST.  Only the OUTPUT is re-namespaced.
 *
 * ── Aux-col policy (P1, decided 2026-06-22) ───────────────────────────────
 * `abs(x)` / `min` / `max` lowering in `user_constraint_lp.cpp` creates
 * `add_aux_col` columns (`abs_aux` / `min_aux` / `max_aux`) plus helper rows
 * that are NOT registered in the AMPL variable registry and carry no stable
 * user-facing name.  `UserModel` captures ONLY the NAMED user cols
 * (`DecisionVariable.value`) and NAMED rows (`UserConstraint.constraint` and
 * its slacks).  The aux cols/rows are treated as **internal / non-captured**
 * LP-implementation detail: they have no tag-name to file under, and their
 * physical content is fully recoverable from the named row's dual.  This is
 * the simplest correct policy and matches what the standalone
 * `UserConstraintLP::add_to_output` already does (it never emits aux cols).
 */

#include <string>
#include <utility>

#include <gtopt/output_context.hpp>
#include <gtopt/user_model_lp.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

namespace
{
/// Compose the stable output field stem `<tag>/<name>` for one bundled
/// declaration.  Owned by the LP instance so the `string_view` published into
/// the `OutputContext` field key stays valid for the whole solve.
[[nodiscard]] std::string make_field(std::string_view tag,
                                     std::string_view name)
{
  return std::string {tag} + "/" + std::string {name};
}

/// Output namespacing tag: explicit `tag`, else the element name (so a capture
/// is always namespaced under a non-empty directory).
[[nodiscard]] std::string resolve_tag(const UserModel& um)
{
  return (um.tag.has_value() && !um.tag->empty()) ? *um.tag : um.name;
}
}  // namespace

UserModelLP::UserModelLP(const UserModel& um, InputContext& ic)
    : ObjectLP<UserModel>(um)
    , m_tag_(resolve_tag(um))
{
  m_variables_.reserve(um.variable_array.size());
  m_var_fields_.reserve(um.variable_array.size());
  for (const auto& dv : um.variable_array) {
    m_var_fields_.emplace_back(make_field(m_tag_, dv.name));
    m_variables_.emplace_back(dv, ic);
  }

  m_constraints_.reserve(um.constraint_array.size());
  m_con_fields_.reserve(um.constraint_array.size());
  for (const auto& uc : um.constraint_array) {
    auto field = make_field(m_tag_, uc.name);
    auto slack = field + "/slack";
    auto slack_neg = field + "/slack_neg";
    m_con_fields_.emplace_back(ConFields {
        .field = std::move(field),
        .slack = std::move(slack),
        .slack_neg = std::move(slack_neg),
    });
    m_constraints_.emplace_back(uc, ic);
  }
}

bool UserModelLP::add_to_lp(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  // Variables first so any constraint that references
  // `decision_variable("X").value` resolves the column the variable just
  // registered (the standalone collection order DecisionVariableLP →
  // UserConstraintLP, reproduced inside the bundle).
  for (auto& dv : m_variables_) {
    if (!dv.add_to_lp(sc, scenario, stage, lp)) {
      return false;
    }
  }
  for (auto& uc : m_constraints_) {
    if (!uc.add_to_lp(sc, scenario, stage, lp)) {
      return false;
    }
  }
  return true;
}

bool UserModelLP::add_to_phase_lp(SystemContext& sc,
                                  const SceneLP& scene,
                                  const PhaseLP& phase,
                                  LinearProblem& lp)
{
  // Variables FIRST so a phase/global-scoped constraint that references a
  // phase/global-scoped (e.g. `state`) variable resolves the column the
  // variable just registered — reproducing the standalone collection order
  // (DecisionVariableLP → UserConstraintLP) inside the planning pass.  A
  // coarse `phase`-scope variable (incl. an AMPL `state` α) builds its single
  // (scene, phase) column here; block/stage-scoped variables already built in
  // `add_to_lp` and no-op here.
  for (auto& dv : m_variables_) {
    if (!dv.add_to_phase_lp(sc, scene, phase, lp)) {
      return false;
    }
  }
  for (auto& uc : m_constraints_) {
    if (!uc.add_to_phase_lp(sc, scene, phase, lp)) {
      return false;
    }
  }
  return true;
}

bool UserModelLP::add_to_global_lp(SystemContext& sc,
                                   const SceneLP& scene,
                                   const PhaseLP& phase,
                                   LinearProblem& lp)
{
  for (auto& dv : m_variables_) {
    if (!dv.add_to_global_lp(sc, scene, phase, lp)) {
      return false;
    }
  }
  for (auto& uc : m_constraints_) {
    if (!uc.add_to_global_lp(sc, scene, phase, lp)) {
      return false;
    }
  }
  return true;
}

bool UserModelLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

  // ── Variables: re-emit each delegate's :sol / :cost under the tag ────────
  for (const auto& [i, dv] : enumerate(m_variables_)) {
    const auto& vcols = dv.value_cols_holder();
    if (vcols.empty()) {
      continue;
    }
    // Field stem `<tag>/<var-name>`; the Id (uid + name) keys the rows so two
    // bundles with the same tag never overwrite each other on disk.
    const std::string_view field = m_var_fields_[i];
    const Id vid {dv.uid(), dv.decision_variable().name};
    out.add_col_sol(cname, field, vid, vcols);
    out.add_col_cost(cname, field, vid, vcols);
  }

  // ── Constraints: re-emit each delegate's :dual (+ :slack) under the tag ──
  for (const auto& [i, uc] : enumerate(m_constraints_)) {
    const auto& rows = uc.rows_holder();
    if (rows.empty()) {
      continue;
    }
    const auto& fields = m_con_fields_[i];
    const Id cid {uc.uid(), uc.user_constraint().name};

    if (uc.scale_type() == ConstraintScaleType::Raw) {
      out.add_row_dual_raw(cname, fields.field, cid, rows);
    } else {
      out.add_row_dual(cname, fields.field, cid, rows);
    }

    // Auto-created soft slacks — same visible-slack contract as the
    // standalone UserConstraintLP (primal value + realized cost), under the
    // `<tag>/<constraint-name>/slack` sub-stem (owned string, solve-long view).
    const auto& slack = uc.slack_cols_holder();
    const auto& slack_neg = uc.slack_neg_cols_holder();
    if (!slack.empty()) {
      out.add_col_sol(cname, fields.slack, cid, slack);
      out.add_col_cost(cname, fields.slack, cid, slack);
    }
    if (!slack_neg.empty()) {
      out.add_col_sol(cname, fields.slack_neg, cid, slack_neg);
      out.add_col_cost(cname, fields.slack_neg, cid, slack_neg);
    }
  }

  return true;
}

}  // namespace gtopt
