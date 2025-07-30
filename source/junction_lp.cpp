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
  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();

  // Reserve space for balance rows and drain columns
  BIndexHolder<RowIndex> brows;
  brows.reserve(blocks.size());

  BIndexHolder<ColIndex> dcols;
  dcols.reserve(blocks.size());

  const bool add_drain_col = drain();
  for (auto&& block : blocks) {
    const auto buid = block.uid();
    auto brow =
        SparseRow {.name = sc.lp_label(
                       scenario, stage, block, class_name(), "bal", uid())};

    if (add_drain_col) {
      const auto dcol = lp.add_col(
          {.name = sc.lp_label(
               scenario, stage, block, class_name(), "drain", uid())});
      dcols[buid] = dcol;
      brow[dcol] = -1;
    }

    brows[buid] = lp.add_row(std::move(brow));
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  drain_cols[st_key] = std::move(dcols);
  balance_rows[st_key] = std::move(brows);

  return true;
}

bool JunctionLP::add_to_output(OutputContext& out) const
{
  const auto pid = id();

  out.add_col_sol(class_name(), "drain", pid, drain_cols);
  out.add_col_cost(class_name(), "drain", pid, drain_cols);
  out.add_row_dual(class_name(), "balance", pid, balance_rows);

  return true;
}

}  // namespace gtopt
