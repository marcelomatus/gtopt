/**
 * @file      demand_profile_lp.cpp
 * @brief     Header of
 * @date      Sat Apr  5 23:12:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/demand_profile_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

DemandProfileLP::DemandProfileLP(DemandProfile pdemand_profile,
                                 InputContext& ic)
    : ObjectLP<DemandProfile>(std::move(pdemand_profile))
    , scost(ic, ClassName, id(), std::move(demand_profile().scost))
    , profile(ic, ClassName, id(), std::move(demand_profile().profile))
{
}

bool DemandProfileLP::add_to_lp(const SystemContext& sc,
                                const ScenarioLP& scenario,
                                const StageLP& stage,
                                LinearProblem& lp)
{
  constexpr std::string_view cname = ClassName;

  if (!is_active(stage)) {
    return true;
  }

  auto&& demand = sc.element<DemandLP>(demand_sid());
  if (!demand.is_active(stage)) {
    return true;
  }

  auto&& load_cols = demand.load_cols_at(scenario, stage);

  const auto [stage_capacity, capacity_col] =
      demand.capacity_and_col(stage, lp);

  if (!capacity_col && !demand.demand().capacity) {
    SPDLOG_WARN("requires that Demand defines capacity or expansion");
    return false;
  }

  const auto stage_scost = scost.optval(stage.uid()).value_or(0.0);

  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> scols;
  BIndexHolder<RowIndex> srows;
  scols.reserve(blocks.size());
  srows.reserve(blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto lcol = load_cols.at(buid);

    const auto block_profile =
        profile.at(scenario.uid(), stage.uid(), block.uid());

    const auto block_scost =
        sc.block_ecost(scenario, stage, block, stage_scost);
    auto name = sc.lp_label(scenario, stage, block, cname, "prof", uid());
    const auto scol = lp.add_col({.name = name, .cost = block_scost});
    scols[buid] = scol;

    auto srow = SparseRow {.name = std::move(name)};
    srow[scol] = 1;
    srow[lcol] = 1;

    if (capacity_col) {
      srow[*capacity_col] = -block_profile;
      srows[buid] = lp.add_row(std::move(srow.greater_equal(0)));
    } else {
      const auto cprofile = stage_capacity * block_profile;
      srows[buid] = lp.add_row(std::move(srow.greater_equal(cprofile)));
    }
  }

  // Store spillover columns and rows for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  spillover_cols[st_key] = std::move(scols);
  spillover_rows[st_key] = std::move(srows);

  return true;
}

bool DemandProfileLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_col_sol(cname, "unserved", id(), spillover_cols);
  out.add_row_dual(cname, "unserved", id(), spillover_rows);

  return true;
}

}  // namespace gtopt
