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

#include <gtopt/cost_helper.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

FlowLP::FlowLP(const Flow& pflow, const InputContext& ic)
    : ObjectLP<Flow>(pflow)
    , discharge(ic, Element::class_name, id(), std::move(flow().discharge))
    , fcost(ic, Element::class_name, id(), std::move(flow().fcost))
{
}

bool FlowLP::add_to_lp(const SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

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

  // ``--lp-reduction``: fold FIXED-discharge flows (constants) into the
  // junction-balance RHS instead of emitting a fixed singleton column.
  const bool reduce_fixed_flow = sc.options().lp_reduction();

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    // ``discharge`` is OPTIONAL — when set, the column is FORCED to
    // exactly ``discharge`` (legacy hard equality, regardless of
    // ``fcost``).  When unset, the column is a free slack ``[0,
    // +inf)`` priced at ``fcost`` so the LP only activates it when
    // the junction balance demands it.  ``fcost`` itself is also
    // optional — when neither field is set, no LP column is emitted.
    const auto block_discharge_opt =
        discharge.at(scenario.uid(), stage.uid(), block.uid());
    const auto block_fcost = fcost.optval(stage.uid(), buid);
    if (!block_discharge_opt.has_value() && !block_fcost.has_value()) {
      continue;
    }

    // lp_reduction: a FIXED-discharge flow (discharge set, no fcost) is a
    // constant.  Fold ±discharge into the junction-balance RHS rather than
    // emit a fixed [d, d] singleton column (PaPILO-style singleton
    // substitution).  Dual-transparent: the balance row's dual (water value /
    // LMP) is unchanged.  Skipped when fcost is set (a soft slack stays a
    // column) or there is no junction (flow-turbine mode keeps the column).
    if (reduce_fixed_flow && junction_ptr != nullptr
        && block_discharge_opt.has_value() && !block_fcost.has_value())
    {
      const double d = *block_discharge_opt;
      const auto& balance_rows = junction_ptr->balance_rows_at(scenario, stage);
      auto& brow = lp.row_at(balance_rows.at(buid));
      // The balance LHS would receive (is_input ? +1 : -1)·d; moving that
      // constant to the RHS gives (is_input ? -d : +d).
      const double delta = is_input() ? -d : d;
      brow.lowb += delta;
      brow.uppb += delta;
      continue;  // no LP column emitted for this fixed flow
    }

    const double block_lowb = block_discharge_opt.value_or(0.0);
    const double block_uppb = block_discharge_opt.value_or(DblMax);
    const double block_cost = block_fcost.has_value()
        ? CostHelper::block_ecost(scenario, stage, block, *block_fcost)
        : 0.0;

    const auto fcol = lp.add_col({
        .lowb = block_lowb,
        .uppb = block_uppb,
        .cost = block_cost,
        .class_name = Element::class_name.full_name(),
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

  // Register PAMPL-visible columns.
  if (!flow_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), FlowName, scenario, stage, flow_cols.at(st_key));
  }

  return true;
}

bool FlowLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

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
