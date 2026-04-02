/**
 * @file      right_junction_lp.cpp
 * @brief     Implementation of RightJunctionLP methods
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the LP formulation for water rights balance nodes.
 * Creates a balance row per block (analogous to JunctionLP) and
 * an optional drain column when excess supply is allowed.
 *
 * FlowRightLP adds its flow variables to these balance rows
 * when its right_junction field matches this RightJunction.
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/right_junction_lp.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

bool RightJunctionLP::add_to_lp(const SystemContext& sc,
                                const ScenarioLP& scenario,
                                const StageLP& stage,
                                LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return false;
  }

  BIndexHolder<RowIndex> brows;
  BIndexHolder<ColIndex> dcols;
  map_reserve(brows, blocks.size());
  map_reserve(dcols, blocks.size());

  const bool add_drain_col = drain();

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    auto brow = SparseRow {
        .name = sc.lp_row_label(scenario, stage, block, cname, "bal", uid()),
    };

    if (add_drain_col) {
      const auto dcol = lp.add_col({
          .name =
              sc.lp_col_label(scenario, stage, block, cname, "drain", uid()),
      });
      dcols[buid] = dcol;
      brow[dcol] = -1.0;
    }

    brows[buid] = lp.add_row(std::move(brow));
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  drain_cols[st_key] = std::move(dcols);
  balance_rows[st_key] = std::move(brows);

  return true;
}

bool RightJunctionLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, "drain", pid, drain_cols);
  out.add_col_cost(cname, "drain", pid, drain_cols);
  out.add_row_dual(cname, "balance", pid, balance_rows);

  return true;
}

}  // namespace gtopt
