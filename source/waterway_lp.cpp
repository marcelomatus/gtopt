/**
 * @file      waterway_lp.cpp
 * @brief     Implementation of waterway LP formulation
 * @date      Wed Jul 30 12:02:36 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements WaterwayLP construction and add_to_lp, which
 * builds LP flow variables and constraints between hydro junctions.
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

WaterwayLP::WaterwayLP(const Waterway& pwaterway, const InputContext& ic)
    : ObjectLP<Waterway>(pwaterway)
    , fmin(ic, ClassName, id(), std::move(waterway().fmin))
    , fmax(ic, ClassName, id(), std::move(waterway().fmax))
    , capacity(ic, ClassName, id(), std::move(waterway().capacity))
    , lossfactor(ic, ClassName, id(), std::move(waterway().lossfactor))
{
}

bool WaterwayLP::add_to_lp(const SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp)
{
  static const auto ampl_name = std::string {ClassName.snake_case()};

  if (!is_active(stage)) {
    return true;
  }

  if (junction_a_sid() == junction_b_sid()) {
    return true;
  }

  const auto& junction_a = sc.element<JunctionLP>(junction_a_sid());
  const auto& junction_b = sc.element<JunctionLP>(junction_b_sid());
  if (!junction_a.is_active(stage) || !junction_b.is_active(stage)) {
    return true;
  }

  const auto& balance_rows_a = junction_a.balance_rows_at(scenario, stage);
  const auto& balance_rows_b = junction_b.balance_rows_at(scenario, stage);

  const auto stage_capacity =
      capacity.at(stage.uid()).value_or(LinearProblem::DblMax);
  const auto stage_lossfactor = sc.stage_lossfactor(stage, lossfactor);

  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> fcols;
  map_reserve(fcols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const auto balance_row_a = balance_rows_a.at(buid);
    const auto balance_row_b = balance_rows_b.at(buid);

    const auto [block_fmax, block_fmin] =
        sc.block_maxmin_at(stage, block, fmax, fmin, stage_capacity);

    auto& brow_a = lp.row_at(balance_row_a);
    auto& brow_b = lp.row_at(balance_row_b);

    //  adding flow variable

    const auto fc = lp.add_col({
        .lowb = block_fmin,
        .uppb = block_fmax,
        .class_name = ClassName.full_name(),
        .variable_name = FlowName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });

    fcols[buid] = fc;

    // adding flow to the junction balances, including the losses
    brow_a[fc] = -1;
    brow_b[fc] = +1 - stage_lossfactor;
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  flow_cols[st_key] = std::move(fcols);

  // Register PAMPL-visible columns.
  if (!flow_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), FlowName, scenario, stage, flow_cols.at(st_key));
  }

  return true;
}

bool WaterwayLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, FlowName, pid, flow_cols);
  out.add_col_cost(cname, FlowName, pid, flow_cols);

  return true;
}

}  // namespace gtopt
