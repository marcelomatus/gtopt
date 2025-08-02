/**
 * @file      turbine_lp.cpp
 * @brief     Implementation of TurbineLP class
 * @date      Thu Jul 31 02:05:38 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the TurbineLP class which provides the linear
 * programming representation of hydroelectric turbines, including their
 * constraints and relationships with other system components.
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
TurbineLP::TurbineLP(Turbine pturbine, InputContext& ic)
    : ObjectLP<Turbine>(std::move(pturbine))
    , conversion_rate(ic, ClassName, id(), std::move(turbine().conversion_rate))
{
}

/**
 * @brief Adds turbine constraints to the linear problem
 *
 * @param sc System context containing component relationships
 * @param scenario Current scenario being processed
 * @param stage Current stage being processed
 * @param lp Linear problem to add constraints to
 * @return true if successful, false on error
 *
 * Adds constraints that enforce the relationship between water flow through
 * the turbine and electrical power generation based on the conversion rate.
 */
bool TurbineLP::add_to_lp(const SystemContext& sc,
                          const ScenarioLP& scenario,
                          const StageLP& stage,
                          LinearProblem& lp)
{
  constexpr std::string_view cname = ClassName;
  if (!is_active(stage)) {
    return true;
  }

  const auto stage_conversion_rate =
      conversion_rate.at(stage.uid()).value_or(1.0);

  auto&& blocks = stage.blocks();

  const auto& generator = sc.element<GeneratorLP>(this->generator_sid());
  const auto& gen_cols = generator.generation_cols_at(scenario, stage);

  const auto& waterway = sc.element<WaterwayLP>(this->waterway_sid());
  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);

  BIndexHolder<RowIndex> rrows;
  rrows.reserve(blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const auto fcol = flow_cols.at(buid);
    const auto gcol = gen_cols.at(buid);

    auto rrow = SparseRow {.name = sc.lp_label(
                               scenario, stage, block, cname, "conv", uid())}
                    .equal(0);
    rrow[fcol] = -stage_conversion_rate;
    rrow[gcol] = 1;

    rrows[buid] = lp.add_row(std::move(rrow));
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  conversion_rows[st_key] = std::move(rrows);

  return true;
}

/**
 * @brief Adds turbine dual variables to the output context
 *
 * @param out Output context to write results to
 * @return true if successful, false on error
 *
 * Outputs the dual variables associated with the turbine's conversion
 * rate constraints for sensitivity analysis.
 */
bool TurbineLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_row_dual(cname, "conversion", id(), conversion_rows);

  return true;
}

}  // namespace gtopt
