/**
 * @file      decision_variable_lp.cpp
 * @brief     LP build path for DecisionVariable elements
 * @date      Mon May 19 21:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * One LP column per (scenario, stage, block) per DecisionVariable
 * object.  The column defaults to non-negative (``[0, DblMax]``) so it
 * never emits a *free* (unbounded-below) column that would break GPU
 * first-order solvers; an explicit ``lower_bound`` opts out, and an
 * α-rebased column (``obj_constant`` set) auto-stays free below.
 * An optional ``cost`` adds the column to the LP objective via
 * ``CostHelper::block_ecost`` so the units match the other gtopt LP
 * elements.  Registered with the AMPL resolver under
 * ``decision_variable("X").value`` so UserConstraint expressions can
 * reference it directly.
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/user_constraint_enums.hpp>

namespace gtopt
{

bool DecisionVariableLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  if (!is_active(stage)) {
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
  // explicitly (free or negative).
  //
  // The FCF cost-to-go `alpha` is NOT a DecisionVariable — it is the native
  // boundary-cut state variable registered by the monolithic / SDDP
  // boundary-cut loader from `boundary_cuts.csv`.  Nothing FCF/α-related is
  // special-cased here; any `alpha_fcf` DecisionVariable appearing in a
  // bundle is a stale converter artifact (current plexos2gtopt filters it).
  const auto lower = decision_variable().lower_bound.value_or(0.0);
  const auto upper = decision_variable().upper_bound.value_or(DblMax);
  const auto cost = decision_variable().cost.value_or(0.0);

  // ``cost_type`` decides how ``cost`` folds into the objective:
  //  - Raw (default): ``value`` is a face-value money / unitless amount
  //    (e.g. a PLEXOS penalty knob, reserve VoRS, or the FCF cost-to-go
  //    ``alpha_fcf``) → ``cost`` as-is, added to the objective at face
  //    value (NO probability, discount, or duration).  This is the right
  //    default for the general PLEXOS DecisionVariables, which are
  //    discrete/face-value penalties — Δt-weighting them (the old "power"
  //    default) over-charged them by the block length.
  //  - Power: ``value`` is a rate (MW) → ``cost · prob · discount ·
  //    block_duration`` (``block_ecost``).
  //  - Energy: ``value`` is a total energy (MWh) → ``cost · prob ·
  //    discount`` (NO duration multiply).
  // Avoids the ``cost = 1/duration`` magic correction for non-power vars.
  const auto scale_type = enum_from_name<ConstraintScaleType>(
                              decision_variable().cost_type.value_or("raw"))
                              .value_or(ConstraintScaleType::Raw);

  // Optional single-block scope: when ``block`` is set, the column is
  // created only on that block (end-of-horizon quantities like the FCF
  // ``alpha_fcf`` must be a single last-block variable — a per-block
  // column would let the unconstrained blocks distort the objective).
  const auto scope_block = decision_variable().block;

  BIndexHolder<ColIndex> vcols;
  map_reserve(vcols, scope_block ? std::size_t {1} : blocks.size());

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
        // Mirror the objective time-basis onto the column so the reduced-cost
        // readback in OutputContext is the exact inverse of the fold above
        // (Power → ÷duration, Energy → ÷(prob·disc), Raw → face value, e.g.
        // alpha_fcf / VoRS penalties).
        .cost_scale_type = scale_type,
        .class_name = cname,
        .variable_name = ValueName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    vcols[block.uid()] = col;

    // Mean-shift (α-rebase) restitution: the column holds the rebased
    // ``value' = value − obj_constant``; ``obj_constant`` is the physical
    // dollar amount the rebase removed from the objective.  Add it back
    // verbatim so the reported objective equals the un-rebased model.
    // Added once per created column — intended for single-``block`` vars.
    if (const auto oc = decision_variable().obj_constant;
        oc.has_value() && cost != 0.0)
    {
      lp.add_obj_constant(oc.value());
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

bool DecisionVariableLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  out.add_col_sol(cname, ValueName, id(), value_cols);
  out.add_col_cost(cname, ValueName, id(), value_cols);
  return true;
}

}  // namespace gtopt
