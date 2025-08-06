/**
 * @file      junction_lp.cpp
 * @brief     Implementation of junction LP formulation
 * @date      Tue Jul 29 23:08:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This file implements the linear programming formulation for power system
 * junctions, including flow balance constraints and drain effects.
 */

#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

bool JunctionLP::add_to_lp(const SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp)
{
  static constexpr std::string_view cname = ShortName;

  // Skip inactive junctions for this stage
  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return false;
  }

  // Reserve space for balance rows and drain columns
  BIndexHolder<RowIndex> brows;
  BIndexHolder<ColIndex> dcols;
  brows.reserve(blocks.size());
  dcols.reserve(blocks.size());

  const bool add_drain_col = drain();

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    // Create balance row for this block
    auto brow = SparseRow {
        .name = sc.lp_label(scenario, stage, block, cname, "bal", uid())};

    // Add drain column if needed
    if (add_drain_col) {
      const auto dcol = lp.add_col(
          {.name = sc.lp_label(scenario, stage, block, cname, "drain", uid())});
      dcols[buid] = dcol;
      brow[dcol] = -1.0;  // Drain coefficient
    }

    brows[buid] = lp.add_row(std::move(brow));
  }

  // Store indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  drain_cols[st_key] = std::move(dcols);
  balance_rows[st_key] = std::move(brows);

  return true;
}

bool JunctionLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName;
  const auto pid = id();

  // Add all solution components to output context
  out.add_col_sol(cname, "drain", pid, drain_cols);
  out.add_col_cost(cname, "drain", pid, drain_cols);
  out.add_row_dual(cname, "balance", pid, balance_rows);

  return true;
}

}  // namespace gtopt
