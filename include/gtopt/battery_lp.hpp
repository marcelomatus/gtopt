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
  static constexpr std::string_view FinpName {"finp"};
  static constexpr std::string_view FoutName {"fout"};
  // User-facing attribute aliases used by PAMPL expression resolution.
  // "charge" resolves to finp_cols, "discharge" resolves to fout_cols.
  static constexpr std::string_view ChargeName {"charge"};
  static constexpr std::string_view DischargeName {"discharge"};
  /// Filter metadata key published by `add_to_lp` for `sum(...)`
  /// predicate matching.  Battery `bus` is optional, so only `type`
  /// is registered (see `add_to_lp` for the rationale).
  static constexpr std::string_view TypeKey {"type"};

  using CapacityBase = CapacityObjectLP<Battery>;
  using StorageBase = StorageLP<CapacityObjectLP<Battery>>;

  /**
   * @brief Access the underlying battery object (non-const)
   * @return Reference to the battery object
   */
  [[nodiscard]] constexpr auto&& battery(this auto&& self) noexcept
  {
    // Forward the object() call with same value category as self
    return self.object();
  }

  /**
   * @brief Constructs a BatteryLP from a Battery object
   * @tparam BatteryT Type of battery object (must satisfy BatteryLike concept)
   * @param ic Input context for parameter processing
   * @param pbattery Battery object to convert to LP representation
   */
  explicit BatteryLP(const Battery& pbattery, const InputContext& ic);

  /**
   * @brief Adds battery variables and constraints to the linear problem
   * @param sc       System context containing current state
   * @param scenario Current scenario LP object.
   * @param stage    Current stage LP object.
   * @param lp       Linear problem to add variables and constraints to
   */
  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  /**
   * @brief Adds battery output results to the output context
   * @param out Output context to add results to
   */
  bool add_to_output(OutputContext& out) const;

  /**
   * @brief Gets the flow variables for a specific scenario and stage
   * @return Reference to the flow variables
   */
  [[nodiscard]] constexpr auto&& finp_cols_at(const ScenarioLP& scenario,
                                              const StageLP& stage) const
  {
    return finp_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] constexpr auto&& fout_cols_at(const ScenarioLP& scenario,
                                              const StageLP& stage) const
  {
    return fout_cols.at({scenario.uid(), stage.uid()});
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_input_efficiency(StageUid s) const
  {
    return input_efficiency.at(s);
  }
  [[nodiscard]] auto param_output_efficiency(StageUid s) const
  {
    return output_efficiency.at(s);
  }
  /// @}

private:
  OptTRealSched input_efficiency;
  OptTRealSched output_efficiency;

  STBIndexHolder<ColIndex> finp_cols;
  STBIndexHolder<ColIndex> fout_cols;
};

// Pin the data-struct constant value so an accidental rename of the
// `Battery::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Battery"`).
static_assert(BatteryLP::Element::class_name == LPClassName {"Battery"},
              "Battery::class_name must remain \"Battery\"");

}  // namespace gtopt
