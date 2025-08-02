/**
 * @file      battery_lp.hpp
 * @brief     Header for battery LP formulation
 * @date      Sun Apr 20 01:55:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the LP representation of batteries in power system
 * planning.
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
  [[nodiscard]] constexpr auto&& battery(this auto&& self) noexcept
  {
    // Forward the object() call with same value category as self
    return std::forward_like<decltype(self)>(self.object());
  }

  /**
   * @brief Constructs a BatteryLP from a Battery object
   * @tparam BatteryT Type of battery object (must satisfy BatteryLike concept)
   * @param ic Input context for parameter processing
   * @param pbattery Battery object to convert to LP representation
   */
  explicit BatteryLP(Battery pbattery, const InputContext& ic)
      : StorageBase(std::move(pbattery), ic, ClassName)
  {
  }

  /**
   * @brief Adds battery variables and constraints to the linear problem
   * @param sc System context containing current state
   * @param lp Linear problem to add variables and constraints to
   * @return True if successful, false otherwise
   */
  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  /**
   * @brief Adds battery output results to the output context
   * @param out Output context to add results to
   * @return True if successful, false otherwise
   */
  bool add_to_output(OutputContext& out) const;

  /**
   * @brief Gets the flow variables for a specific scenario and stage
   * @return Reference to the flow variables
   */
  [[nodiscard]] constexpr auto&& flow_cols_at(const ScenarioLP& scenario,
                                              const StageLP& stage) const
  {
    return flow_cols.at({scenario.uid(), stage.uid()});
  }

private:
  STBIndexHolder<ColIndex> flow_cols;  ///< Holds flow variables
};

}  // namespace gtopt
