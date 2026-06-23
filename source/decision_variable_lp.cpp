/**
 * @file      decision_variable_lp.cpp
 * @brief     LP build path for DecisionVariable elements
 * @date      Mon May 19 21:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * One LP column per (scenario, stage, block) per DecisionVariable
 * object (the default `block` scope).  The column defaults to
 * non-negative (``[0, DblMax]``) so it never emits a *free*
 * (unbounded-below) column that would break GPU first-order solvers; an
 * explicit ``lower_bound`` opts out, and an α-rebased column
 * (``obj_constant`` set) auto-stays free below.  An optional ``cost``
 * adds the column to the LP objective via ``CostHelper::block_ecost`` so
 * the units match the other gtopt LP elements.  Registered with the AMPL
 * resolver under ``decision_variable("X").value`` so UserConstraint
 * expressions can reference it directly.
 *
 * ── Coarse scope (piece 4/5) ──────────────────────────────────────────────
 * A `scope` of `stage` collapses the per-block fan-out to one column per
 * (scenario, stage); `phase` / `global` collapse it further to ONE column
 * per (scene, phase) cell, built in the planning passes
 * (`add_to_phase_lp` / `add_to_global_lp`) instead of the operational
 * per-(scenario, stage) sweep.
 *
 * ── AMPL state variable (piece 5) ─────────────────────────────────────────
 * `state: true` registers a coarse (`stage` / `phase` / `global`) column as
 * a cross-phase `StateVariable` via `SystemContext::add_state_col` and, when
 * `link: true`, defers a link from this phase's column to the previous
 * phase's same-variable column (`defer_state_link`).  It then RIDES the
 * generic SDDP backward pass — coupled across phases and present in every
 * optimality / feasibility cut — with NO engine change.  Guards:
 *   - `state: true` requires `link: true` (hard error otherwise).
 *   - `state: true` must NOT be `block`-scoped (hard error otherwise).
 *   - the StateVariable key uses a DEDICATED class name (`StateClassName`)
 *     so its identity round-trips through cut I/O without colliding with
 *     engine state (reservoir efin element class, the built-in α class).
 */

#include <format>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <gtopt/cost_helper.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/user_constraint_enums.hpp>

namespace gtopt
{

DecisionVariableLP::DecisionVariableLP(const DecisionVariable& pdv,
                                       const InputContext& /*ic*/)
    : ObjectLP<DecisionVariable>(pdv)
{
  // Parse the time-granularity `scope` once at construction so the build
  // path dispatches on a plain enum.  Unset ⇒ Block (legacy behaviour).
  // An unrecognised value is a hard error (same fail-fast policy as
  // UserConstraintLP) — a silent fallback to Block would quietly turn a
  // coarse-scope variable into a per-block one.
  if (pdv.scope.has_value() && !pdv.scope->empty()) {
    const auto parsed = enum_from_name<ConstraintScope>(*pdv.scope);
    if (!parsed.has_value()) {
      throw std::runtime_error(
          std::format("decision_variable '{}': unknown scope '{}' — "
                      "valid values are: block, stage, phase, global",
                      pdv.name,
                      *pdv.scope));
    }
    m_scope_ = *parsed;
  }

  m_is_state_ = pdv.state.value_or(false);
  m_link_ = pdv.link.value_or(false);

  // ── Guard 2: a state variable REQUIRES a link ───────────────────────────
  // An un-linked state variable would silently decouple the phases (its
  // per-phase columns would float free instead of propagating the
  // end-of-phase value into the next phase) — the most insidious failure
  // mode.  Error here at construction so the bad bundle surfaces directly.
  if (m_is_state_ && !m_link_) {
    throw std::runtime_error(
        std::format("decision_variable '{}': `state: true` requires "
                    "`link: true` — an un-linked state variable would "
                    "silently decouple the SDDP phases",
                    pdv.name));
  }

  // ── Guard 3: a state variable must NOT be block-scoped ───────────────────
  // A per-block state column has no single end-of-phase value to propagate
  // across the phase boundary; state variables live at the coarse
  // (stage / phase / global) granularity, like reservoir efin and α.
  if (m_is_state_ && m_scope_ == ConstraintScope::Block) {
    throw std::runtime_error(
        std::format("decision_variable '{}': `state: true` must be "
                    "coarse-scoped (scope = stage, phase or global), NEVER "
                    "block — a per-block column has no single end-of-phase "
                    "value to couple across the SDDP phase boundary",
                    pdv.name));
  }
}

bool DecisionVariableLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  // Phase/global-scoped columns are built ONCE per (scene, phase) cell by the
  // planning passes (`add_to_phase_lp` / `add_to_global_lp`), NOT in this
  // per-(scenario, stage) operational sweep.  Skip them here.
  if (scope_is_planning(m_scope_)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return true;
  }

  static constexpr auto ampl_name = Element::class_name.snake_case();
  static constexpr auto cname = Element::class_name.full_name();

  // Default-non-negative columns: a bare DecisionVariable (no explicit
  // `lower_bound`) defaults to `x >= 0` so it never emits a *free*
  // (unbounded-below) LP column — free columns break GPU first-order /
  // heuristic solvers (cuOpt feasibility-jump, PDLP) which cannot project
  // an unbounded column.  The element opts out by setting `lower_bound`
  // explicitly (free or negative).  (Unchanged from the original
  // operational path — kept ADDITIVE: do not change legacy block/stage
  // behaviour.)
  const auto lower = decision_variable().lower_bound.value_or(0.0);
  const auto upper = decision_variable().upper_bound.value_or(DblMax);
  const auto cost = decision_variable().cost.value_or(0.0);

  // ``cost_type`` decides how ``cost`` folds into the objective (see the
  // header).  Raw (default) = face value; Power = duration-weighted; Energy
  // = prob × discount.
  const auto scale_type = enum_from_name<ConstraintScaleType>(
                              decision_variable().cost_type.value_or("raw"))
                              .value_or(ConstraintScaleType::Raw);

  // Optional single-block scope: when ``block`` is set, the column is
  // created only on that block (end-of-horizon quantities like the FCF
  // ``alpha_fcf`` must be a single last-block variable).
  const auto scope_block = decision_variable().block;

  // Stage scope: collapse the per-block fan-out to a SINGLE column per
  // (scenario, stage), keyed at the stage's first block, with a
  // StageContext.  References to `decision_variable("X").value` then resolve
  // that one column from any block (eini/capainst-style sharing).
  const bool is_stage_scope = (m_scope_ == ConstraintScope::Stage);

  BIndexHolder<ColIndex> vcols;
  map_reserve(
      vcols, (scope_block || is_stage_scope) ? std::size_t {1} : blocks.size());

  // Energy cost factor (probability · discount, no duration); Raw uses the
  // bare ``cost`` (face value).
  const auto energy_factor = CostHelper::cost_factor(
      scenario.probability_factor(), stage.discount_factor());

  for (auto&& block : blocks) {
    if (scope_block && block.uid() != *scope_block) {
      continue;
    }
    double col_cost = 0.0;
    if (cost != 0.0) {
      switch (scale_type) {
        case ConstraintScaleType::Energy:
          col_cost = cost * energy_factor;
          break;
        case ConstraintScaleType::Raw:
          col_cost = cost;  // face value — no prob/discount/duration
          break;
        case ConstraintScaleType::Power:
          col_cost = CostHelper::block_ecost(scenario, stage, block, cost);
          break;
      }
    }
    const auto col = lp.add_col({
        .lowb = lower,
        .uppb = upper,
        .cost = col_cost,
        .cost_scale_type = scale_type,
        .class_name = cname,
        .variable_name = ValueName,
        .variable_uid = uid(),
        .context = is_stage_scope
            ? LpContext {make_stage_context(scenario.uid(), stage.uid())}
            : LpContext {make_block_context(
                  scenario.uid(), stage.uid(), block.uid())},
    });
    vcols[block.uid()] = col;

    // Mean-shift (α-rebase) restitution — add the removed constant back.
    if (const auto oc = decision_variable().obj_constant;
        oc.has_value() && cost != 0.0)
    {
      lp.add_obj_constant(oc.value());
    }

    if (is_stage_scope) {
      // One column for the whole stage; remaining blocks alias it so any
      // per-block reference resolves to the same LP column.
      for (auto&& b : blocks) {
        vcols[b.uid()] = col;
      }
      break;
    }
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  value_cols[st_key] = std::move(vcols);

  // Register PAMPL-visible columns so user constraints can reference
  // ``decision_variable("X").value`` from any (scenario, stage, block).
  if (!value_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), ValueName, scenario, stage, value_cols.at(st_key));
  }

  return true;
}

bool DecisionVariableLP::build_cell_col(SystemContext& sc,
                                        const SceneLP& scene,
                                        const PhaseLP& phase,
                                        LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();
  static constexpr auto cname = Element::class_name.full_name();

  const auto& stages = phase.stages();
  if (stages.empty()) {
    return true;
  }
  // Representative anchor: the phase's LAST stage.  This matches the FCF
  // terminal-cut shape (a single column at the end of the horizon) AND keeps
  // the producing-side StateVariable key consistent with the consuming side:
  // the next phase's `defer_state_link` synthesizes a prev_key from
  // `sc.prev_stage(next_phase.first_stage)`, which is exactly
  // `this_phase.stages().back()`.  Anchoring on the literal `.back()` (not the
  // last *active* stage) guarantees the keys round-trip across the boundary.
  const StageLP* rep_stage = &stages.back();
  if (!is_active(*rep_stage)) {
    return true;  // inactive terminal stage: nothing to anchor in this cell
  }
  const auto& scenarios = scene.scenarios();
  if (scenarios.empty()) {
    return true;
  }
  const auto& scenario = scenarios.front();

  const auto lower = decision_variable().lower_bound.value_or(
      decision_variable().obj_constant.has_value() ? -DblMax : 0.0);
  const auto upper = decision_variable().upper_bound.value_or(DblMax);
  const auto cost = decision_variable().cost.value_or(0.0);
  const auto scale_type = enum_from_name<ConstraintScaleType>(
                              decision_variable().cost_type.value_or("raw"))
                              .value_or(ConstraintScaleType::Raw);

  double col_cost = 0.0;
  if (cost != 0.0) {
    const auto energy_factor = CostHelper::cost_factor(
        scenario.probability_factor(), rep_stage->discount_factor());
    switch (scale_type) {
      case ConstraintScaleType::Energy:
        col_cost = cost * energy_factor;
        break;
      case ConstraintScaleType::Raw:
        col_cost = cost;
        break;
      case ConstraintScaleType::Power:
        // Power weighting needs a block; the cell column has no single block,
        // so fall back to the energy factor (prob × discount).  A power-typed
        // cost on a global column is unusual; document the substitution.
        col_cost = cost * energy_factor;
        break;
    }
  }

  // Planning-cell columns carry a PhaseContext (scene_uid, phase_uid,
  // element_uid) so two distinct global columns in the same cell receive
  // distinct labelable identities.
  const auto cell_ctx = make_phase_context(scene.uid(), phase.uid(), uid());

  SparseCol col {
      .lowb = lower,
      .uppb = upper,
      .cost = col_cost,
      .cost_scale_type = scale_type,
      .class_name = cname,
      .variable_name = ValueName,
      .variable_uid = uid(),
      .context = cell_ctx,
  };

  ColIndex col_idx {unknown_index};
  if (m_is_state_) {
    // Register the cell column as a cross-phase StateVariable so it rides
    // the generic SDDP backward pass.  Use the DEDICATED state class name so
    // its key never collides with engine state in the cut-I/O registry.
    // var_scale = 1.0 (no semantic LP scaling on the user column).
    col_idx = sc.add_state_col(
        lp,
        StateVariable::key(
            scenario, *rep_stage, StateClassName, uid(), ValueName),
        std::move(col),
        0.0 /*scost*/,
        1.0 /*var_scale*/);

    // Defer a link from this phase's column to the previous phase's
    // same-variable column.  `prev_phase != nullptr` ⇒ a real cross-phase
    // boundary; the previous phase's LAST stage is the producing key.  On
    // the first phase there is no previous phase, so no link is deferred
    // (the column is the initial state — free within its bounds).
    if (m_link_) {
      const auto [prev_stage, prev_phase] = sc.prev_stage(stages.front());
      if (prev_phase != nullptr && prev_stage != nullptr) {
        sc.defer_state_link(
            StateVariable::key(
                scenario, *prev_stage, StateClassName, uid(), ValueName),
            col_idx);
      }
    }
  } else {
    col_idx = lp.add_col(std::move(col));
  }

  if (cost != 0.0) {
    if (const auto oc = decision_variable().obj_constant; oc.has_value()) {
      lp.add_obj_constant(oc.value());
    }
  }

  // Store the single cell column keyed at the representative (scenario,
  // stage) so `add_to_output` emits it and AMPL references resolve it.  Every
  // block of the representative stage aliases the one column (a stage with no
  // blocks contributes nothing — the registration below is then skipped).
  BIndexHolder<ColIndex> vcols;
  for (auto&& b : rep_stage->blocks()) {
    vcols[b.uid()] = col_idx;
  }
  const auto st_key = std::tuple {scenario.uid(), rep_stage->uid()};
  value_cols[st_key] = std::move(vcols);

  if (!value_cols.at(st_key).empty()) {
    sc.add_ampl_variable(ampl_name,
                         uid(),
                         ValueName,
                         scenario,
                         *rep_stage,
                         value_cols.at(st_key));
  }

  return true;
}

bool DecisionVariableLP::add_to_phase_lp(SystemContext& sc,
                                         const SceneLP& scene,
                                         const PhaseLP& phase,
                                         LinearProblem& lp)
{
  if (m_scope_ != ConstraintScope::Phase) {
    return true;
  }
  return build_cell_col(sc, scene, phase, lp);
}

bool DecisionVariableLP::add_to_global_lp(SystemContext& sc,
                                          const SceneLP& scene,
                                          const PhaseLP& phase,
                                          LinearProblem& lp)
{
  if (m_scope_ != ConstraintScope::Global) {
    return true;
  }
  return build_cell_col(sc, scene, phase, lp);
}

bool DecisionVariableLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  out.add_col_sol(cname, ValueName, id(), value_cols);
  out.add_col_cost(cname, ValueName, id(), value_cols);
  return true;
}

}  // namespace gtopt
