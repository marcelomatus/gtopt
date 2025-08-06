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

ConverterLP::ConverterLP(Converter pconverter, InputContext& ic)
    : CapacityBase(std::move(pconverter), ic, ClassName)
    , conversion_rate(
          ic, ClassName.name, id(), std::move(converter().conversion_rate))
{
}

bool ConverterLP::add_to_lp(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name;

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) [[unlikely]] {
    return false;
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto [stage_capacity, capacity_col] = capacity_and_col(stage, lp);

  const auto stage_conversion_rate =
      conversion_rate.at(stage.uid()).value_or(1.0);

  auto&& blocks = stage.blocks();

  auto&& generator = sc.element<GeneratorLP>(generator_sid());
  auto&& gen_cols = generator.generation_cols_at(scenario, stage);

  auto&& demand = sc.element<DemandLP>(demand_sid());
  auto&& load_cols = demand.load_cols_at(scenario, stage);

  auto&& battery = sc.element<BatteryLP>(battery_sid());
  auto&& flow_cols = battery.flow_cols_at(scenario, stage);

  BIndexHolder<RowIndex> rrows;
  BIndexHolder<RowIndex> crows;
  rrows.reserve(blocks.size());
  crows.reserve(blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto fcol = flow_cols.at(buid);
    const auto gcol = gen_cols.at(buid);
    const auto lcol = load_cols.at(buid);

    auto rrow = SparseRow {.name = sc.lp_label(
                               scenario, stage, block, cname, "conv", uid())}
                    .equal(0);

    rrow[fcol] = -stage_conversion_rate;
    rrow[gcol] = +1;
    rrow[lcol] = -1;

    rrows[buid] = lp.add_row(std::move(rrow));

    // adding the capacity constraint
    if (capacity_col) {
      auto crow = SparseRow {.name = sc.lp_label(
                                 scenario, stage, block, cname, "cap", uid())}
                      .greater_equal(0);

      crow[*capacity_col] = 1;
      crow[gcol] = -1;
      crow[lcol] = -1;

      crows[buid] = lp.add_row(std::move(crow));
    }
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  capacity_rows[st_key] = std::move(crows);
  conversion_rows[st_key] = std::move(rrows);

  return true;
}

bool ConverterLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.name;

  out.add_row_dual(cname, "conversion", id(), conversion_rows);
  out.add_row_dual(cname, "capacity", id(), capacity_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
