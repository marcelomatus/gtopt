/**
 * @file      pump_lp.cpp
 * @brief     Implementation of PumpLP class
 * @date      Sat Apr 12 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the PumpLP class which provides the linear
 * programming representation of hydraulic pumps, including their
 * constraints and relationships with other system components.
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/pump_lp.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
PumpLP::PumpLP(const Pump& ppump, InputContext& ic)
    : ObjectLP<Pump>(ppump)
    , pump_factor(ic, Element::class_name, id(), std::move(pump().pump_factor))
    , efficiency(ic, Element::class_name, id(), std::move(pump().efficiency))
    , capacity(ic, Element::class_name, id(), std::move(pump().capacity))
{
}

/**
 * @brief Adds pump constraints to the linear problem
 *
 * @param sc System context containing component relationships
 * @param scenario Current scenario being processed
 * @param stage Current stage being processed
 * @param lp Linear problem to add constraints to
 * @return true if successful, false on error
 *
 * Adds constraints that enforce the relationship between electrical power
 * consumed by the pump and water flow through the waterway.
 *
 * Conversion constraint (per block):
 *   pump_factor × flow - load <= 0
 *
 * This means: load >= pump_factor × flow, i.e. the pump must consume
 * at least pump_factor MW per m³/s of water pumped.
 *
 * Optional capacity constraint (per block):
 *   flow <= capacity
 */
bool PumpLP::add_to_lp(const SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  const auto stage_pump_factor = pump_factor.at(stage.uid()).value_or(1.0);
  const auto stage_efficiency = efficiency.at(stage.uid()).value_or(1.0);

  // Effective conversion: MW consumed per m³/s pumped, adjusted by efficiency.
  // Higher efficiency means less power per unit flow.
  const auto stage_conversion_rate = stage_pump_factor / stage_efficiency;

  const auto stage_capacity = capacity.at(stage.uid());

  auto&& blocks = stage.blocks();

  const auto& waterway = sc.element<WaterwayLP>(waterway_sid());
  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);

  const auto& demand = sc.element<DemandLP>(demand_sid());
  const auto& load_cols = demand.load_cols_at(scenario, stage);

  BIndexHolder<RowIndex> rrows;
  BIndexHolder<RowIndex> crows;
  map_reserve(rrows, blocks.size());
  map_reserve(crows, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const auto fcol = flow_cols.at(buid);
    const auto lcol = load_cols.at(buid);

    // Conversion constraint: pump_factor/efficiency × flow - load <= 0
    // Equivalently: load >= (pump_factor / efficiency) × flow
    auto rrow = SparseRow {
        .class_name = Element::class_name.short_name(),
        .constraint_name = ConversionName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    };
    rrow[fcol] = stage_conversion_rate;
    rrow[lcol] = -1;

    rrows[buid] = lp.add_row(std::move(rrow.less_equal(0)));

    if (stage_capacity) {
      auto crow =
          SparseRow {
              .class_name = Element::class_name.short_name(),
              .constraint_name = CapacityName,
              .variable_uid = uid(),
              .context =
                  make_block_context(scenario.uid(), stage.uid(), block.uid()),
          }
              .less_equal(*stage_capacity);
      crow[fcol] = 1;

      crows[buid] = lp.add_row(std::move(crow));
    }
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  conversion_rows[st_key] = std::move(rrows);
  capacity_rows[st_key] = std::move(crows);

  return true;
}

/**
 * @brief Adds pump dual variables to the output context
 *
 * @param out Output context to write results to
 * @return true if successful, false on error
 *
 * Outputs the dual variables associated with the pump's conversion
 * rate constraints for sensitivity analysis.
 */
bool PumpLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.short_name();

  out.add_row_dual(cname, ConversionName, id(), conversion_rows);
  out.add_row_dual(cname, CapacityName, id(), capacity_rows);

  return true;
}

}  // namespace gtopt
