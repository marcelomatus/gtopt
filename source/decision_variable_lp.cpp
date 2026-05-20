/**
 * @file      decision_variable_lp.cpp
 * @brief     LP build path for DecisionVariable elements
 * @date      Mon May 19 21:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * One LP column per (scenario, stage, block) per DecisionVariable
 * object.  Bounds default to the LP free range (``[-DblMax, DblMax]``);
 * an optional ``cost`` adds the column to the LP objective via
 * ``CostHelper::block_ecost`` so the units match the other gtopt LP
 * elements.  Registered with the AMPL resolver under
 * ``decision_variable("X").value`` so UserConstraint expressions can
 * reference it directly.
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

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

  const auto lower = decision_variable().lower_bound.value_or(-DblMax);
  const auto upper = decision_variable().upper_bound.value_or(DblMax);
  const auto cost = decision_variable().cost.value_or(0.0);

  BIndexHolder<ColIndex> vcols;
  map_reserve(vcols, blocks.size());

  for (auto&& block : blocks) {
    const auto col = lp.add_col({
        .lowb = lower,
        .uppb = upper,
        .cost = (cost != 0.0)
            ? CostHelper::block_ecost(scenario, stage, block, cost)
            : 0.0,
        .class_name = cname,
        .variable_name = ValueName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    vcols[block.uid()] = col;
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
