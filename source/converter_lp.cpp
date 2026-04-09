/**
 * @file      converter_lp.cpp
 * @brief     Implementation of converter LP formulation
 * @date      Wed Apr  2 02:12:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements ConverterLP construction and add_to_lp, which
 * builds LP variables and constraints for energy conversion rate coupling.
 */

#include <gtopt/converter_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

ConverterLP::ConverterLP(const Converter& pconverter, InputContext& ic)
    : CapacityBase(pconverter, ic, ClassName)
    , conversion_rate(
          ic, ClassName, id(), std::move(converter().conversion_rate))
{
}

bool ConverterLP::add_to_lp(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) [[unlikely]] {
    return false;
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto [opt_capacity, capacity_col] = capacity_and_col(stage, lp);

  const auto stage_conversion_rate =
      conversion_rate.at(stage.uid()).value_or(1.0);

  auto&& blocks = stage.blocks();

  auto&& generator = sc.element<GeneratorLP>(generator_sid());
  auto&& gen_cols = generator.generation_cols_at(scenario, stage);

  auto&& demand = sc.element<DemandLP>(demand_sid());
  auto&& demand_cols = demand.load_cols_at(scenario, stage);

  auto&& battery = sc.element<BatteryLP>(battery_sid());
  auto&& finp_cols = battery.finp_cols_at(scenario, stage);
  auto&& fout_cols = battery.fout_cols_at(scenario, stage);

  BIndexHolder<RowIndex> grows;
  BIndexHolder<RowIndex> drows;
  BIndexHolder<RowIndex> crows;
  map_reserve(grows, blocks.size());
  map_reserve(drows, blocks.size());
  map_reserve(crows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();

    const auto block_context =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    const auto gcol = gen_cols.at(buid);
    {
      auto grow =
          SparseRow {
              .class_name = ClassName.full_name(),
              .constraint_name = GenerationName,
              .variable_uid = uid(),
              .context = block_context,
          }
              .equal(0);
      const auto ocol = fout_cols.at(buid);

      grow[ocol] = -stage_conversion_rate;
      grow[gcol] = +1;
      grows[buid] = lp.add_row(std::move(grow));
    }

    const auto dcol = demand_cols.at(buid);
    {
      const auto icol = finp_cols.at(buid);
      auto drow =
          SparseRow {
              .class_name = ClassName.full_name(),
              .constraint_name = DemandName,
              .variable_uid = uid(),
              .context = block_context,
          }
              .equal(0);
      drow[icol] = -stage_conversion_rate;
      drow[dcol] = +1;
      drows[buid] = lp.add_row(std::move(drow));
    }

    // adding the capacity constraint
    if (capacity_col) {
      auto crow =
          SparseRow {
              .class_name = ClassName.full_name(),
              .constraint_name = CapacityName,
              .variable_uid = uid(),
              .context = block_context,
          }
              .greater_equal(0);

      crow[*capacity_col] = 1;
      crow[gcol] = -1;
      crow[dcol] = -1;

      crows[buid] = lp.add_row(std::move(crow));
    }
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  capacity_rows[st_key] = std::move(crows);
  generation_rows[st_key] = std::move(grows);
  demand_rows[st_key] = std::move(drows);

  return true;
}

bool ConverterLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();

  out.add_row_dual(cname, GenerationName, id(), generation_rows);
  out.add_row_dual(cname, DemandName, id(), demand_rows);
  out.add_row_dual(cname, CapacityName, id(), capacity_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
