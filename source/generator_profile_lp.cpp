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

#include "gtopt/block.hpp"

namespace gtopt
{
GeneratorProfileLP::GeneratorProfileLP(InputContext& ic,
                                       GeneratorProfile pgenerator_profile)
    : ObjectLP<GeneratorProfile>(std::move(pgenerator_profile))
    , scost(ic, ClassName, id(), std::move(generator_profile().scost))
    , profile(ic, ClassName, id(), std::move(generator_profile().profile))
    , generator_index(
          ic.make_element_index<GeneratorLP>(generator_profile(), generator()))
{
}

bool GeneratorProfileLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioIndex& scenario_index,
                                   const StageIndex& stage_index,
                                   LinearProblem& lp)
{
  constexpr std::string_view cname = "gprof";

  if (!is_active(stage_index)) {
    return true;
  }

  auto&& generator_lp = sc.element(generator_index);
  if (!generator_lp.is_active(stage_index)) {
    return true;
  }

  auto&& generation_cols =
      generator_lp.generation_cols_at(scenario_index, stage_index);

  const auto [stage_capacity, capacity_col] =
      generator_lp.capacity_and_col(stage_index, lp);

  if (!capacity_col && !generator_lp.generator().capacity) {
    SPDLOG_WARN(
        "GeneratorProfile requires that Generator defines capacity or "
        "expansion");

    return false;
  }

  const auto stage_scost = scost.optval(stage_index).value_or(0.0);

  const auto& blocks = sc.stage_blocks(stage_index);

  BIndexHolder scols;
  scols.reserve(blocks.size());
  BIndexHolder srows;
  srows.reserve(blocks.size());

  for (const auto& [block_index, block, gcol] :
       enumerate<BlockIndex>(blocks, generation_cols))
  {
    const auto block_profile =
        profile.at(scenario_index, stage_index, block_index);

    const auto block_scost =
        sc.block_ecost(scenario_index, stage_index, block, stage_scost);
    auto name =
        sc.stb_label(scenario_index, stage_index, block, cname, "prof", uid());
    const auto scol = lp.add_col({.name = name, .cost = block_scost});
    scols.push_back(scol);

    SparseRow srow {.name = std::move(name)};
    srow[scol] = 1;
    srow[gcol] = 1;

    if (capacity_col) {
      srow[capacity_col.value()] = -block_profile;
      srows.push_back(lp.add_row(std::move(srow.greater_equal(0))));
    } else {
      const auto cprofile = stage_capacity * block_profile;
      srows.push_back(lp.add_row(std::move(srow.greater_equal(cprofile))));
    }
  }

  return emplace_bholder(
             scenario_index, stage_index, spillover_cols, std::move(scols))
             .second
      && emplace_bholder(
             scenario_index, stage_index, spillover_rows, std::move(srows))
             .second;
}

bool GeneratorProfileLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_col_sol(cname, "spillover", id(), spillover_cols);
  out.add_row_dual(cname, "spillover", id(), spillover_rows);

  return true;
}

}  // namespace gtopt
