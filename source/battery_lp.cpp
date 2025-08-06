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

#include <gtopt/battery_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

BatteryLP::BatteryLP(Battery pbattery, const InputContext& ic)
    : StorageBase(std::move(pbattery), ic, ClassName, ShortName)
{
}

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

bool BatteryLP::add_to_lp(SystemContext& sc,
                          const ScenarioLP& scenario,
                          const StageLP& stage,
                          LinearProblem& lp)
{
  static constexpr std::string_view cname = ShortName;

  // Add capacity-related variables and constraints
  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) [[unlikely]] {
    return false;
  }

  // Get capacity information
  auto&& [stage_capacity, capacity_col] = capacity_and_col(stage, lp);

  // Get blocks for this stage
  const auto& blocks = stage.blocks();

  // Create flow variables for each time block
  BIndexHolder<ColIndex> fcols;
  fcols.reserve(blocks.size());

  for (auto&& block : blocks) {
    const auto col = lp.add_col(SparseCol {
        .name = sc.lp_label(scenario, stage, block, cname, "flow", uid())}
                                    .free());
    fcols[block.uid()] = col;
  }

  // Add storage-specific constraints (energy balance, SOC limits, etc.)
  if (!StorageBase::add_to_lp(
          sc, scenario, stage, lp, cname, fcols, stage_capacity, capacity_col))
      [[unlikely]]
  {
    SPDLOG_CRITICAL(
        fmt::format("Failed to add storage constraints for battery {}", uid()));

    return false;
  }

  // Store flow variable indices for later use
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  flow_cols[st_key] = std::move(fcols);

  return true;
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
  static constexpr std::string_view cname = ClassName;

  // Add flow variable solutions and costs to output
  out.add_col_sol(cname, "flow", id(), flow_cols);
  out.add_col_cost(cname, "flow", id(), flow_cols);

  // Process storage and capacity outputs
  return StorageBase::add_to_output(out, cname)
      && CapacityBase::add_to_output(out);
}

}  // namespace gtopt
