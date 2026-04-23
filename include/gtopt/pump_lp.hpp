/**
 * @file      pump_lp.hpp
 * @brief     Defines the PumpLP class for linear programming representation
 * @date      Sat Apr 12 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the PumpLP class which provides the linear programming
 * representation of a hydraulic pump, including its constraints and
 * relationships with waterways and demands.
 */
#pragma once

#include <cassert>

#include <gtopt/demand_lp.hpp>
#include <gtopt/pump.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes (system_lp.hpp includes
// pump_lp.hpp).
class SystemLP;

/// Single-ID alias for referencing a PumpLP in other LP elements
using PumpLPSId = ObjectSingleId<class PumpLP>;

/**
 * @brief Linear programming representation of a hydraulic pump
 *
 * This class extends ObjectLP to provide LP-specific functionality for
 * pumps, including:
 * - Conversion rate constraints between electrical power and water flow
 * - Relationships with connected waterways and demands
 * - Output of dual variables for sensitivity analysis
 *
 * The pump constraint links the demand's load column to the waterway's
 * flow column:
 *   pump_power [MW] >= pump_factor [MW/(m³/s)] × pump_flow [m³/s]
 *
 * Or equivalently (as an LP row):
 *   pump_factor × flow - load <= 0
 *
 * This ensures the pump consumes at least enough power to move the water.
 */
class PumpLP : public ObjectLP<Pump>
{
public:
  static constexpr LPClassName ClassName {"Pump"};
  static constexpr std::string_view ConversionName {"conversion"};
  static constexpr std::string_view CapacityName {"capacity"};

  /**
   * @brief Construct a PumpLP from a Pump and input context
   * @param ppump The pump to represent
   * @param ic Input context containing system configuration
   */
  explicit PumpLP(const Pump& ppump, InputContext& ic);

  [[nodiscard]] constexpr auto&& pump(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] auto waterway_sid() const -> WaterwayLPSId
  {
    return WaterwayLPSId {pump().waterway};
  }

  [[nodiscard]] constexpr auto demand_sid() const noexcept
  {
    return DemandLPSId {pump().demand};
  }

  /// @return The pump efficiency [p.u.] for the given stage (default 1.0)
  [[nodiscard]] auto stage_efficiency(StageUid tuid) const -> Real
  {
    return efficiency.at(tuid).value_or(1.0);
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Access conversion-rate constraint row indices for a (scenario, stage)
  [[nodiscard]] auto conversion_rows_at(const ScenarioLP& scenario,
                                        const StageLP& stage) const
      -> const BIndexHolder<RowIndex>&
  {
    return conversion_rows.at({scenario.uid(), stage.uid()});
  }

private:
  OptTRealSched pump_factor;
  OptTRealSched efficiency;
  OptTRealSched capacity;

  STBIndexHolder<RowIndex> conversion_rows;
  STBIndexHolder<RowIndex> capacity_rows;
};

}  // namespace gtopt
