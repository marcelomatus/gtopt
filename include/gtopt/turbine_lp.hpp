/**
 * @file      turbine_lp.hpp
 * @brief     Defines the TurbineLP class for linear programming representation
 * @date      Thu Jul 31 01:50:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the TurbineLP class which provides the linear programming
 * representation of a hydroelectric turbine, including its constraints and
 * relationships with waterways and generators.
 */
#pragma once

#include <cassert>

#include <gtopt/flow_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes (system_lp.hpp includes
// turbine_lp.hpp).
class SystemLP;

/// Single-ID alias for referencing a TurbineLP in other LP elements
using TurbineLPSId = ObjectSingleId<class TurbineLP>;

/**
 * @brief Linear programming representation of a hydroelectric turbine
 *
 * This class extends ObjectLP to provide LP-specific functionality for
 * turbines, including:
 * - Conversion rate constraints between water flow and power generation
 * - Relationships with connected waterways and generators
 * - Output of dual variables for sensitivity analysis
 */
class TurbineLP : public ObjectLP<Turbine>
{
public:
  static constexpr LPClassName ClassName {"Turbine"};
  static constexpr std::string_view ConversionName {"conversion"};
  static constexpr std::string_view CapacityName {"capacity"};

  /**
   * @brief Construct a TurbineLP from a Turbine and input context
   * @param pturbine The turbine to represent
   * @param ic Input context containing system configuration
   */
  explicit TurbineLP(const Turbine& pturbine, InputContext& ic);

  [[nodiscard]] constexpr auto&& turbine(this auto&& self) noexcept
  {
    return self.object();
  }

  /// @return Whether this turbine uses a flow reference (not a waterway)
  [[nodiscard]] constexpr bool uses_flow() const noexcept
  {
    return turbine().flow.has_value();
  }

  [[nodiscard]] auto waterway_sid() const noexcept -> WaterwayLPSId
  {
    const auto& opt = turbine().waterway;
    assert(opt.has_value()
           && "waterway_sid() called on a Turbine without a waterway");
    return WaterwayLPSId {*opt};
  }

  [[nodiscard]] auto flow_sid() const noexcept -> FlowLPSId
  {
    const auto& opt = turbine().flow;
    assert(opt.has_value() && "flow_sid() called on a Turbine without a flow");
    return FlowLPSId {*opt};
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {turbine().generator};
  }

  /// @return Whether this turbine has drainage enabled
  [[nodiscard]] constexpr auto drain() const noexcept
  {
    return turbine().drain.value_or(false);
  }

  /// @return The turbine efficiency [p.u.] for the given stage (default 1.0)
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
  OptTRealSched production_factor;
  OptTRealSched efficiency;
  OptTRealSched capacity;

  STBIndexHolder<RowIndex> conversion_rows;
  STBIndexHolder<RowIndex> capacity_rows;
};

}  // namespace gtopt
