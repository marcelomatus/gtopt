/**
 * @file      converter_lp.cpp
 * @brief     Header of
 * @date      Wed Apr  2 02:12:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/converter_lp.hpp>
#include <gtopt/element_context.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>

namespace gtopt
{

ConverterLP::ConverterLP(InputContext& ic, Converter pconverter)
    : CapacityBase(ic, ClassName, std::move(pconverter))
    , conversion_rate(
          ic, ClassName, id(), std::move(converter().conversion_rate))
    , generator_index(
          ic.make_element_index<GeneratorLP>(converter(), generator()))
    , demand_index(ic.make_element_index<DemandLP>(converter(), demand()))
    , battery_index(ic.element_index(battery()))
{
}

bool ConverterLP::add_to_lp(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  constexpr std::string_view cname = "conv";

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp, cname)) [[unlikely]] {
    return false;
  }

  const auto stage_index = stage.index();
  const auto scenario_index = scenario.index();

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto [stage_capacity, capacity_col] = capacity_and_col(stage, lp);

  const auto stage_conversion_rate =
      conversion_rate.at(stage.uid()).value_or(1.0);

  auto&& blocks = stage.blocks();

  auto&& generator = sc.element(generator_index);
  auto&& gen_cols = generator.generation_cols_at(scenario, stage);

  auto&& demand = sc.element(demand_index);
  auto&& load_cols = demand.load_cols_at(scenario_index, stage_index);

  auto&& battery = sc.element(battery_index);
  auto&& flow_cols = battery.flow_cols_at(scenario_index, stage_index);

  BIndexHolder rrows;
  rrows.reserve(blocks.size());
  BIndexHolder crows;
  crows.reserve(blocks.size());

  for (const auto& [block, gcol, lcol, fcol] :
       std::views::zip(blocks, gen_cols, load_cols, flow_cols))
  {
    SparseRow rrow {
        .name = sc.stb_label(scenario, stage, block, cname, "conv", uid())};

    rrow[fcol] = -stage_conversion_rate;
    rrow[gcol] = +1;
    rrow[lcol] = -1;

    rrows.push_back(lp.add_row(std::move(rrow.equal(0))));

    // adding the capacity constraint
    if (capacity_col.has_value()) {
      SparseRow crow {
          .name = sc.stb_label(scenario, stage, block, cname, "cap", uid())};

      crow[capacity_col.value()] = 1;
      crow[gcol] = -1;
      crow[lcol] = -1;

      crows.push_back(lp.add_row(std::move(crow.greater_equal(0))));
    }
  }

  return emplace_bholder(scenario, stage, capacity_rows, std::move(crows))
             .second
      && emplace_bholder(scenario, stage, conversion_rows, std::move(rrows))
             .second;
}

bool ConverterLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_row_dual(cname, "conversion", id(), conversion_rows);
  out.add_row_dual(cname, "capacity", id(), capacity_rows);

  return CapacityBase::add_to_output(out, cname);
}

}  // namespace gtopt
