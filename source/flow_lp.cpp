/**
 * @file      flow_lp.cpp
 * @brief     Implementation of FlowLP methods
 * @date      Wed Jul 30 15:56:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains implementation of:
 * - FlowLP construction and initialization
 * - Adding flow variables to LP problems
 * - Managing flow constraints in junctions
 * - Output generation for flow solutions
 */

#include <functional>
#include <optional>

#include <gtopt/flow_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

FlowLP::FlowLP(const Flow& pflow, const InputContext& ic)
    : ObjectLP<Flow>(pflow)
    , discharge(ic, ClassName, id(), std::move(flow().discharge))
{
}

bool FlowLP::add_to_lp(const SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  // Junction is optional: flow-turbine mode has no junction.
  const JunctionLP* junction_ptr = nullptr;
  if (has_junction()) {
    junction_ptr = &sc.element<JunctionLP>(junction_sid());
    if (!junction_ptr->is_active(stage)) {
      return true;
    }
  }

  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> fcols;
  map_reserve(fcols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    //  adding flow variable
    const auto block_discharge =
        discharge.at(scenario.uid(), stage.uid(), block.uid());

    const auto fcol = lp.add_col({
        .lowb = block_discharge,
        .uppb = block_discharge,
        .class_name = ClassName.full_name(),
        .variable_name = FlowName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    fcols[buid] = fcol;

    // adding flow to the junction balances (only when junction exists)
    if (junction_ptr != nullptr) {
      const auto& balance_rows = junction_ptr->balance_rows_at(scenario, stage);
      auto& brow = lp.row_at(balance_rows.at(buid));
      brow[fcol] = is_input() ? 1 : -1;
    }
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  flow_cols[st_key] = std::move(fcols);

  return true;
}

bool FlowLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();

  out.add_col_sol(cname, FlowName, id(), flow_cols);
  out.add_col_cost(cname, FlowName, id(), flow_cols);

  return true;
}

bool FlowLP::update_aperture(
    LinearInterface& li,
    const ScenarioLP& base_scenario,
    const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
    const StageLP& stage) const
{
  if (!is_active(stage)) {
    return true;
  }

  const auto st_key = std::tuple {base_scenario.uid(), stage.uid()};
  const auto it = flow_cols.find(st_key);
  if (it == flow_cols.end()) {
    return true;  // no columns registered for this (scenario, stage)
  }

  const auto& fcols = it->second;
  for (const auto& [block_uid, col] : fcols) {
    const auto new_val = value_fn(stage.uid(), block_uid);
    if (new_val.has_value()) {
      li.set_col_low(col, *new_val);
      li.set_col_upp(col, *new_val);
    }
    // If nullopt, keep the forward-pass value (no change)
  }

  return true;
}

}  // namespace gtopt
