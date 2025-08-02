/**
 * @file      generator_profile_lp.cpp
 * @brief     Header of
 * @date      Tue Apr  1 22:03:50 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/generator_profile_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
GeneratorProfileLP::GeneratorProfileLP(GeneratorProfile pgenerator_profile,
                                       InputContext& ic)
    : ObjectLP<GeneratorProfile>(std::move(pgenerator_profile))
    , scost(ic, ClassName, id(), std::move(generator_profile().scost))
    , profile(ic, ClassName, id(), std::move(generator_profile().profile))
{
}

bool GeneratorProfileLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  constexpr std::string_view cname = ClassName;

  if (!is_active(stage)) {
    return true;
  }

  auto&& generator = sc.element<GeneratorLP>(generator_sid());
  if (!generator.is_active(stage)) {
    return true;
  }

  auto&& generation_cols = generator.generation_cols_at(scenario, stage);

  const auto [stage_capacity, capacity_col] =
      generator.capacity_and_col(stage, lp);

  if (!capacity_col && !generator.generator().capacity) {
    SPDLOG_WARN(
        "GeneratorProfile requires that Generator defines capacity or "
        "expansion");

    return false;
  }

  const auto stage_scost = scost.optval(stage.uid()).value_or(0.0);

  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> scols;
  scols.reserve(blocks.size());
  BIndexHolder<RowIndex> srows;
  srows.reserve(blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto gcol = generation_cols.at(buid);
    const auto block_profile =
        profile.at(scenario.uid(), stage.uid(), block.uid());

    const auto block_scost =
        sc.block_ecost(scenario, stage, block, stage_scost);
    auto name = sc.lp_label(scenario, stage, block, cname, "prof", uid());
    const auto scol = lp.add_col({.name = name, .cost = block_scost});
    scols[buid] = scol;

    SparseRow srow {.name = std::move(name)};
    srow[scol] = 1;
    srow[gcol] = 1;

    if (capacity_col) {
      srow[capacity_col.value()] = -block_profile;
      srows[buid] = lp.add_row(std::move(srow.greater_equal(0)));
    } else {
      const auto cprofile = stage_capacity * block_profile;
      srows[buid] = lp.add_row(std::move(srow.greater_equal(cprofile)));
    }
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  spillover_cols[st_key] = std::move(scols);
  spillover_rows[st_key] = std::move(srows);

  return true;
}

bool GeneratorProfileLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_col_sol(cname, "spillover", id(), spillover_cols);
  out.add_col_cost(cname, "spillover", id(), spillover_cols);
  out.add_row_dual(cname, "spillover", id(), spillover_rows);

  return true;
}

}  // namespace gtopt
