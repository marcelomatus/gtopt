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

#include <iostream>
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
bool BatteryLP::add_to_lp(const SystemContext& sc, LinearProblem& lp)
{
  constexpr std::string_view cname = "batt";

  // Add capacity-related variables and constraints
  if (!CapacityBase::add_to_lp(sc, lp, cname)) [[unlikely]] {
    return false;
  }

  const auto stage_index = sc.stage_index();
  if (!is_active(stage_index)) [[unlikely]] {
    return true;
  }

  // Get capacity information
  auto&& [stage_capacity, capacity_col] = capacity_and_col(sc, lp);

  // Get blocks for this stage
  auto&& blocks = sc.stage_blocks();

  // Create flow variables for each time block
  BIndexHolder fcols;
  fcols.reserve(blocks.size());

  // Use C++23 ranges to process blocks
  for (auto&& block :
       blocks
           | std::views::transform(
               [&](auto&& b) { return sc.stb_label(b, cname, "flow", uid()); }))
  {
    SparseCol fcol {.name = block};
    fcols.push_back(lp.add_col(std::move(fcol.free())));
  }

  // Add storage-specific constraints (energy balance, SOC limits, etc.)
  if (!StorageBase::add_to_lp(
          sc, lp, cname, fcols, stage_capacity, capacity_col)) [[unlikely]]
  {
    std::cerr << "Failed to add storage constraints for battery " << uid()
              << std::endl;
    return false;
  }

  // Store flow variable indices for later use
  return sc.emplace_bholder(flow_cols, std::move(fcols)).second;
}

/**
 * @brief Adds battery output results to the output context
 * @param out Output context to add results to
 * @return True if successful, false otherwise
 *
 * Processes optimization results for:
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
