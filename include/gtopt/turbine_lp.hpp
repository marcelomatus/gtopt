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

#include <gtopt/generator_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

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
  constexpr static std::string_view ClassName = "Turbine";

  /**
   * @brief Construct a TurbineLP from a Turbine and input context
   * @param pturbine The turbine to represent
   * @param ic Input context containing system configuration
   */
  explicit TurbineLP(Turbine pturbine, InputContext& ic);

  [[nodiscard]] constexpr auto&& turbine(this auto&& self) noexcept
  {
    return std::forward_like<decltype(self)>(self.object());
  }

  [[nodiscard]] constexpr auto waterway_sid() const noexcept
  {
    return WaterwayLPSId {turbine().waterway};
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {turbine().generator};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched conversion_rate;

  STBIndexHolder<RowIndex> conversion_rows;
};

}  // namespace gtopt
