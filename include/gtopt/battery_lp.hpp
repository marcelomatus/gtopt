/**
 * @file      battery_lp.hpp
 * @brief     Header for battery LP formulation
 * @date      Sun Apr 20 01:55:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the LP representation of batteries in power system
 * optimization.
 */

#pragma once

#include <gtopt/battery.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{
using BatteryLPId = ObjectId<class BatteryLP>;
using BatteryLPSId = ObjectSingleId<class BatteryLP>;

/**
 * @class BatteryLP
 * @brief Linear programming representation of a battery energy storage system
 *
 * This class provides the LP formulation for battery models, including:
 * - Energy balance constraints
 * - State of charge tracking
 * - Charge/discharge limits
 * - Capacity constraints
 */
class BatteryLP : public StorageLP<CapacityObjectLP<Battery>>
{
public:
  constexpr static std::string_view ClassName = "Battery";

  using CapacityBase = CapacityObjectLP<Battery>;
  using StorageBase = StorageLP<CapacityObjectLP<Battery>>;

  /**
   * @brief Access the underlying battery object (non-const)
   * @return Reference to the battery object
   */
  [[nodiscard]] constexpr auto&& battery() { return object(); }

  /**
   * @brief Access the underlying battery object (const)
   * @return Const reference to the battery object
   */
  [[nodiscard]] constexpr auto&& battery() const { return object(); }

  /**
   * @brief Constructs a BatteryLP from a Battery object
   * @tparam BatteryT Type of battery object (must satisfy BatteryLike concept)
   * @param ic Input context for parameter processing
   * @param pbattery Battery object to convert to LP representation
   */
  template<typename BatteryT>
  explicit BatteryLP(const InputContext& ic, BatteryT&& pbattery)
      : StorageBase(ic, ClassName, std::forward<BatteryT>(pbattery))
  {
  }

  /**
   * @brief Adds battery variables and constraints to the linear problem
   * @param sc System context containing current state
   * @param lp Linear problem to add variables and constraints to
   * @return True if successful, false otherwise
   */
  bool add_to_lp(const SystemContext& sc,
                 const ScenarioIndex& scenario_index,
                 const StageIndex& stage_index,
                 LinearProblem& lp);

  /**
   * @brief Adds battery output results to the output context
   * @param out Output context to add results to
   * @return True if successful, false otherwise
   */
  bool add_to_output(OutputContext& out) const;

  /**
   * @brief Gets the flow variables for a specific scenario and stage
   * @param scenary_index Scenario index
   * @param stage_index Stage index
   * @return Reference to the flow variables
   */
  [[nodiscard]] auto&& flow_cols_at(const ScenarioIndex scenary_index,
                                    const StageIndex stage_index) const
  {
    return flow_cols.at({scenary_index, stage_index});
  }

private:
  STBIndexHolder flow_cols;  ///< Holds flow variables by scenario and stage
};

}  // namespace gtopt
