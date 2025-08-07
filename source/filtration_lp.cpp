/**
 * @file      filtration_lp.cpp
 * @brief     Implementation of FiltrationLP methods
 * @date      Thu Jul 31 23:33:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the linear programming formulation for filtration systems.
 */

#include <gtopt/filtration_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
bool FiltrationLP::add_to_lp(const SystemContext& sc,
                             const ScenarioLP& scenario,
                             const StageLP& stage,
                             LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& waterway = sc.element<WaterwayLP>(waterway_sid());
  const auto& reservoir = sc.element<ReservoirLP>(reservoir_sid());

  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);
  const auto vini_col = reservoir.vini_col_at(scenario, stage);
  const auto vfin_col = reservoir.vfin_col_at(scenario, stage);

  const auto& blocks = stage.blocks();

  BIndexHolder<RowIndex> frows;
  BIndexHolder<ColIndex> fcols;
  frows.reserve(blocks.size());
  fcols.reserve(blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const auto fcol = flow_cols.at(buid);

    auto frow = SparseRow {.name = sc.lp_label(
                               scenario, stage, block, cname, "filt", uid())}
                    .equal(constant());

    frow[vini_col] = frow[vfin_col] = -slope() * 0.5;

    frow[fcol] = 1;

    frows[buid] = lp.add_row(std::move(frow));
    fcols[buid] = fcol;
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  filtration_rows[st_key] = std::move(frows);
  filtration_cols[st_key] = std::move(fcols);

  return true;
}

bool FiltrationLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, "filtration", pid, filtration_cols);
  out.add_col_cost(cname, "filtration", pid, filtration_cols);
  out.add_row_dual(cname, "filtration", pid, filtration_rows);

  return true;
}

}  // namespace gtopt
