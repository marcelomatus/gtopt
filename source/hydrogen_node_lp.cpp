/**
 * @file      hydrogen_node_lp.cpp
 * @brief     Implementation of HydrogenNodeLP balance row
 * @date      Sat May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <gtopt/hydrogen_node_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

bool HydrogenNodeLP::add_to_lp([[maybe_unused]] const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return false;
  }

  BIndexHolder<RowIndex> brows;
  map_reserve(brows, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    auto brow = SparseRow {
        .class_name = Element::class_name.full_name(),
        .constraint_name = BalanceName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    };
    brows[buid] = lp.add_row(std::move(brow));
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  balance_rows[st_key] = std::move(brows);

  return true;
}

bool HydrogenNodeLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  out.add_row_dual(cname, BalanceName, id(), balance_rows);
  return true;
}

}  // namespace gtopt
