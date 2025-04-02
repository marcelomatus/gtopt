/**
 * @file      battery_lp.cpp
 * @brief     Header of
 * @date      Wed Apr  2 02:19:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/battery_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

bool BatteryLP::add_to_lp(const SystemContext& sc, LinearProblem& lp)
{
  constexpr std::string_view cname = "batt";

  if (!CapacityBase::add_to_lp(sc, lp, cname)) {
    return false;
  }

  const auto stage_index = sc.stage_index();
  if (!is_active(stage_index)) [[unlikely]] {
    return true;
  }

  auto&& [stage_capacity, capacity_col] = capacity_and_col(sc, lp);

  auto&& blocks = sc.stage_blocks();

  BIndexHolder fcols;
  fcols.reserve(blocks.size());
  for (auto&& block : blocks) {
    SparseCol fcol {.name = sc.stb_label(block, cname, "flow", uid())};

    fcols.push_back(lp.add_col(std::move(fcol.free())));
  }

  if (!StorageBase::add_to_lp(
          sc, lp, cname, fcols, stage_capacity, capacity_col))
  {
    return false;
  }

  return sc.emplace_bholder(flow_cols, std::move(fcols)).second;
}

bool BatteryLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_col_sol(cname, "flow", id(), flow_cols);
  out.add_col_cost(cname, "flow", id(), flow_cols);

  return StorageBase::add_to_output(out, cname)
      && CapacityBase::add_to_output(out, cname);
}

}  // namespace gtopt
