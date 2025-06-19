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

#include "gtopt/block.hpp"

namespace gtopt
{

DemandProfileLP::DemandProfileLP(InputContext& ic,
                                 DemandProfile pdemand_profile)
    : ObjectLP<DemandProfile>(std::move(pdemand_profile))
    , scost(ic, ClassName, id(), std::move(demand_profile().scost))
    , profile(ic, ClassName, id(), std::move(demand_profile().profile))
    , demand_index(ic.make_element_index<DemandLP>(demand_profile(), demand()))
{
}

bool DemandProfileLP::add_to_lp(const SystemContext& sc,
                                const ScenarioLP& scenario,
                                const StageLP& stage,
                                LinearProblem& lp)
{
  constexpr std::string_view cname = "dprof";

  const auto stage_index = stage.index();
  const auto scenario_index = scenario.index();

  if (!is_active(stage)) {
    return true;
  }

  auto&& demand_lp = sc.element(demand_index);
  if (!demand_lp.is_active(stage)) {
    return true;
  }

  auto&& load_cols = demand_lp.load_cols_at(scenario.uid(), stage.uid());

  const auto [stage_capacity, capacity_col] =
      demand_lp.capacity_and_col(stage, lp);

  if (!capacity_col && !demand_lp.demand().capacity) {
    SPDLOG_WARN("requires that Demand defines capacity or expansion");
    return false;
  }

  const auto stage_scost = scost.optval(stage.uid()).value_or(0.0);

  const auto& blocks = stage.blocks();

  BIndexHolder scols;
  scols.reserve(blocks.size());
  BIndexHolder srows;
  srows.reserve(blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto lcol = load_cols.at(buid);

    const auto block_profile =
        profile.at(scenario.uid(), stage.uid(), block.uid());

    const auto block_scost =
        sc.block_ecost(scenario, stage, block, stage_scost);
    auto name = sc.stb_label(scenario, stage, block, cname, "prof", uid());
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

  return emplace_bholder(scenario, stage, spillover_cols, std::move(scols))
             .second
      && emplace_bholder(scenario, stage, spillover_rows, std::move(srows))
             .second;
}

bool DemandProfileLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_col_sol(cname, "unserved", id(), spillover_cols);
  out.add_row_dual(cname, "unserved", id(), spillover_rows);

  return true;
}

}  // namespace gtopt
