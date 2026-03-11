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
  static constexpr LPClassName ClassName {"Turbine", "tur"};

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

  [[nodiscard]] constexpr auto waterway_sid() const noexcept
  {
    return WaterwayLPSId {turbine().waterway};
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

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /**
   * @brief Update reservoir-dependent LP coefficients for this turbine.
   *
   * Finds the ReservoirEfficiencyLP element that references this turbine,
   * queries the associated reservoir for the current volume (using the
   * previous phase's solution if available, or the initial volume for the
   * first phase / first iteration), and updates the turbine conversion-rate
   * coefficient in the LP via ReservoirEfficiencyLP::update_conversion_coeff.
   *
   * Respects the per-element skip count from ReservoirEfficiencyLP.
   *
   * @param sys        SystemLP that owns this turbine (non-const for coeff
   * update)
   * @param scenario   Current scenario
   * @param stage      Current stage
   * @param phase      Current phase (PhaseIndex{0} = first phase = use eini)
   * @param iteration  Current SDDP iteration (1-based; 0 = initialisation)
   * @return Number of LP coefficients updated
   */
  int update_lp(SystemLP& sys,
                const ScenarioLP& scenario,
                const StageLP& stage,
                PhaseIndex phase,
                int iteration);

  /// Access conversion-rate constraint row indices for a (scenario, stage)
  [[nodiscard]] auto conversion_rows_at(const ScenarioLP& scenario,
                                        const StageLP& stage) const
      -> const BIndexHolder<RowIndex>&
  {
    return conversion_rows.at({scenario.uid(), stage.uid()});
  }

private:
  OptTRealSched conversion_rate;
  OptTRealSched capacity;

  STBIndexHolder<RowIndex> conversion_rows;
  STBIndexHolder<RowIndex> capacity_rows;
};

}  // namespace gtopt
