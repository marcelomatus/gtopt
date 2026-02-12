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

#include <gtopt/flow_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

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
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& junction = sc.element<JunctionLP>(junction_sid());
  if (!junction.is_active(stage)) {
    return true;
  }

  const auto& balance_rows = junction.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> fcols;
  map_reserve(fcols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    //  adding flow variable
    const auto block_discharge =
        discharge.at(scenario.uid(), stage.uid(), block.uid());

    auto col_name = sc.lp_label(scenario, stage, block, cname, "flow", uid());
    const auto fcol = lp.add_col({
        .name = std::move(col_name),
        .lowb = block_discharge,
        .uppb = block_discharge,
    });
    fcols[buid] = fcol;

    // adding flow to the junction balances
    auto& brow = lp.row_at(balance_rows.at(buid));
    brow[fcol] = is_input() ? 1 : -1;
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  flow_cols[st_key] = std::move(fcols);

  return true;
}

bool FlowLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();

  out.add_col_sol(cname, "flow", id(), flow_cols);
  out.add_col_cost(cname, "flow", id(), flow_cols);

  return true;
}

}  // namespace gtopt
