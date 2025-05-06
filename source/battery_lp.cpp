/**
 * @file      battery_lp.cpp
 * @brief     Implementation of BatteryLP class for battery LP formulation
 * @date      Wed Apr  2 02:19:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the BatteryLP class, which handles the representation
 * of battery energy storage systems in linear programming problems. It includes
 * methods to create variables for energy flows, state of charge tracking, and
 * capacity constraints.
 */

#include <ranges>

#include <gtopt/battery_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

/**
 * @brief Adds battery variables and constraints to the linear problem
 * @param sc System context containing current state
 * @param lp Linear problem to add variables and constraints to
 * @return True if successful, false otherwise
 *
 * This method creates:
 * 1. Flow variables for each time block (can be positive for discharge or
 * negative for charge)
 * 2. State of charge tracking constraints between time blocks
 * 3. Capacity constraints linking battery operation to installed capacity
 */
bool BatteryLP::add_to_lp(const SystemContext& sc,
                          const ScenarioIndex& scenario_index,
                          const StageIndex& stage_index,
                          LinearProblem& lp)
{
  constexpr std::string_view cname = "batt";

  // Add capacity-related variables and constraints
  if (!CapacityBase::add_to_lp(sc, scenario_index, stage_index, lp, cname))
      [[unlikely]]
  {
    return false;
  }

  if (!is_active(stage_index)) [[unlikely]] {
    return true;
  }

  // Get capacity information
  auto&& [stage_capacity, capacity_col] = capacity_and_col(stage_index, lp);

  // Get blocks for this stage
  const auto& blocks = sc.stage_blocks(stage_index);

  // Create flow variables for each time block
  BIndexHolder fcols;
  fcols.reserve(blocks.size());

  // Use C++23 ranges to process blocks
  for (auto&& name :
       blocks
           | std::views::transform(
               [&](const auto& b)
               {
                 return sc.stb_label(
                     scenario_index, stage_index, b, cname, "flow", uid());
               }))
  {
    SparseCol fcol {.name = name};
    fcols.push_back(lp.add_col(std::move(fcol.free())));
  }

  // Add storage-specific constraints (energy balance, SOC limits, etc.)
  if (!StorageBase::add_to_lp(sc,
                              scenario_index,
                              stage_index,
                              lp,
                              cname,
                              fcols,
                              stage_capacity,
                              capacity_col)) [[unlikely]]
  {
    SPDLOG_CRITICAL(
        fmt::format("Failed to add storage constraints for battery {}", uid()));

    return false;
  }

  // Store flow variable indices for later use
  return emplace_bholder(
             scenario_index, stage_index, flow_cols, std::move(fcols))
      .second;
}

/**
 * @brief Adds battery output results to the output context
 * @param out Output context to add results to
 * @return True if successful, false otherwise
 *
 * Processes planning results for:
 * - Flow variables (charge/discharge decisions)
 * - Storage levels
 * - Capacity-related outputs
 */
bool BatteryLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  // Add flow variable solutions and costs to output
  out.add_col_sol(cname, "flow", id(), flow_cols);
  out.add_col_cost(cname, "flow", id(), flow_cols);

  // Process storage and capacity outputs
  return StorageBase::add_to_output(out, cname)
      && CapacityBase::add_to_output(out, cname);
}

}  // namespace gtopt
